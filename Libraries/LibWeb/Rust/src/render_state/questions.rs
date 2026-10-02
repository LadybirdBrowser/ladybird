/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The questions the host asks a document's render state, and the answers it waits for: writes the host pays for, and
//! the rows as of every change the host queued.

use super::{DocumentHost, RenderMessage, RenderWait, wait_for_render_state};
use crate::layout::LayoutNodeArena;
use crate::layout::layout_changes::{LayoutWrite, LayoutWritten};
use crate::layout::row_reads::RowSnapshot;

/// A question the host asks about a document's render state, answered as of every change it queued before.
pub(crate) enum Query {
    /// A write to the document's layout tree the host waits for, answered with what it owes the host.
    Write(LayoutWrite),
    /// The document's rows, which the render state publishes: with every row's scrollable overflow measured first
    /// where `measure_overflow`.
    CommittedRows { measure_overflow: bool },
}

/// The answer to a [`Query`].
pub(crate) enum Answer {
    Written(LayoutWritten),
    Rows(RowSnapshot),
}

impl Query {
    /// Answers the question from `arena`, the arena of the document it was asked about.
    pub(super) fn answer(self, arena: &mut LayoutNodeArena) -> Answer {
        match self {
            Self::Write(write) => Answer::Written(write.apply(arena)),
            Self::CommittedRows { measure_overflow } => Answer::Rows(arena.publish_row_snapshot(measure_overflow)),
        }
    }
}

/// Asks the render state of `host`'s document `query`, spending `wait`, and answers what it answered.
pub(crate) fn ask(wait: impl RenderWait, host: &DocumentHost, query: Query) -> Answer {
    let document = host.document();
    wait_for_render_state(wait, host, |reply| RenderMessage::Ask { document, query, reply })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeSlotId;
    use crate::render_state::ScriptForcedRead;

    fn host() -> (*mut DocumentHost, &'static DocumentHost) {
        let pointer = crate::render_state::document_host::document_host_create(0);
        // SAFETY: The host lives until the test destroys it.
        (pointer, unsafe { &*pointer })
    }

    fn arena_of(host: &DocumentHost) -> &'static mut LayoutNodeArena {
        // SAFETY: The arena lives as long as the host's render state, and the test reaches it only between questions.
        unsafe { &mut *crate::render_state::arena_for_unconverted_entry(host.document()).cast::<LayoutNodeArena>() }
    }

    fn destroy(pointer: *mut DocumentHost) {
        // SAFETY: The host is destroyed once, and nothing reaches it after.
        unsafe { crate::render_state::document_host::document_host_destroy(pointer) };
    }

    #[test]
    fn a_question_about_the_rows_answers_them_as_of_the_arena() {
        let (pointer, host) = host();
        let row = arena_of(host).allocate_for_test().slot;
        let Answer::Rows(rows) = ask(
            ScriptForcedRead::for_test(),
            host,
            Query::CommittedRows { measure_overflow: true },
        ) else {
            panic!("rows are answered with rows");
        };
        assert!(rows.node(row).is_some());
        assert!(rows.overflow_is_measured());
        arena_of(host)
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        destroy(pointer);
    }

    #[test]
    fn a_write_is_answered_with_what_it_owes_the_host() {
        let (pointer, host) = host();
        let arena = arena_of(host);
        let parent = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(parent, child, NodeSlotId::INVALID);
        let Answer::Written(written) = ask(
            ScriptForcedRead::for_test(),
            host,
            Query::Write(LayoutWrite::DropSubtree { root: parent }),
        ) else {
            panic!("a write is answered with what it wrote");
        };
        assert!(!written.was_attached);
        written
            .host_work
            .apply(&crate::stage::MainThread::for_test(), arena_of(host));
        assert_eq!(arena_of(host).live_slot_count(), 0);
        destroy(pointer);
    }
}
