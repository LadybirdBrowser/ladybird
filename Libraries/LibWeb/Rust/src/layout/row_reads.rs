/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rows of a document's layout as the host reads them: the shape and the paintable rows a recording reads. The
//! render state publishes them as one [`RowSnapshot`] when the host asks, and the host keeps
//! the latest one beside it (see [`crate::render_state::DocumentHost::rows`]), reading neither the arena nor the render
//! state, unless it queued a change that alters them since: then it asks for them again first.

use super::LayoutNodeArena;
use super::node_data::{NodeSlotId, PaintNode};
use crate::painting::published_frame::PublishedRows;

/// The rows of a document's layout as its render state published them. It is immutable and owns all of it, through the
/// copy-on-write generations the arena's columns publish, so the host reads it while the arena goes on changing.
#[cfg_attr(
    not(test),
    expect(dead_code, reason = "the host reads the published rows from the next commit on")
)]
pub(crate) struct RowSnapshot {
    /// The layout tree's shape and the paintable rows, and the columns read beside them.
    pub(crate) paintable: PublishedRows,
}

// The host reads a snapshot while the render state writes the arena it was published from: it holds no cell, no
// borrow and no handle of the arena.
const _: () = {
    const fn assert_published<T: Send + Sync + 'static>() {}
    assert_published::<RowSnapshot>();
};

#[cfg_attr(
    not(test),
    expect(dead_code, reason = "the host reads the published rows from the next commit on")
)]
impl RowSnapshot {
    /// The row in slot `id`, if the slot held it when the rows were published.
    pub(crate) fn node(&self, id: NodeSlotId) -> Option<&PaintNode> {
        self.paintable.node(id)
    }
}

impl LayoutNodeArena {
    /// The rows as they are now, for the host to read.
    pub(crate) fn publish_row_snapshot(&mut self) -> RowSnapshot {
        RowSnapshot {
            paintable: self.publish_rows(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;

    #[test]
    fn a_snapshot_holds_the_rows_as_they_were_when_published() {
        let mut arena = LayoutNodeArena::new();
        let row = arena.allocate_for_test().slot;
        arena.write_shape(row).set_kind(NodeKind::BlockContainer);
        let before = arena.publish_row_snapshot();
        arena.write_shape(row).set_kind(NodeKind::InlineNode);
        let after = arena.publish_row_snapshot();
        assert_eq!(before.node(row).map(|node| node.kind), Some(NodeKind::BlockContainer));
        assert_eq!(after.node(row).map(|node| node.kind), Some(NodeKind::InlineNode));
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.publish_row_snapshot().node(row).is_none());
        assert!(after.node(row).is_some());
    }
}
