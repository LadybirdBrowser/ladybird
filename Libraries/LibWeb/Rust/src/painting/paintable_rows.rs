/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::layout::{fragment_tree, used_values};
use crate::painting::paintable_data::*;
use crate::painting::record::cache::PaintCache;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::ffi::c_void;
use std::ops::{Deref, DerefMut};

pub(crate) const PAINTABLE_SLOTS_PER_CHUNK: usize = 64;

#[repr(align(64))]
struct PaintableRowChunk {
    slots: [PaintableData; PAINTABLE_SLOTS_PER_CHUNK],
}

fn new_chunk() -> Box<PaintableRowChunk> {
    // SAFETY: Every slot is written with PaintableData::default() before the chunk is exposed;
    // the chunk is built in place on the heap because it is too large for the stack.
    unsafe {
        let mut chunk = Box::<PaintableRowChunk>::new_uninit();
        let slots = &raw mut (*chunk.as_mut_ptr()).slots;
        for offset in 0..PAINTABLE_SLOTS_PER_CHUNK {
            (&raw mut (*slots)[offset]).write(PaintableData::default());
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
    link: Option<Box<fragment_tree::FragmentLink>>,
}

#[derive(Default)]
pub(crate) struct PaintableRowStore {
    chunks: Vec<Box<PaintableRowChunk>>,
    side_data: RefCell<Vec<PaintableSideData>>,
    paint_caches: RefCell<Vec<PaintCache>>,
    absolute_rect_memo: RefCell<Vec<Option<(NodeSlotId, u64, crate::css::css_pixels::CssPixelRect)>>>,
    absolute_rect_memo_epoch: Cell<u64>,
    committed_fragment_links: RefCell<Vec<CommittedFragmentLinkSlot>>,
    chrome_state_callback: Cell<Option<ChromeStateCallback>>,
    completed_record_gen: Cell<u64>,
    all_paint_caches_dirty_gen: Cell<u64>,
    all_descendant_subtree_caches_dirty_gen: Cell<u64>,
    paint_recording_in_progress: Cell<bool>,
}

pub(crate) struct PaintableRows<Arena> {
    arena: Arena,
}

pub(crate) type PaintableRowsRef<'a> = PaintableRows<&'a LayoutNodeArena>;
pub(crate) type PaintableRowsMut<'a> = PaintableRows<&'a mut LayoutNodeArena>;

impl Clone for PaintableRowsRef<'_> {
    fn clone(&self) -> Self {
        Self { arena: self.arena }
    }
}

pub(crate) trait PaintableRowsRead: Deref<Target = LayoutNodeArena> {
    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData;
    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool;
}

pub(crate) trait PaintableRowsWrite: PaintableRowsRead {
    fn paintable_data_mut(&mut self, id: NodeSlotId) -> &mut PaintableData;
}

impl<Arena> Deref for PaintableRows<Arena>
where
    Arena: Deref<Target = LayoutNodeArena>,
{
    type Target = LayoutNodeArena;

    fn deref(&self) -> &Self::Target {
        self.arena.deref()
    }
}

impl<Arena> DerefMut for PaintableRows<Arena>
where
    Arena: DerefMut<Target = LayoutNodeArena>,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.arena.deref_mut()
    }
}

impl<Arena> PaintableRows<Arena>
where
    Arena: Deref<Target = LayoutNodeArena>,
{
    pub(crate) fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let index = id.slot_index() as usize;
        let chunk = self
            .arena
            .paintable_rows
            .chunks
            .get(index / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot ID");
        let data = &chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK];
        assert_eq!(
            data.slot_generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn paintable_data_ptr(&self, id: NodeSlotId) -> *const PaintableData {
        self.paintable_data(id)
    }

    pub(crate) fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        let index = id.slot_index() as usize;
        let Some(chunk) = self.arena.paintable_rows.chunks.get(index / PAINTABLE_SLOTS_PER_CHUNK) else {
            return false;
        };
        let generation = chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK].slot_generation;
        generation != 0 && generation == id.generation()
    }

    pub(crate) fn clear_cached_overflow_data(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        let index = id.slot_index() as usize;
        let chunk = &self.arena.paintable_rows.chunks[index / PAINTABLE_SLOTS_PER_CHUNK];
        let data = (&raw const chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK]).cast_mut();
        // SAFETY: paintable_row_is_populated established that the chunk and slot exist.
        unsafe { (&raw mut (*data).overflow_valid_across_recommits).write(false) };
    }

    pub(crate) fn inline_pieces_root(&self, inline_paintable: NodeSlotId) -> Option<NodeSlotId> {
        if !self.paintable_row_is_populated(inline_paintable) {
            return None;
        }
        let root = self.paintable_data(inline_paintable).containing_block;
        (self.paintable_row_is_populated(root) && self.paintable_data(root).kind.has_lines()).then_some(root)
    }

    pub(crate) fn visual_context_assignments(&self) -> Vec<(u32, bool, usize, usize, usize, usize)> {
        (0..self.arena.paintable_row_count())
            .map(|index| {
                let chunk = &self.arena.paintable_rows.chunks[index / PAINTABLE_SLOTS_PER_CHUNK];
                let data = &chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK];
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

    pub(crate) fn mark_paint_cache_self_dirty(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        self.arena.debug_assert_not_recording();
        let next_dirty_gen = self.arena.paint_cache_next_dirty_gen();
        self.arena.paintable_paint_cache(id).mark_self_dirty(next_dirty_gen);
        let mut ancestor = crate::painting::paint_order::paint_parent(self, id);
        while let Some(current) = ancestor {
            if self
                .arena
                .paintable_paint_cache(current)
                .mark_descendants_dirty(next_dirty_gen)
            {
                break;
            }
            ancestor = crate::painting::paint_order::paint_parent(self, current);
        }
    }

    pub(crate) fn mark_descendant_subtree_caches_dirty_along_paint_chain(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        self.arena.debug_assert_not_recording();
        let next_dirty_gen = self.arena.paint_cache_next_dirty_gen();
        let mut current = Some(id);
        while let Some(slot) = current {
            if self
                .arena
                .paintable_paint_cache(slot)
                .mark_descendants_dirty(next_dirty_gen)
            {
                break;
            }
            current = crate::painting::paint_order::paint_parent(self, slot);
        }
    }

    pub(crate) fn invalidate_paint_cache(&self, id: NodeSlotId) {
        self.mark_paint_cache_self_dirty(id);
    }

    pub(crate) fn mark_descendant_subtree_caches_dirty_from_layout_node(&self, mut node: NodeSlotId) {
        loop {
            if self.paintable_row_is_populated(node) {
                self.mark_descendant_subtree_caches_dirty_along_paint_chain(node);
                return;
            }
            if !crate::painting::fragment_ownership::node_is_fragmented_inline(self.arena.deref(), node) {
                return;
            }
            let Some(parent) = self.arena.node_parent_if_live(node) else {
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
                let child_flags = self.arena.node_flags_if_live(child_slot);
                if child_flags & NodeFlag::Anonymous as u32 != 0 {
                    self.mark_paint_cache_self_dirty(child_slot);
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
            if crate::painting::style_queries::is_text_decoration_propagation_boundary(self.arena.deref(), current) {
                continue;
            }
            if data.kind.has_lines() || data.kind == PaintableKind::InlinePaintable {
                self.mark_paint_cache_self_dirty(current);
            }
            if let Some(first_child) = crate::painting::paint_order::first_paint_child(self, current) {
                stack.push(first_child);
            }
        }
        self.mark_descendant_subtree_caches_dirty_along_paint_chain(root);
    }

    pub(crate) fn prepare_paintable_row_recommit_notification(&self, id: NodeSlotId) -> PaintableRowReset {
        assert!(self.paintable_row_is_populated(id));
        self.arena
            .prepare_paintable_row_reset(id, PaintableRowResetKind::Recommitted)
    }
}

impl<Arena> PaintableRows<Arena>
where
    Arena: DerefMut<Target = LayoutNodeArena>,
{
    pub(crate) fn paintable_data_mut(&mut self, id: NodeSlotId) -> &mut PaintableData {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let index = id.slot_index() as usize;
        let chunk = self
            .arena
            .paintable_rows
            .chunks
            .get_mut(index / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot ID");
        let data = &mut chunk.slots[index % PAINTABLE_SLOTS_PER_CHUNK];
        assert_eq!(
            data.slot_generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn begin_paintable_row_recommit(&mut self, id: NodeSlotId) {
        {
            let data = self.paintable_data_mut(id);
            data.offset = used_values::FfiCssPixelPoint::default();
            data.content_size = used_values::FfiCssPixelSize::default();
            data.overflow_measured_this_commit = false;
            data.sticky_insets = FfiStickyInsets::default();
            data.has_sticky_insets = false;
            data.local_padding_box_union = used_values::FfiCssPixelRect::default();
            data.local_border_box_union = used_values::FfiCssPixelRect::default();
            data.stacking_context = crate::painting::stacking_context::NO_STACKING_CONTEXT;
        }
        // The paint cache is deliberately kept; the commit diff marks rows whose committed
        // fragment identity or offset actually changed.
        self.arena.paintable_side_data_mut(id).clear_committed_records();
    }
}

impl<Arena> PaintableRowsRead for PaintableRows<Arena>
where
    Arena: Deref<Target = LayoutNodeArena>,
{
    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        PaintableRows::paintable_data(self, id)
    }

    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        PaintableRows::paintable_row_is_populated(self, id)
    }
}

impl<Arena> PaintableRowsWrite for PaintableRows<Arena>
where
    Arena: DerefMut<Target = LayoutNodeArena>,
{
    fn paintable_data_mut(&mut self, id: NodeSlotId) -> &mut PaintableData {
        PaintableRows::paintable_data_mut(self, id)
    }
}

impl PaintableRowStore {
    pub(crate) fn committed_fragment_link_cloned(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
    ) -> Option<fragment_tree::FragmentLink> {
        self.committed_fragment_links
            .borrow()
            .get(layout_slot_index as usize)
            .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
            .and_then(|slot| slot.link.as_deref().cloned())
    }

    pub(crate) fn with_committed_fragment_link<R>(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
        read: impl FnOnce(Option<&fragment_tree::FragmentLink>) -> R,
    ) -> R {
        let slots = self.committed_fragment_links.borrow();
        read(
            slots
                .get(layout_slot_index as usize)
                .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
                .and_then(|slot| slot.link.as_deref()),
        )
    }

    pub(crate) fn set_committed_fragment_link(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
        link: fragment_tree::FragmentLink,
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
    ) -> Option<fragment_tree::FragmentLink> {
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
    pub(crate) fn paintable_rows(&self) -> PaintableRowsRef<'_> {
        PaintableRows { arena: self }
    }

    pub(crate) fn paintable_rows_mut(&mut self) -> PaintableRowsMut<'_> {
        PaintableRows { arena: self }
    }

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
        read: impl FnOnce(Option<&fragment_tree::FragmentLink>) -> R,
    ) -> R {
        debug_assert!(self.paintable_row_is_populated(node));
        let slots = self.paintable_rows.committed_fragment_links.borrow();
        read(
            slots
                .get(node.slot_index() as usize)
                .and_then(|entry| entry.link.as_deref()),
        )
    }

    pub(crate) fn with_committed_fragment_link_during_layout<R>(
        &self,
        node: NodeSlotId,
        read: impl FnOnce(Option<&fragment_tree::FragmentLink>) -> R,
    ) -> R {
        self.paintable_rows
            .with_committed_fragment_link(node.slot_index(), node.generation(), read)
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

    pub(crate) fn paint_cache_completed_record_gen(&self) -> u64 {
        self.paintable_rows.completed_record_gen.get()
    }

    pub(crate) fn paint_cache_next_dirty_gen(&self) -> u64 {
        self.paintable_rows
            .completed_record_gen
            .get()
            .checked_add(1)
            .expect("paint cache record generation overflowed")
    }

    pub(crate) fn note_paint_record_completed_with_cache_writes(&self) {
        let generation = &self.paintable_rows.completed_record_gen;
        generation.set(
            generation
                .get()
                .checked_add(1)
                .expect("paint cache record generation overflowed"),
        );
    }

    pub(crate) fn set_paint_recording_in_progress(&self, in_progress: bool) {
        self.paintable_rows.paint_recording_in_progress.set(in_progress);
    }

    pub(crate) fn debug_assert_not_recording(&self) {
        debug_assert!(
            !self.paintable_rows.paint_recording_in_progress.get(),
            "paint cache invalidation during display list recording would be aged out unseen"
        );
    }

    pub(crate) fn mark_all_paint_caches_dirty(&self) {
        self.debug_assert_not_recording();
        self.paintable_rows
            .all_paint_caches_dirty_gen
            .set(self.paint_cache_next_dirty_gen());
    }

    pub(crate) fn mark_all_descendant_subtree_caches_dirty(&self) {
        self.debug_assert_not_recording();
        self.paintable_rows
            .all_descendant_subtree_caches_dirty_gen
            .set(self.paint_cache_next_dirty_gen());
    }

    pub(crate) fn all_paint_caches_dirty(&self) -> bool {
        self.paintable_rows.all_paint_caches_dirty_gen.get() > self.paintable_rows.completed_record_gen.get()
    }

    pub(crate) fn all_descendant_subtree_caches_dirty(&self) -> bool {
        self.paintable_rows.all_descendant_subtree_caches_dirty_gen.get()
            > self.paintable_rows.completed_record_gen.get()
    }

    pub(crate) fn inline_pieces_root(&self, inline_paintable: NodeSlotId) -> Option<NodeSlotId> {
        self.paintable_rows().inline_pieces_root(inline_paintable)
    }

    pub(crate) fn populate_paintable_row(&mut self, layout_node: NodeSlotId) {
        let store = &mut self.paintable_rows;
        let index = layout_node.slot_index() as usize;
        let chunks = &mut store.chunks;
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

        chunks[index / PAINTABLE_SLOTS_PER_CHUNK].slots[index % PAINTABLE_SLOTS_PER_CHUNK] = PaintableData {
            slot_generation: layout_node.generation(),
            ..PaintableData::default()
        };
        side_data[index] = PaintableSideData::default();
        paint_caches[index].clear();
        absolute_rect_memo[index] = None;
    }

    fn reset_paintable_row(&mut self, mark_caches_dirty_along_paint_chain: bool, reset: PaintableRowReset) {
        let id = reset.slot;
        if mark_caches_dirty_along_paint_chain {
            self.paintable_rows()
                .mark_descendant_subtree_caches_dirty_along_paint_chain(id);
        }
        self.clear_absolute_rect_memo();
        let store = &mut self.paintable_rows;
        let index = id.slot_index() as usize;
        store.chunks[index / PAINTABLE_SLOTS_PER_CHUNK].slots[index % PAINTABLE_SLOTS_PER_CHUNK] =
            PaintableData::default();
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

    pub(crate) fn paintable_row_freed(&mut self, reset: PaintableRowReset) {
        self.reset_paintable_row(false, reset);
    }

    pub(crate) fn prepare_paintable_row_cleared_reset(&self, layout_node: NodeSlotId) -> Option<PaintableRowReset> {
        self.paintable_row_is_populated(layout_node)
            .then(|| self.prepare_paintable_row_reset(layout_node, PaintableRowResetKind::Cleared))
    }

    pub(crate) fn paintable_row_cleared(&mut self, reset: PaintableRowReset) {
        self.reset_paintable_row(true, reset);
    }

    pub(crate) fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        self.paintable_rows().paintable_row_is_populated(id)
    }

    fn paintable_data_by_index(&self, index: u32) -> &PaintableData {
        let chunk = self
            .paintable_rows
            .chunks
            .get(index as usize / PAINTABLE_SLOTS_PER_CHUNK)
            .expect("invalid paintable arena slot index");
        &chunk.slots[index as usize % PAINTABLE_SLOTS_PER_CHUNK]
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
        self.paintable_rows().invalidate_paint_cache(id);
    }

    pub(crate) fn transfer_fragments_to_replacement_node(
        &self,
        containing_block: NodeSlotId,
        old_node: NodeSlotId,
        new_node: NodeSlotId,
    ) {
        if !self.paintable_row_is_populated(containing_block) {
            return;
        }
        let mut side = self.paintable_side_data_mut(containing_block);
        for fragment in &mut side.fragments {
            if fragment.layout_node == old_node {
                fragment.layout_node = new_node;
            }
        }
        for piece in &mut side.inline_box_pieces {
            if piece.node == old_node {
                piece.node = new_node;
            }
        }
    }

    pub(crate) fn mark_descendant_subtree_caches_dirty_from_layout_node(&self, node: NodeSlotId) {
        self.paintable_rows()
            .mark_descendant_subtree_caches_dirty_from_layout_node(node);
    }

    pub(crate) fn invalidate_for_repaint(&self, id: NodeSlotId) {
        self.paintable_rows().invalidate_for_repaint(id);
    }

    pub(crate) fn invalidate_propagated_text_decoration_caches(&self, root: NodeSlotId) {
        self.paintable_rows().invalidate_propagated_text_decoration_caches(root);
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
}
