/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rows of a document's layout as the host reads them: what each row is, its links, its style, and the paintable
//! rows a recording reads. The render state publishes them as one [`RowSnapshot`] when the host asks, and the host
//! keeps the latest one beside it (see [`crate::render_state::DocumentHost::rows`]), reading neither the arena nor the
//! render state, unless the rows were written since: then it asks for them again first.

use super::LayoutNodeArena;
use super::RowsVersion;
use super::host_tables::ShellFacts;
use super::node_data::{CompositorAnimationFrameKind, FfiNodeLink, NodeKind, NodeSlotId, PaintNode, StylePayloadsRef};
use super::node_facts;
use crate::css::style::tree::StyleNodeID;
use crate::painting::published_frame::PublishedRows;

/// The rows of a document's layout as its render state published them. It is immutable and owns all of it, through the
/// copy-on-write generations the arena's columns publish, so the host reads it while the arena goes on changing.
pub(crate) struct RowSnapshot {
    /// The layout tree's shape and the paintable rows, and the columns read beside them.
    pub(crate) paintable: PublishedRows,
    /// How far the arena's rows had been written when they were published.
    version: RowsVersion,
}

// The host reads a snapshot while the render state writes the arena it was published from: it holds no cell, no
// borrow and no handle of the arena.
const _: () = {
    const fn assert_published<T: Send + Sync + 'static>() {}
    assert_published::<RowSnapshot>();
};

impl RowSnapshot {
    /// Whether the rows read as the arena's do at `version`.
    pub(crate) fn reads_as(&self, version: RowsVersion) -> bool {
        self.version == version
    }

    /// The row in slot `id`, if the slot held it when the rows were published.
    pub(crate) fn node(&self, id: NodeSlotId) -> Option<&PaintNode> {
        self.paintable.node(id)
    }

    /// The row in slot `id`, which a layout node asks about only while the row lives.
    fn live_node(&self, id: NodeSlotId) -> &PaintNode {
        self.node(id).expect("a layout node reads a live row")
    }

    pub(crate) fn flags(&self, id: NodeSlotId) -> u32 {
        self.live_node(id).flags
    }

    pub(crate) fn link(&self, id: NodeSlotId, link: FfiNodeLink) -> NodeSlotId {
        let node = self.live_node(id);
        match link {
            FfiNodeLink::Parent => node.parent,
            FfiNodeLink::FirstChild => node.first_child,
            FfiNodeLink::LastChild => node.last_child,
            FfiNodeLink::PreviousSibling => node.previous_sibling,
            FfiNodeLink::NextSibling => node.next_sibling,
        }
    }

    pub(crate) fn generated_for(&self, id: NodeSlotId) -> u8 {
        self.live_node(id).generated_for
    }

    pub(crate) fn has_compositor_animation_frame(&self, id: NodeSlotId, kind: CompositorAnimationFrameKind) -> bool {
        self.live_node(id).compositor_animation_frame_kinds & kind as u8 != 0
    }

    /// The node whose style the row in slot `id` carries, or none for a row that is gone.
    pub(crate) fn style_node(&self, id: NodeSlotId) -> Option<StyleNodeID> {
        self.node(id)?.style_node
    }

    pub(crate) fn style_record(&self, id: NodeSlotId) -> u64 {
        self.live_node(id).style_record
    }

    pub(crate) fn style_payloads(&self, id: NodeSlotId) -> StylePayloadsRef {
        self.live_node(id).style
    }

    pub(crate) fn is_atomic_inline(&self, id: NodeSlotId) -> bool {
        let node = self.live_node(id);
        node_facts::node_is_atomic_inline(node, node.style())
    }

    pub(crate) fn is_fragmented_inline(&self, id: NodeSlotId) -> bool {
        let node = self.live_node(id);
        node_facts::node_is_fragmented_inline(node, node.style())
    }

    /// What the shell factory needs to make the layout node of the row in slot `id`, or none for a row that is gone or
    /// has no layout node.
    pub(crate) fn shell_facts(&self, id: NodeSlotId) -> Option<ShellFacts> {
        let kind = self.node(id)?.kind;
        (kind != NodeKind::Unset).then_some(ShellFacts { id, kind })
    }
}

impl LayoutNodeArena {
    /// The rows as they are now, for the host to read. A host callback a layout pass makes reads the rows as the pass
    /// has left them so far.
    pub(crate) fn publish_row_snapshot(&mut self) -> RowSnapshot {
        let paintable = self.publish_rows();
        RowSnapshot {
            paintable,
            version: self.rows_version(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_snapshot_holds_the_rows_as_they_were_when_published() {
        let mut arena = LayoutNodeArena::new();
        let row = arena.allocate_for_test().slot;
        arena.write_shape(row).set_kind(NodeKind::BlockContainer);
        let before = arena.publish_row_snapshot();
        assert!(before.reads_as(arena.rows_version()));
        arena.write_shape(row).set_kind(NodeKind::InlineNode);
        assert!(!before.reads_as(arena.rows_version()));
        let after = arena.publish_row_snapshot();
        assert_eq!(before.node(row).map(|node| node.kind), Some(NodeKind::BlockContainer));
        assert_eq!(after.node(row).map(|node| node.kind), Some(NodeKind::InlineNode));
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.publish_row_snapshot().node(row).is_none());
        assert!(after.node(row).is_some());
    }

    #[test]
    fn a_snapshot_reads_the_links_a_layout_node_asks_about() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test().slot;
        let first = arena.allocate_for_test().slot;
        let last = arena.allocate_for_test().slot;
        arena.insert_child(parent, first, NodeSlotId::INVALID);
        arena.insert_child(parent, last, NodeSlotId::INVALID);
        let rows = arena.publish_row_snapshot();
        assert_eq!(rows.link(parent, FfiNodeLink::FirstChild), first);
        assert_eq!(rows.link(parent, FfiNodeLink::LastChild), last);
        assert_eq!(rows.link(last, FfiNodeLink::PreviousSibling), first);
        assert_eq!(rows.link(first, FfiNodeLink::NextSibling), last);
        assert_eq!(rows.link(last, FfiNodeLink::Parent), parent);
        arena.detach_child(parent, first);
        assert!(!rows.reads_as(arena.rows_version()));
        assert_eq!(
            arena.publish_row_snapshot().link(last, FfiNodeLink::PreviousSibling),
            NodeSlotId::INVALID
        );
        let main_thread = crate::stage::MainThread::for_test();
        arena
            .free_subtree(first)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        arena
            .free_subtree(parent)
            .destroy_shells_and_invoke_callbacks(&main_thread);
    }
}
