/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::layout::{FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize};
use crate::painting::paintable_data::*;
use crate::painting::record::cache::PaintCache;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::ffi::c_void;

pub(crate) const PAINTABLE_SLOTS_PER_CHUNK: usize = 64;

#[repr(align(64))]
struct PaintableRowChunk {
    slots: [Cell<PaintableData>; PAINTABLE_SLOTS_PER_CHUNK],
}

fn new_chunk() -> Box<PaintableRowChunk> {
    // SAFETY: Every slot is written with PaintableData::default() before the chunk is exposed;
    // the chunk is built in place on the heap because it is too large for the stack.
    unsafe {
        let mut chunk = Box::<PaintableRowChunk>::new_uninit();
        let slots = &raw mut (*chunk.as_mut_ptr()).slots;
        for offset in 0..PAINTABLE_SLOTS_PER_CHUNK {
            (&raw mut (*slots)[offset]).write(Cell::new(PaintableData::default()));
        }
        chunk.assume_init()
    }
}

type ChromeStateCallback = (
    *mut c_void,
    unsafe extern "C" fn(*mut c_void, NodeSlotId, PaintableRowResetKind),
);

#[derive(Clone, Copy)]
pub(crate) struct PaintableRowReset {
    slot: NodeSlotId,
    kind: PaintableRowResetKind,
    callback: Option<ChromeStateCallback>,
}

impl PaintableRowReset {
    pub(crate) fn invoke_callback(self) {
        if let Some((context, callback)) = self.callback {
            // SAFETY: Registration and unregistration keep the callback context live.
            unsafe { callback(context, self.slot, self.kind) };
        }
    }
}

#[derive(Default)]
struct CommittedFragmentLinkSlot {
    layout_slot_generation: u8,
    link: Option<Box<crate::layout::FragmentLink>>,
}

#[derive(Default)]
pub(crate) struct PaintableRowStore {
    chunks: RefCell<Vec<Box<PaintableRowChunk>>>,
    side_data: RefCell<Vec<PaintableSideData>>,
    paint_caches: RefCell<Vec<PaintCache>>,
    absolute_rect_memo: RefCell<Vec<Option<(NodeSlotId, u64, crate::css::css_pixels::CssPixelRect)>>>,
    absolute_rect_memo_epoch: Cell<u64>,
    committed_fragment_links: RefCell<Vec<CommittedFragmentLinkSlot>>,
    chrome_state_callback: Cell<Option<ChromeStateCallback>>,
}

impl PaintableRowStore {
    pub(crate) fn committed_fragment_link_cloned(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
    ) -> Option<crate::layout::FragmentLink> {
        self.committed_fragment_links
            .borrow()
            .get(layout_slot_index as usize)
            .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
            .and_then(|slot| slot.link.as_deref().cloned())
    }

    pub(crate) fn set_committed_fragment_link(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
        link: crate::layout::FragmentLink,
    ) {
        let mut slots = self.committed_fragment_links.borrow_mut();
        if slots.len() <= layout_slot_index as usize {
            slots.resize_with(layout_slot_index as usize + 1, CommittedFragmentLinkSlot::default);
        }
        let slot = &mut slots[layout_slot_index as usize];
        if slot.layout_slot_generation != layout_slot_generation {
            *slot = CommittedFragmentLinkSlot {
                layout_slot_generation,
                link: Some(Box::new(link)),
            };
        } else if let Some(retained_link) = &mut slot.link {
            **retained_link = link;
        } else {
            slot.link = Some(Box::new(link));
        }
    }

    pub(crate) fn take_committed_fragment_link(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
    ) -> Option<crate::layout::FragmentLink> {
        self.committed_fragment_links
            .borrow_mut()
            .get_mut(layout_slot_index as usize)
            .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
            .and_then(|slot| slot.link.take())
            .map(|link| *link)
    }

    pub(crate) fn reset_committed_fragment_link_slot(&self, layout_slot_index: u32) {
        if let Some(slot) = self
            .committed_fragment_links
            .borrow_mut()
            .get_mut(layout_slot_index as usize)
        {
            *slot = CommittedFragmentLinkSlot::default();
        }
    }
}

impl LayoutNodeArena {
    pub(crate) fn set_chrome_state_callback(
        &self,
        context: *mut c_void,
        callback: unsafe extern "C" fn(*mut c_void, NodeSlotId, PaintableRowResetKind),
    ) {
        self.paintable_rows.chrome_state_callback.set(Some((context, callback)));
    }

    pub(crate) fn clear_chrome_state_callback(&self) {
        self.paintable_rows.chrome_state_callback.set(None);
    }

    pub(crate) fn with_committed_fragment_link<R>(
        &self,
        node: NodeSlotId,
        read: impl FnOnce(Option<&crate::layout::FragmentLink>) -> R,
    ) -> R {
        debug_assert!(self.paintable_row_is_populated(node));
        let slots = self.paintable_rows.committed_fragment_links.borrow();
        read(
            slots
                .get(node.slot_index() as usize)
                .and_then(|entry| entry.link.as_deref()),
        )
    }

    fn prepare_paintable_row_reset(&self, slot: NodeSlotId, kind: PaintableRowResetKind) -> PaintableRowReset {
        PaintableRowReset {
            slot,
            kind,
            callback: self.paintable_rows.chrome_state_callback.get(),
        }
    }

    pub(crate) fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<crate::css::css_pixels::CssPixelRect> {
        let store = &self.paintable_rows;
        let (memoized_id, memoized_epoch, rect) = store
            .absolute_rect_memo
            .borrow()
            .get(id.slot_index() as usize)
            .copied()
            .flatten()?;
        (memoized_id == id && memoized_epoch == store.absolute_rect_memo_epoch.get()).then_some(rect)
    }

    pub(crate) fn memoize_absolute_rect(&self, id: NodeSlotId, rect: crate::css::css_pixels::CssPixelRect) {
        let store = &self.paintable_rows;
        store.absolute_rect_memo.borrow_mut()[id.slot_index() as usize] =
            Some((id, store.absolute_rect_memo_epoch.get(), rect));
    }

    pub(crate) fn clear_absolute_rect_memo(&self) {
        let epoch = &self.paintable_rows.absolute_rect_memo_epoch;
        epoch.set(epoch.get().checked_add(1).expect("absolute rect memo epoch overflowed"));
    }

    pub(crate) fn paintable_row_count(&self) -> usize {
        self.paintable_rows.side_data.borrow().len()
    }

    pub(crate) fn visual_context_assignments(&self) -> Vec<(u32, bool, usize, usize, usize, usize)> {
        (0..self.paintable_row_count())
            .map(|index| {
                let data = self.paintable_data_by_index(index as u32);
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
        for cache in self.paintable_rows.paint_caches.borrow().iter() {
            cache.clear_descendant_subtrees();
        }
    }

    pub(crate) fn clear_all_paintable_paint_caches(&self) {
        for cache in self.paintable_rows.paint_caches.borrow().iter() {
            cache.clear();
        }
    }

    pub(crate) fn inline_pieces_root(&self, inline_paintable: NodeSlotId) -> Option<NodeSlotId> {
        if !self.paintable_row_is_populated(inline_paintable) {
            return None;
        }
        let root = self.paintable_data(inline_paintable).containing_block;
        (self.paintable_row_is_populated(root) && self.paintable_data(root).kind.has_lines()).then_some(root)
    }

    pub(crate) fn populate_paintable_row(&self, layout_node: NodeSlotId) {
        let store = &self.paintable_rows;
        let index = layout_node.slot_index() as usize;
        let mut chunks = store.chunks.borrow_mut();
        let mut side_data = store.side_data.borrow_mut();
        let mut paint_caches = store.paint_caches.borrow_mut();
        let mut absolute_rect_memo = store.absolute_rect_memo.borrow_mut();
        while side_data.len() <= index {
            if side_data.len().is_multiple_of(PAINTABLE_SLOTS_PER_CHUNK) {
                chunks.push(new_chunk());
            }
            side_data.push(PaintableSideData::default());
            paint_caches.push(PaintCache::default());
            absolute_rect_memo.push(None);
        }

        chunks[index / PAINTABLE_SLOTS_PER_CHUNK].slots[index % PAINTABLE_SLOTS_PER_CHUNK].set(PaintableData {
            slot_generation: layout_node.generation(),
            ..PaintableData::default()
        });
        side_data[index] = PaintableSideData::default();
        paint_caches[index].clear();
        absolute_rect_memo[index] = None;
    }

    fn reset_paintable_row(&self, clear_caches_along_paint_chain: bool, reset: PaintableRowReset) {
        let id = reset.slot;
        if clear_caches_along_paint_chain {
            self.clear_descendant_subtree_caches_along_paint_chain(id);
        }
        self.clear_absolute_rect_memo();
        let store = &self.paintable_rows;
        let index = id.slot_index() as usize;
        store.chunks.borrow()[index / PAINTABLE_SLOTS_PER_CHUNK].slots[index % PAINTABLE_SLOTS_PER_CHUNK]
            .set(PaintableData::default());
        store.side_data.borrow_mut()[index] = PaintableSideData::default();
        store.paint_caches.borrow()[index].clear();
    }

    pub(crate) fn prepare_paintable_row_freed_reset(&self, layout_slot_index: u32) -> Option<PaintableRowReset> {
        if layout_slot_index as usize >= self.paintable_row_count() {
            return None;
        }
        let generation = self.paintable_data_by_index(layout_slot_index).slot_generation;
        if generation == 0 {
            return None;
        }
        Some(self.prepare_paintable_row_reset(
            NodeSlotId::new(layout_slot_index, generation),
            PaintableRowResetKind::Freed,
        ))
    }

    pub(crate) fn paintable_row_freed(&self, reset: PaintableRowReset) {
        self.reset_paintable_row(false, reset);
    }

    pub(crate) fn prepare_paintable_row_cleared_reset(&self, layout_node: NodeSlotId) -> Option<PaintableRowReset> {
        self.paintable_row_is_populated(layout_node)
            .then(|| self.prepare_paintable_row_reset(layout_node, PaintableRowResetKind::Cleared))
    }

    pub(crate) fn paintable_row_cleared(&self, reset: PaintableRowReset) {
        self.reset_paintable_row(true, reset);
    }

    pub(crate) fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        let index = id.slot_index() as usize;
        if index >= self.paintable_row_count() {
            return false;
        }
        let generation = self.paintable_data_by_index(index as u32).slot_generation;
        generation != 0 && generation == id.generation()
    }

    fn paintable_data_by_index(&self, index: u32) -> PaintableData {
        let chunks = self.paintable_rows.chunks.borrow();
        let chunk = chunks
            .get(index as usize / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot index");
        chunk.slots[index as usize % PAINTABLE_SLOTS_PER_CHUNK].get()
    }

    fn with_paintable_data_cell<R>(&self, id: NodeSlotId, read: impl FnOnce(&Cell<PaintableData>) -> R) -> R {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let index = id.slot_index() as usize;
        let chunks = self.paintable_rows.chunks.borrow();
        let chunk = chunks
            .get(index / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot ID");
        let cell = &chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK];
        assert_eq!(
            cell.get().slot_generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        read(cell)
    }

    pub(crate) fn paintable_data_ptr(&self, id: NodeSlotId) -> *mut PaintableData {
        self.with_paintable_data_cell(id, Cell::as_ptr)
    }

    pub(crate) fn paintable_data(&self, id: NodeSlotId) -> PaintableData {
        self.with_paintable_data_cell(id, Cell::get)
    }

    pub(crate) fn update_paintable_data<R>(&self, id: NodeSlotId, callback: impl FnOnce(&mut PaintableData) -> R) -> R {
        self.with_paintable_data_cell(id, |cell| {
            let mut data = cell.get();
            let result = callback(&mut data);
            cell.set(data);
            result
        })
    }

    pub(crate) fn paintable_paint_cache(&self, id: NodeSlotId) -> Ref<'_, PaintCache> {
        Ref::map(self.paintable_rows.paint_caches.borrow(), |caches| {
            &caches[id.slot_index() as usize]
        })
    }

    pub(crate) fn paintable_paint_cache_if_allocated(&self, id: NodeSlotId) -> Option<Ref<'_, PaintCache>> {
        Ref::filter_map(self.paintable_rows.paint_caches.borrow(), |caches| {
            caches.get(id.slot_index() as usize)
        })
        .ok()
    }

    pub(crate) fn invalidate_paint_cache(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        self.paintable_paint_cache(id).clear();
        let mut ancestor = crate::painting::paint_order::paint_parent(self, id);
        while let Some(current) = ancestor {
            self.paintable_paint_cache(current).clear_descendant_subtrees();
            ancestor = crate::painting::paint_order::paint_parent(self, current);
        }
    }

    pub(crate) fn clear_descendant_subtree_caches_along_paint_chain(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        let mut current = Some(id);
        while let Some(slot) = current {
            self.paintable_paint_cache(slot).clear_descendant_subtrees();
            current = crate::painting::paint_order::paint_parent(self, slot);
        }
    }

    pub(crate) fn clear_descendant_subtree_caches_from_layout_node(&self, mut node: NodeSlotId) {
        loop {
            if self.paintable_row_is_populated(node) {
                self.clear_descendant_subtree_caches_along_paint_chain(node);
                return;
            }
            if !crate::painting::fragment_ownership::node_is_fragmented_inline(self, node) {
                return;
            }
            let Some(parent) = self.node_parent_if_live(node) else {
                return;
            };
            node = parent;
        }
    }

    pub(crate) fn invalidate_for_repaint(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        self.invalidate_paint_cache(id);
        let mut stack = vec![id];
        while let Some(current) = stack.pop() {
            let mut child = crate::painting::paint_order::first_paint_child(self, current);
            while let Some(child_slot) = child {
                let child_flags = self.node_flags_if_live(child_slot);
                if child_flags & NodeFlag::Anonymous as u32 != 0 {
                    self.paintable_paint_cache(child_slot).clear();
                    stack.push(child_slot);
                }
                child = crate::painting::paint_order::next_paint_sibling(self, child_slot);
            }
        }
        if self.paintable_data(id).kind == PaintableKind::InlinePaintable {
            let mut ancestor = crate::painting::paint_order::paint_parent(self, id);
            while let Some(current) = ancestor {
                self.invalidate_paint_cache(current);
                if self.paintable_data(current).kind.has_lines() {
                    break;
                }
                ancestor = crate::painting::paint_order::paint_parent(self, current);
            }
        }
    }

    pub(crate) fn invalidate_propagated_text_decoration_caches(&self, root: NodeSlotId) {
        if !self.paintable_row_is_populated(root) {
            return;
        }

        let mut stack = Vec::new();
        if let Some(first_child) = crate::painting::paint_order::first_paint_child(self, root) {
            stack.push(first_child);
        }
        while let Some(current) = stack.pop() {
            if let Some(next_sibling) = crate::painting::paint_order::next_paint_sibling(self, current) {
                stack.push(next_sibling);
            }

            let data = self.paintable_data(current);
            if crate::painting::style_queries::is_text_decoration_propagation_boundary(self, current) {
                continue;
            }
            // Only fragment-painting paintables record propagated decorations.
            if data.kind.has_lines() || data.kind == PaintableKind::InlinePaintable {
                self.paintable_paint_cache(current).clear();
            }
            if let Some(first_child) = crate::painting::paint_order::first_paint_child(self, current) {
                stack.push(first_child);
            }
        }
        let mut ancestor = Some(root);
        while let Some(current) = ancestor {
            self.paintable_paint_cache(current).clear_descendant_subtrees();
            ancestor = crate::painting::paint_order::paint_parent(self, current);
        }
    }

    pub(crate) fn paintable_side_data(&self, id: NodeSlotId) -> Ref<'_, PaintableSideData> {
        debug_assert!(self.paintable_row_is_populated(id));
        Ref::map(self.paintable_rows.side_data.borrow(), |side_data| {
            &side_data[id.slot_index() as usize]
        })
    }

    pub(crate) fn paintable_side_data_mut(&self, id: NodeSlotId) -> RefMut<'_, PaintableSideData> {
        debug_assert!(self.paintable_row_is_populated(id));
        RefMut::map(self.paintable_rows.side_data.borrow_mut(), |side_data| {
            &mut side_data[id.slot_index() as usize]
        })
    }

    pub(crate) fn prepare_paintable_row_recommit_notification(&self, id: NodeSlotId) -> PaintableRowReset {
        assert!(self.paintable_row_is_populated(id));
        self.prepare_paintable_row_reset(id, PaintableRowResetKind::Recommitted)
    }

    pub(crate) fn begin_paintable_row_recommit(&self, id: NodeSlotId) {
        self.update_paintable_data(id, |data| {
            data.offset = FfiCssPixelPoint::default();
            data.content_size = FfiCssPixelSize::default();
            data.overflow = FfiOverflowData::default();
            data.has_overflow = false;
            data.sticky_insets = FfiStickyInsets::default();
            data.has_sticky_insets = false;
            data.local_padding_box_union = FfiCssPixelRect::default();
            data.local_border_box_union = FfiCssPixelRect::default();
            data.stacking_context = crate::painting::stacking_context::NO_STACKING_CONTEXT;
        });
        self.paintable_paint_cache(id).clear();
        self.paintable_side_data_mut(id).clear_committed_records();
    }
}
