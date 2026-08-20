/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::{FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize, LayoutNodeArena};
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
    pub(crate) paint_caches: Vec<crate::painting::record::cache::PaintCache>,
    absolute_rect_memo: std::cell::RefCell<Vec<Option<(PaintableSlotId, u64, crate::css::css_pixels::CssPixelRect)>>>,
    absolute_rect_memo_epoch: Cell<u64>,
    pub(crate) scrollable_overflow_contained_boxes: std::collections::HashMap<NodeSlotId, Vec<NodeSlotId>>,
    pub(crate) selection: Option<crate::painting::selection::SelectionRange>,
}

impl PaintableArena {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn memoized_absolute_rect(&self, id: PaintableSlotId) -> Option<crate::css::css_pixels::CssPixelRect> {
        let (memoized_id, memoized_epoch, rect) = self
            .absolute_rect_memo
            .borrow()
            .get(id.slot_index() as usize)
            .copied()
            .flatten()?;
        (memoized_id == id && memoized_epoch == self.absolute_rect_memo_epoch.get()).then_some(rect)
    }

    pub fn memoize_absolute_rect(&self, id: PaintableSlotId, rect: crate::css::css_pixels::CssPixelRect) {
        self.absolute_rect_memo.borrow_mut()[id.slot_index() as usize] =
            Some((id, self.absolute_rect_memo_epoch.get(), rect));
    }

    pub fn clear_absolute_rect_memo(&self) {
        self.absolute_rect_memo_epoch.set(
            self.absolute_rect_memo_epoch
                .get()
                .checked_add(1)
                .expect("absolute rect memo epoch overflowed"),
        );
    }

    pub(crate) fn slot_count(&self) -> usize {
        self.side_data.len()
    }

    pub(crate) fn visual_context_assignments(&self) -> Vec<(u32, bool, usize, usize, usize, usize)> {
        (0..self.slot_count())
            .map(|index| {
                let data = self.data_by_index(index as u32);
                (
                    u32::from(data.slot_generation),
                    data.has_accumulated_visual_context,
                    data.accumulated_visual_context_index,
                    data.accumulated_visual_context_for_descendants_index,
                    data.visual_context_nodes_begin,
                    data.visual_context_nodes_end,
                )
            })
            .collect()
    }

    pub(crate) fn clear_descendant_subtree_caches(&self) {
        for cache in &self.paint_caches {
            cache.clear_descendant_subtrees();
        }
    }

    pub(crate) fn inline_pieces_root(&self, inline_paintable: PaintableSlotId) -> Option<PaintableSlotId> {
        if !self.is_live(inline_paintable) {
            return None;
        }
        let root = self.data_ref(inline_paintable).containing_block;
        (!root.is_invalid() && self.is_live(root) && self.data_ref(root).kind.has_lines()).then_some(root)
    }

    pub(crate) fn for_each_in_subtree(&self, root: PaintableSlotId, mut callback: impl FnMut(PaintableSlotId)) {
        let mut stack = vec![root];
        while let Some(current) = stack.pop() {
            if let Some(next) = self.next_sibling(current)
                && current != root
            {
                stack.push(next);
            }
            if let Some(first_child) = self.first_child(current) {
                stack.push(first_child);
            }
            callback(current);
        }
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
            self.paint_caches
                .push(crate::painting::record::cache::PaintCache::default());
            self.absolute_rect_memo.get_mut().push(None);
        }

        let generation = layout_node.generation();
        let data = self.data_mut_by_index(index);
        *data = PaintableData::default();
        data.slot_generation = generation;
        data.layout_node = layout_node;
        data.shell = shell;
        self.side_data[index as usize] = PaintableSideData::default();
        self.paint_caches[index as usize].clear();
        self.absolute_rect_memo.get_mut()[index as usize] = None;

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
        self.clear_absolute_rect_memo();
        self.remove_from_tree(id);
        while let Some(child) = self.first_child(id) {
            self.remove_from_tree(child);
        }
        let index = id.slot_index();
        *self.data_mut_by_index(index) = PaintableData::default();
        self.side_data[index as usize] = PaintableSideData::default();
        self.paint_caches[index as usize].clear();
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
        self.paint_caches[id.slot_index() as usize].clear();
        let mut ancestor = self.data_ref(id).parent;
        while !ancestor.is_invalid() {
            self.paint_caches[ancestor.slot_index() as usize].clear_descendant_subtrees();
            ancestor = self.data_ref(ancestor).parent;
        }
    }

    pub(crate) fn invalidate_propagated_text_decoration_caches(
        &self,
        layout_arena: &LayoutNodeArena,
        root: PaintableSlotId,
    ) {
        if !self.is_live(root) {
            return;
        }

        let mut stack = Vec::new();
        if let Some(first_child) = self.first_child(root) {
            stack.push(first_child);
        }
        while let Some(current) = stack.pop() {
            if let Some(next_sibling) = self.next_sibling(current) {
                stack.push(next_sibling);
            }

            let data = self.data_ref(current);
            if crate::painting::style_queries::is_text_decoration_propagation_boundary(layout_arena, data.layout_node) {
                continue;
            }
            // Only fragment-painting paintables record propagated decorations.
            if data.kind.has_lines() || data.kind == PaintableKind::InlinePaintable {
                self.paint_caches[current.slot_index() as usize].clear();
            }
            if let Some(first_child) = self.first_child(current) {
                stack.push(first_child);
            }
        }
        let mut ancestor = root;
        while !ancestor.is_invalid() {
            self.paint_caches[ancestor.slot_index() as usize].clear_descendant_subtrees();
            ancestor = self.data_ref(ancestor).parent;
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
        self.clear_descendant_subtree_caches_inclusive(child);
        self.clear_descendant_subtree_caches_inclusive(parent);
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
        self.clear_descendant_subtree_caches_inclusive(child);
        self.clear_descendant_subtree_caches_inclusive(parent);
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
        self.clear_descendant_subtree_caches_inclusive(id);
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

    fn clear_descendant_subtree_caches_inclusive(&self, id: PaintableSlotId) {
        let mut current = id;
        while !current.is_invalid() {
            self.paint_caches[current.slot_index() as usize].clear_descendant_subtrees();
            current = self.data_ref(current).parent;
        }
    }

    pub fn reset_for_relayout(&mut self, id: PaintableSlotId) {
        self.clear_absolute_rect_memo();
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
        self.paint_caches[id.slot_index() as usize].clear();
        self.side_mut(id).reset_for_relayout();
    }
}
