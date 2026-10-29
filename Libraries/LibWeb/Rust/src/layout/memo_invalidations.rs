/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the arena's writes leave the layout stage's run memos to drop or reconsider. The memos live in the stage's
//! scratch, which the arena cannot reach, so the arena notes what a write did to a box and the stage applies the notes
//! to its memos before a layout pass reads them and before it sweeps them.
//!
//! The notes of a box coalesce. Only a layout pass stores memo entries, and a stage applies the notes before each pass,
//! so between two applications a box's entries change only through the notes: entries a note drops stay dropped, and
//! the notes after it see none.

use super::fc_run_cache::FcRunCacheStore;
use super::in_flow_block_cache::InFlowBlockCacheStore;
use super::node_data::NodeSlotId;
use super::rendered_text::TextMemos;

#[derive(Clone, Copy, Default)]
struct SlotNotes {
    noted: bool,
    text_memos_dropped: bool,
    run_cache_entries_dropped: bool,
    in_flow_block_entries_dropped: bool,
    fragment_cache_epoch_bumped: bool,
    structural_inline_changes: u32,
    generation: u8,
}

/// The notes of each box the arena wrote since the stage last applied them, in the order the boxes were first noted.
#[derive(Clone, Default)]
pub(crate) struct MemoInvalidations {
    slots: Vec<SlotNotes>,
    noted_slots: Vec<u32>,
}

impl MemoInvalidations {
    fn notes_of(&mut self, slot: u32) -> &mut SlotNotes {
        let index = slot as usize;
        if self.slots.len() <= index {
            self.slots.resize_with(index + 1, SlotNotes::default);
        }
        let notes = &mut self.slots[index];
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
        notes.text_memos_dropped = true;
        notes.in_flow_block_entries_dropped = true;
    }

    /// The text row in `slot` got other rendered text, which the chunks and segmenter made of the old text no longer
    /// describe.
    pub(crate) fn note_text_replaced(&mut self, slot: u32) {
        self.notes_of(slot).text_memos_dropped = true;
    }

    /// The fragment cache epoch of `node` moved on: its run cache entries need the sweep, and its in-flow block entry,
    /// valid for one epoch only, is dropped.
    pub(crate) fn note_fragment_cache_epoch_bumped(&mut self, node: NodeSlotId) {
        let notes = self.notes_of(node.slot_index());
        notes.in_flow_block_entries_dropped = true;
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
        run_cache: &FcRunCacheStore,
        in_flow_block_cache: &InFlowBlockCacheStore,
        text_memos: &TextMemos,
    ) {
        for slot in self.noted_slots {
            let notes = self.slots[slot as usize];
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
            if notes.text_memos_dropped {
                text_memos.forget(slot);
            }
        }
    }
}
