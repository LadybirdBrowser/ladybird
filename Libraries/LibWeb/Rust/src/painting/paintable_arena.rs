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

#[derive(Clone, Copy, Default)]
struct SlotMetadata {
    generation: u8,
    occupied: bool,
}

#[derive(Default)]
pub struct PaintableArena {
    chunks: Vec<Box<Chunk>>,
    slot_metadata: Vec<SlotMetadata>,
    side_data: Vec<PaintableSideData>,
    free_list: Vec<u32>,
    next_index: u32,
    live_count: u32,
    paintable_of_node: Vec<PaintableSlotId>,
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

    pub fn live_count(&self) -> u32 {
        self.live_count
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

    pub fn allocate(&mut self, layout_node: NodeSlotId, shell: *mut c_void) -> PaintableAllocation {
        let index = if let Some(index) = self.free_list.pop() {
            index
        } else {
            let index = self.next_index;
            assert!(
                index < MAX_PAINTABLE_SLOT_COUNT,
                "paintable arena exhausted its 24-bit slot index space"
            );
            if (index as usize).is_multiple_of(PAINTABLE_SLOTS_PER_CHUNK) {
                self.chunks.push(new_chunk());
            }
            self.slot_metadata.push(SlotMetadata::default());
            self.side_data.push(PaintableSideData::default());
            self.paint_caches.get_mut().push(None);
            self.next_index = self
                .next_index
                .checked_add(1)
                .expect("paintable arena exhausted its slot ID space");
            index
        };
        self.live_count = self
            .live_count
            .checked_add(1)
            .expect("paintable arena live count overflowed");

        let metadata = &mut self.slot_metadata[index as usize];
        assert!(!metadata.occupied, "paintable arena allocated a live slot");
        metadata.generation = metadata
            .generation
            .checked_add(1)
            .expect("retired paintable arena slot was reused");
        metadata.occupied = true;
        let generation = metadata.generation;

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

    pub fn free(&mut self, id: PaintableSlotId, generation: u32) {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let index = id.slot_index();
        assert_eq!(
            u32::from(id.generation()),
            generation,
            "paintable arena slot ID and allocation generation disagree"
        );
        let metadata = &mut self.slot_metadata[index as usize];
        assert!(metadata.occupied, "paintable arena freed an unused slot");
        assert_eq!(
            metadata.generation,
            id.generation(),
            "paintable arena freed a stale slot generation"
        );
        metadata.occupied = false;
        let should_reuse = metadata.generation != u8::MAX;
        self.absolute_rect_memo.get_mut().clear();

        self.remove_from_tree(id);
        while let Some(child) = self.first_child(id) {
            self.remove_from_tree(child);
        }
        let layout_node = self.data_ref(id).layout_node;
        if !layout_node.is_invalid()
            && let Some(entry) = self.paintable_of_node.get_mut(layout_node.slot_index() as usize)
            && *entry == id
        {
            *entry = PaintableSlotId::INVALID;
        }

        *self.data_mut_by_index(index) = PaintableData::default();
        self.side_data[index as usize] = PaintableSideData::default();
        self.paint_caches.get_mut()[index as usize] = None;
        self.live_count = self
            .live_count
            .checked_sub(1)
            .expect("paintable arena live count underflowed");
        if should_reuse {
            self.free_list.push(index);
        }
    }

    pub fn layout_node_freed(&mut self, layout_slot_index: u32) {
        let Some(entry) = self.paintable_of_node.get_mut(layout_slot_index as usize) else {
            return;
        };
        let paintable = std::mem::replace(entry, PaintableSlotId::INVALID);
        if !paintable.is_invalid() && self.is_live(paintable) {
            self.update_data(paintable, |data| {
                data.layout_node = NodeSlotId::INVALID;
                data.containing_block = PaintableSlotId::INVALID;
            });
            self.absolute_rect_memo.get_mut().clear();
        }
    }

    pub fn is_live(&self, id: PaintableSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        let index = id.slot_index() as usize;
        self.slot_metadata
            .get(index)
            .is_some_and(|metadata| metadata.occupied && metadata.generation == id.generation())
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
        self.paintable_of_node
            .get(layout_node.slot_index() as usize)
            .copied()
            .filter(|paintable| self.is_live(*paintable))
            .unwrap_or(PaintableSlotId::INVALID)
    }

    pub fn set_paintable_of_node(&mut self, layout_node: NodeSlotId, paintable: PaintableSlotId) {
        let index = layout_node.slot_index() as usize;
        if self.paintable_of_node.len() <= index {
            self.paintable_of_node.resize(index + 1, PaintableSlotId::INVALID);
        }
        self.paintable_of_node[index] = paintable;
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
