/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the arena's writes leave the layout stage's memos to drop or reconsider. The memos live in the stage's scratch,
//! which the arena cannot reach, so the arena notes what a write did to a box and the stage applies the notes to its
//! memos before a layout pass reads them and before it sweeps them.
//!
//! The notes of a box coalesce. Only a layout pass stores memo entries, and a stage applies the notes before each pass,
//! so between two applications a box's entries change only through the notes: entries a note drops stay dropped, and
//! the notes after it see none.

use super::fc_run_cache::FcRunCacheStore;
use super::in_flow_block_cache::InFlowBlockCacheStore;
use super::layout_node_arena::IntrinsicSizeCaches;
use super::node_data::NodeSlotId;
use super::prepared_inline_content::PreparedInlineContentMemos;
use super::rendered_text::TextMemos;

#[derive(Clone, Copy, Default)]
struct SlotNotes {
    noted: bool,
    prepared_inline_content_dropped: bool,
    run_cache_entries_dropped: bool,
    in_flow_block_entries_dropped: bool,
    intrinsic_sizes_dropped: bool,
    fragment_cache_epoch_bumped: bool,
    structural_inline_changes: u32,
    generation: u8,
}

const NOTES_PER_PAGE: usize = 256;

/// The notes of each box the arena wrote since the stage last applied them, in the order the boxes were first noted.
#[derive(Clone, Default)]
pub(crate) struct MemoInvalidations {
    /// Each page holds the notes of `NOTES_PER_PAGE` slots, and comes to be with the first of them noted, so that a copy
    /// of the notes costs what was noted.
    pages: Vec<Option<Box<[SlotNotes; NOTES_PER_PAGE]>>>,
    noted_slots: Vec<u32>,
    dropped_text_states: Vec<u32>,
}

impl MemoInvalidations {
    fn notes_of(&mut self, slot: u32) -> &mut SlotNotes {
        let (page, row) = (slot as usize / NOTES_PER_PAGE, slot as usize % NOTES_PER_PAGE);
        if self.pages.len() <= page {
            self.pages.resize_with(page + 1, || None);
        }
        let notes = &mut self.pages[page].get_or_insert_with(|| Box::new([SlotNotes::default(); NOTES_PER_PAGE]))[row];
        if !notes.noted {
            notes.noted = true;
            self.noted_slots.push(slot);
        }
        notes
    }

    /// The box in `slot` left its committed subtree or the arena, which drops its run cache entries.
    pub(crate) fn note_run_cache_entries_dropped(&mut self, slot: u32) {
        let notes = self.notes_of(slot);
        notes.run_cache_entries_dropped = true;
        notes.fragment_cache_epoch_bumped = false;
        notes.structural_inline_changes = 0;
    }

    /// The box in `slot` left the arena, which drops all its memo entries.
    pub(crate) fn note_slot_freed(&mut self, slot: u32) {
        self.note_run_cache_entries_dropped(slot);
        let notes = self.notes_of(slot);
        notes.prepared_inline_content_dropped = true;
        notes.in_flow_block_entries_dropped = true;
        notes.intrinsic_sizes_dropped = true;
    }

    /// Something the intrinsic sizes of the box in `slot` were measured from changed.
    pub(crate) fn note_intrinsic_sizes_dropped(&mut self, slot: u32) {
        self.notes_of(slot).intrinsic_sizes_dropped = true;
    }

    /// The text state at `text_state` got other rendered text or was let go of, which the chunks and segmenter made of
    /// the old text no longer describe.
    pub(crate) fn note_text_state_dropped(&mut self, text_state: u32) {
        self.dropped_text_states.push(text_state);
    }

    /// The fragment cache epoch of `node` moved on: its run cache entries need the sweep, and its in-flow block entry
    /// and prepared inline content, valid for one epoch only, are dropped.
    pub(crate) fn note_fragment_cache_epoch_bumped(&mut self, node: NodeSlotId) {
        let notes = self.notes_of(node.slot_index());
        notes.in_flow_block_entries_dropped = true;
        notes.prepared_inline_content_dropped = true;
        if !notes.run_cache_entries_dropped {
            notes.fragment_cache_epoch_bumped = true;
            notes.generation = node.generation();
        }
    }

    /// The child list of `node` changed, which run cache entries holding line data count.
    pub(crate) fn note_structural_inline_change(&mut self, node: NodeSlotId) {
        let notes = self.notes_of(node.slot_index());
        if !notes.run_cache_entries_dropped {
            notes.structural_inline_changes += 1;
            notes.generation = node.generation();
        }
    }

    /// Applies the notes to the memos, in the order the boxes were first noted.
    pub(crate) fn apply_to(
        self,
        run_cache: &mut FcRunCacheStore,
        in_flow_block_cache: &mut InFlowBlockCacheStore,
        text_memos: &mut TextMemos,
        prepared_inline_content: &mut PreparedInlineContentMemos,
        intrinsic_size_caches: &mut IntrinsicSizeCaches,
    ) {
        for text_state in self.dropped_text_states {
            text_memos.forget(text_state);
        }
        for slot in self.noted_slots {
            let notes = self.pages[slot as usize / NOTES_PER_PAGE]
                .as_ref()
                .expect("a noted slot has its page")[slot as usize % NOTES_PER_PAGE];
            if notes.run_cache_entries_dropped {
                run_cache.remove_entry(slot);
            } else if notes.structural_inline_changes > 0 || notes.fragment_cache_epoch_bumped {
                let node = NodeSlotId::new(slot, notes.generation);
                for _ in 0..notes.structural_inline_changes {
                    run_cache.note_inline_layout_damage(node);
                }
                if notes.fragment_cache_epoch_bumped {
                    run_cache.note_invalidated_entry(node);
                }
            }
            if notes.in_flow_block_entries_dropped {
                in_flow_block_cache.remove_entry(slot);
            }
            if notes.prepared_inline_content_dropped {
                prepared_inline_content.forget(slot);
            }
            if notes.intrinsic_sizes_dropped {
                intrinsic_size_caches.drop_entries_of_slot(slot);
            }
        }
    }
}
