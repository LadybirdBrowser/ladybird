/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text rows paint timing has not looked at yet, which is how it learns that it paints a text node for the first
//! time without the host noting anything as a text node is inserted or its data changes. The arena notes a text row
//! each time the row is given rendered text it did not have, which every row a tree build makes is once, and a row is
//! again when what it renders changes. Paint timing takes the rows once a frame is laid out, and the host knows from a
//! flag the arena shares with it whether there are any, so a frame that made no text rows asks nothing.

use super::LayoutNodeArena;
use super::node_data::{NodeKind, NodeSlotId};
use crate::fork::ForkReset;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// The fewest rows the noted rows are compacted at, whose dead and repeated rows are dropped as they grow while paint
/// timing takes none, as it does for a document that renders no frames.
const FEWEST_ROWS_TO_COMPACT: usize = 4096;

#[derive(Default)]
struct NotedRows {
    rows: Vec<NodeSlotId>,
    /// How many rows the rows are compacted at, once they grow to it.
    compact_at: usize,
}

/// The text rows of the arena that were given new rendered text since paint timing last took them.
#[derive(Clone, Default)]
pub(crate) struct FirstPaintTextRows {
    /// A fork starts with no rows: paint timing takes the rows of the state the host writes.
    noted: ForkReset<NotedRows>,
    /// Whether paint timing will never take the rows again, which it says once it reports nothing more about text.
    stopped: bool,
    /// Raised while any row is noted, which the host reads without asking.
    any_noted: Arc<AtomicBool>,
}

impl FirstPaintTextRows {
    pub(crate) fn share_any_noted_flag(&mut self, flag: Arc<AtomicBool>) {
        self.any_noted = flag;
    }

    /// Gives a fork a flag of its own, which no host reads, and stops the fork noting rows, which paint timing never
    /// takes from it.
    pub(crate) fn detach_any_noted_flag_for_fork(&mut self) {
        self.any_noted = Arc::default();
        self.stopped = true;
    }
}

impl LayoutNodeArena {
    /// Notes that the row `id` was given rendered text it did not have, for paint timing to look at.
    pub(super) fn note_text_row_for_first_paint(&mut self, id: NodeSlotId) {
        if self.first_paint_text_rows.stopped || self.data(id).kind.get() != NodeKind::TextNode {
            return;
        }
        let mut noted = std::mem::take(&mut *self.first_paint_text_rows.noted);
        if noted.rows.len() >= noted.compact_at.max(FEWEST_ROWS_TO_COMPACT) {
            noted.rows.retain(|&row| self.slot_is_live(row));
            noted.rows.sort_unstable_by_key(|row| row.index);
            noted.rows.dedup();
            noted.compact_at = noted.rows.len() * 2;
        }
        if noted.rows.is_empty() {
            self.first_paint_text_rows.any_noted.store(true, Ordering::Relaxed);
        }
        noted.rows.push(id);
        *self.first_paint_text_rows.noted = noted;
    }

    /// Takes the text rows noted since the last take that are still live, each as the row its text node is bound to,
    /// which is the text node's layout node, once and in the order of their slot ids.
    pub(crate) fn take_text_rows_for_first_paint(&mut self) -> Vec<NodeSlotId> {
        let noted = std::mem::take(&mut *self.first_paint_text_rows.noted);
        self.first_paint_text_rows.any_noted.store(false, Ordering::Relaxed);
        let mut rows: Vec<NodeSlotId> = noted
            .rows
            .into_iter()
            .filter_map(|row| self.node_style_node(row))
            .map(|text| self.bound_row(text))
            .filter(|&row| self.slot_is_live(row) && self.data(row).kind.get() == NodeKind::TextNode)
            .collect();
        rows.sort_unstable_by_key(|row| row.index);
        rows.dedup();
        rows
    }

    /// Stops noting text rows for paint timing, which will never take them again.
    pub(crate) fn stop_noting_text_rows_for_first_paint(&mut self) {
        self.first_paint_text_rows.stopped = true;
        *self.first_paint_text_rows.noted = NotedRows::default();
        self.first_paint_text_rows.any_noted.store(false, Ordering::Relaxed);
    }
}
