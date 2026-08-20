/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::{FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize};
use crate::painting::paintable_data::*;
use std::cell::Cell;
use std::ffi::c_void;
use std::rc::Rc;

pub(crate) const PAINTABLE_SLOTS_PER_CHUNK: usize = 64;

#[repr(align(64))]
struct Chunk {
    slots: [Cell<PaintableData>; PAINTABLE_SLOTS_PER_CHUNK],
}

fn new_chunk() -> Box<Chunk> {
    // SAFETY: Every slot is written with PaintableData::default() before the chunk is exposed;
    // the chunk is built in place on the heap because it is too large for the stack.
    unsafe {
        let mut chunk = Box::<Chunk>::new_uninit();
        let slots = &raw mut (*chunk.as_mut_ptr()).slots;
        for offset in 0..PAINTABLE_SLOTS_PER_CHUNK {
            (&raw mut (*slots)[offset]).write(Cell::new(PaintableData::default()));
        }
        chunk.assume_init()
    }
}

#[derive(Default)]
pub struct PaintableArena {
    chunks: Vec<Box<Chunk>>,
    side_data: Vec<PaintableSideData>,
    pub(crate) stacking_context_tree: Option<crate::painting::stacking_context::StackingContextTree>,
    pub(crate) visual_context: crate::painting::visual_context::VisualContextState,
    pub(crate) hit_test_list: Option<crate::painting::hit_test::HitTestList>,
    pub(crate) hit_test_list_generation: u64,
    pub(crate) last_recording: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) paint_command_cache_source: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) hit_test_item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    pub(crate) paint_caches: std::cell::RefCell<Vec<Option<Box<crate::painting::record::cache::PaintCache>>>>,
    absolute_rect_memo: std::cell::RefCell<std::collections::HashMap<u32, crate::css::css_pixels::CssPixelRect>>,
}

impl PaintableArena {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn memoized_absolute_rect(&self, id: PaintableSlotId) -> Option<crate::css::css_pixels::CssPixelRect> {
        self.absolute_rect_memo.borrow().get(&id.index).copied()
    }

    pub fn memoize_absolute_rect(&self, id: PaintableSlotId, rect: crate::css::css_pixels::CssPixelRect) {
        self.absolute_rect_memo.borrow_mut().insert(id.index, rect);
    }

    pub fn clear_absolute_rect_memo(&self) {
        self.absolute_rect_memo.borrow_mut().clear();
    }

    pub fn row_for_node(&mut self, layout_node: NodeSlotId, shell: *mut c_void) -> PaintableAllocation {
        let index = layout_node.slot_index();
        assert!(
            index < MAX_PAINTABLE_SLOT_COUNT,
            "paintable arena exhausted its 24-bit slot index space"
        );
        while self.side_data.len() <= index as usize {
            if self.side_data.len().is_multiple_of(PAINTABLE_SLOTS_PER_CHUNK) {
                self.chunks.push(new_chunk());
            }
            self.side_data.push(PaintableSideData::default());
            self.paint_caches.get_mut().push(None);
        }

        let generation = layout_node.generation();
        let data = self.data_mut_by_index(index);
        *data = PaintableData::default();
        data.slot_generation = generation;
        data.layout_node = layout_node;
        data.shell = shell;
        self.side_data[index as usize] = PaintableSideData::default();
        self.paint_caches.get_mut()[index as usize] = None;

        PaintableAllocation {
            slot: PaintableSlotId::new(index, generation),
            data: self.data_mut_by_index(index),
            generation: u32::from(generation),
        }
    }

    pub fn shell_destroyed(&mut self, id: PaintableSlotId, generation: u32, shell: *mut c_void) {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        assert_eq!(
            u32::from(id.generation()),
            generation,
            "paintable arena slot ID and allocation generation disagree"
        );
        if !self.is_live(id) || self.data_ref(id).shell != shell {
            return;
        }
        self.reset_row(id);
    }

    fn reset_row(&mut self, id: PaintableSlotId) {
        self.absolute_rect_memo.get_mut().clear();
        self.remove_from_tree(id);
        while let Some(child) = self.first_child(id) {
            self.remove_from_tree(child);
        }
        let index = id.slot_index();
        *self.data_mut_by_index(index) = PaintableData::default();
        self.side_data[index as usize] = PaintableSideData::default();
        self.paint_caches.get_mut()[index as usize] = None;
    }

    pub fn layout_node_freed(&mut self, layout_slot_index: u32) {
        if layout_slot_index as usize >= self.side_data.len() {
            return;
        }
        let generation = self.data_by_index(layout_slot_index).slot_generation;
        if generation == 0 {
            return;
        }
        self.reset_row(PaintableSlotId::new(layout_slot_index, generation));
    }

    pub fn node_cleared(&mut self, layout_node: NodeSlotId, id: PaintableSlotId) {
        if self.paintable_of_node(layout_node) != id {
            return;
        }
        self.reset_row(id);
    }

    pub fn is_live(&self, id: PaintableSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        let index = id.slot_index() as usize;
        if index >= self.side_data.len() {
            return false;
        }
        let generation = self.data_by_index(index as u32).slot_generation;
        generation != 0 && generation == id.generation()
    }

    fn data_by_index(&self, index: u32) -> PaintableData {
        let chunk = self
            .chunks
            .get(index as usize / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot index");
        chunk.slots[index as usize % PAINTABLE_SLOTS_PER_CHUNK].get()
    }

    fn data_cell(&self, id: PaintableSlotId) -> &Cell<PaintableData> {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let index = id.slot_index() as usize;
        let chunk = self
            .chunks
            .get(index / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot ID");
        let data = &chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK];
        let generation = data.get().slot_generation;
        assert_eq!(
            generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        data
    }

    pub fn data_ptr(&self, id: PaintableSlotId) -> *mut PaintableData {
        self.data_cell(id).as_ptr()
    }

    pub fn data_ref(&self, id: PaintableSlotId) -> PaintableData {
        self.data_cell(id).get()
    }

    pub fn update_data<R>(&self, id: PaintableSlotId, callback: impl FnOnce(&mut PaintableData) -> R) -> R {
        let cell = self.data_cell(id);
        let mut data = cell.get();
        let result = callback(&mut data);
        cell.set(data);
        result
    }

    fn data_mut_by_index(&mut self, index: u32) -> &mut PaintableData {
        let index = index as usize;
        let chunk = self
            .chunks
            .get_mut(index / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot ID");
        chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK].get_mut()
    }

    pub fn invalidate_paint_cache(&self, id: PaintableSlotId) {
        if !self.is_live(id) {
            return;
        }
        if let Some(entry) = self.paint_caches.borrow_mut().get_mut(id.slot_index() as usize) {
            *entry = None;
        }
    }

    pub fn side(&self, id: PaintableSlotId) -> &PaintableSideData {
        debug_assert!(self.is_live(id));
        &self.side_data[id.slot_index() as usize]
    }

    pub fn side_mut(&mut self, id: PaintableSlotId) -> &mut PaintableSideData {
        debug_assert!(self.is_live(id));
        &mut self.side_data[id.slot_index() as usize]
    }

    pub fn paintable_of_node(&self, layout_node: NodeSlotId) -> PaintableSlotId {
        if layout_node.is_invalid() {
            return PaintableSlotId::INVALID;
        }
        let paintable = PaintableSlotId {
            index: layout_node.index,
        };
        if !self.is_live(paintable) {
            return PaintableSlotId::INVALID;
        }
        paintable
    }

    pub fn parent(&self, id: PaintableSlotId) -> Option<PaintableSlotId> {
        let parent = self.data_ref(id).parent;
        (!parent.is_invalid()).then_some(parent)
    }

    pub fn first_child(&self, id: PaintableSlotId) -> Option<PaintableSlotId> {
        let child = self.data_ref(id).first_child;
        (!child.is_invalid()).then_some(child)
    }

    pub fn next_sibling(&self, id: PaintableSlotId) -> Option<PaintableSlotId> {
        let sibling = self.data_ref(id).next_sibling;
        (!sibling.is_invalid()).then_some(sibling)
    }

    pub fn insert_before(&self, parent: PaintableSlotId, child: PaintableSlotId, before: PaintableSlotId) {
        assert!(
            self.data_ref(child).parent.is_invalid(),
            "paintable inserted while still parented"
        );
        if before.is_invalid() {
            self.append_child(parent, child);
            return;
        }
        assert_eq!(
            self.data_ref(before).parent,
            parent,
            "insert_before sibling is not a child of parent"
        );
        let prev = self.data_ref(before).prev_sibling;
        self.update_data(child, |child_data| {
            child_data.parent = parent;
            child_data.next_sibling = before;
            child_data.prev_sibling = prev;
        });
        self.update_data(before, |data| data.prev_sibling = child);
        if prev.is_invalid() {
            self.update_data(parent, |data| data.first_child = child);
        } else {
            self.update_data(prev, |data| data.next_sibling = child);
        }
    }

    pub fn append_child(&self, parent: PaintableSlotId, child: PaintableSlotId) {
        assert!(
            self.data_ref(child).parent.is_invalid(),
            "paintable appended while still parented"
        );
        let last = self.data_ref(parent).last_child;
        self.update_data(child, |child_data| {
            child_data.parent = parent;
            child_data.prev_sibling = last;
            child_data.next_sibling = PaintableSlotId::INVALID;
        });
        if last.is_invalid() {
            self.update_data(parent, |data| data.first_child = child);
        } else {
            self.update_data(last, |data| data.next_sibling = child);
        }
        self.update_data(parent, |data| data.last_child = child);
    }

    pub fn remove_from_tree(&self, id: PaintableSlotId) {
        let (parent, prev, next) = {
            let data = self.data_ref(id);
            (data.parent, data.prev_sibling, data.next_sibling)
        };
        if parent.is_invalid() {
            return;
        }
        if prev.is_invalid() {
            self.update_data(parent, |data| data.first_child = next);
        } else {
            self.update_data(prev, |data| data.next_sibling = next);
        }
        if next.is_invalid() {
            self.update_data(parent, |data| data.last_child = prev);
        } else {
            self.update_data(next, |data| data.prev_sibling = prev);
        }
        self.update_data(id, |data| {
            data.parent = PaintableSlotId::INVALID;
            data.prev_sibling = PaintableSlotId::INVALID;
            data.next_sibling = PaintableSlotId::INVALID;
        });
    }

    pub fn reset_for_relayout(&mut self, id: PaintableSlotId) {
        self.absolute_rect_memo.get_mut().clear();
        self.remove_from_tree(id);
        while let Some(child) = self.first_child(id) {
            self.remove_from_tree(child);
        }
        self.update_data(id, |data| {
            data.containing_block = PaintableSlotId::INVALID;
            data.offset = FfiCssPixelPoint::default();
            data.content_size = FfiCssPixelSize::default();
            data.margin = FfiPixelBox::default();
            data.border = FfiPixelBox::default();
            data.padding = FfiPixelBox::default();
            data.inset = FfiPixelBox::default();
            data.overflow = FfiOverflowData::default();
            data.has_overflow = false;
            data.containing_line_box_index = 0;
            data.has_containing_line_box_index = false;
            data.uses_collapsing_borders_model = false;
            data.sticky_insets = FfiStickyInsets::default();
            data.has_sticky_insets = false;
            data.local_padding_box_union = FfiCssPixelRect::default();
            data.local_border_box_union = FfiCssPixelRect::default();
            data.stacking_context = crate::painting::stacking_context::NO_STACKING_CONTEXT;
            data.enclosing_scroll_node_index = 0;
            data.own_scroll_node_index = 0;
            data.has_accumulated_visual_context = false;
            data.accumulated_visual_context_index = 0;
            data.accumulated_visual_context_for_descendants_index = 0;
            data.fixed_background_visual_context = 0;
            data.has_fixed_background_visual_context = false;
            data.svg_viewport_transform = crate::layout::FfiAffineTransform::default();
            data.has_svg_viewport_transform = false;
        });
        self.paint_caches.get_mut()[id.slot_index() as usize] = None;
        self.side_mut(id).reset_for_relayout();
    }
}
