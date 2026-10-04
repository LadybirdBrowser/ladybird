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
use super::layout_node_arena::PublishedBoundRows;
use super::node_data::{
    CompositorAnimationFrameKind, FfiNodeLink, NodeFlag, NodeKind, NodeSlotId, PaintNode, StylePayloadsRef,
};
use super::node_facts;
use crate::css::style::tree::StyleNodeID;
use crate::painting::image_map_areas::ImageMaps;
use crate::painting::published_frame::PublishedRows;
use crate::painting::visual_context::VisualContextTree;
use std::rc::Rc;
use std::sync::Arc;

/// The rows of a document's layout as its render state published them. It is immutable and owns all of it, through the
/// copy-on-write generations the arena's columns publish, so the host reads it while the arena goes on changing.
pub(crate) struct RowSnapshot {
    /// The layout tree's shape and the paintable rows, and the columns read beside them.
    pub(crate) paintable: PublishedRows,
    /// The row each node is bound to.
    bound: PublishedBoundRows,
    /// The `<area>` elements of the image map of every image that has one.
    image_maps: Arc<ImageMaps>,
    /// The visual context tree hit testing maps points through.
    pub(crate) visual_context_tree: Option<Arc<VisualContextTree>>,
    /// How far the arena's rows had been written when they were published.
    version: RowsVersion,
    /// Whether every row's scrollable overflow was measured when the rows were published.
    overflow_is_measured: bool,
}

// The host reads a snapshot while the render state writes the arena it was published from: it holds no cell, no
// borrow and no handle of the arena.
const _: () = {
    const fn assert_published<T: Send + Sync + 'static>() {}
    assert_published::<RowSnapshot>();
};

impl RowSnapshot {
    /// How far the arena's rows had been written when they were published.
    pub(crate) fn version(&self) -> RowsVersion {
        self.version
    }

    /// Whether the rows read as the arena's do at `version`.
    pub(crate) fn reads_as(&self, version: RowsVersion) -> bool {
        self.version == version
    }

    /// Whether the rows answer what each row is, and the row each node is bound to, as the arena's do at the identity
    /// version `identity` (see [`LayoutNodeArena::rows_identity_version`]).
    pub(crate) fn reads_identity_as(&self, identity: u64) -> bool {
        self.version.has_identity_version(identity)
    }

    /// Whether the rows answer which rows are populated as the arena's do at `version`.
    pub(crate) fn reads_population_as(&self, version: RowsVersion) -> bool {
        self.version.has_population_of(version)
    }

    /// Whether the rows answer what each row is, and the style record it has, as the arena's do at `version`.
    pub(crate) fn reads_styles_as(&self, version: RowsVersion) -> bool {
        self.version.has_styles_of(version)
    }

    /// Whether every row's scrollable overflow was measured when the rows were published, which a read of overflow
    /// needs.
    pub(crate) fn overflow_is_measured(&self) -> bool {
        self.overflow_is_measured
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

    /// Whether the arena derived the row's style record.
    pub(crate) fn style_is_derived(&self, id: NodeSlotId) -> bool {
        self.live_node(id).style_is_derived
    }

    pub(crate) fn is_atomic_inline(&self, id: NodeSlotId) -> bool {
        let node = self.live_node(id);
        node_facts::node_is_atomic_inline(node, node.style())
    }

    pub(crate) fn is_fragmented_inline(&self, id: NodeSlotId) -> bool {
        let node = self.live_node(id);
        node_facts::node_is_fragmented_inline(node, node.style())
    }

    /// The live row the element or text node with `style_node` is bound to.
    pub(crate) fn bound_row(&self, style_node: StyleNodeID) -> Option<NodeSlotId> {
        self.live(self.bound.row(style_node))
    }

    /// The live row the pseudo-element of kind `generated_for` on `generator` is bound to.
    pub(crate) fn bound_pseudo_element_row(&self, generator: StyleNodeID, generated_for: u8) -> Option<NodeSlotId> {
        self.live(self.bound.pseudo_element_row(generator, generated_for))
    }

    /// The live viewport row the document is bound to.
    pub(crate) fn bound_viewport_row(&self) -> Option<NodeSlotId> {
        self.live(self.bound.viewport_row())
    }

    /// The first area of the map of the image whose paintable row is `slot`, in tree order, whose shape covers the
    /// point, named by its style-tree identity. Zero when the image has no map, or no shape covers the point.
    pub(crate) fn image_map_area_for_point(&self, slot: NodeSlotId, x: f32, y: f32) -> u32 {
        crate::painting::image_map_areas::area_for_point(&self.image_maps, slot, x, y)
    }

    fn live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.node(id).is_some().then_some(id)
    }

    /// What the shell factory needs to make the layout node of the row in slot `id`, or none for a row that is gone or
    /// has no layout node.
    pub(crate) fn shell_facts(&self, id: NodeSlotId) -> Option<ShellFacts> {
        let kind = self.node(id)?.kind;
        (kind != NodeKind::Unset).then_some(ShellFacts { id, kind })
    }
}

/// The style record each row of a document's layout has, and the payloads it keeps, read from published rows, which
/// the host reads from rows published before the writes it made since that give no row a style record (see
/// [`crate::render_state::DocumentHost::row_styles`]), and nothing else of those rows.
pub(crate) struct RowStyles(Rc<RowSnapshot>);

impl RowStyles {
    pub(crate) fn of(rows: Rc<RowSnapshot>) -> Self {
        Self(rows)
    }

    pub(crate) fn style_record(&self, id: NodeSlotId) -> u64 {
        self.0.style_record(id)
    }

    pub(crate) fn style_payloads(&self, id: NodeSlotId) -> StylePayloadsRef {
        self.0.style_payloads(id)
    }

    pub(crate) fn style_is_derived(&self, id: NodeSlotId) -> bool {
        self.0.style_is_derived(id)
    }
}

/// What each row of a document's layout is, and the row each node is bound to, read from published rows. Installing a
/// style changes none of it, so the host reads it from rows published before the styles it installed since (see
/// [`crate::render_state::DocumentHost::row_identities`]), and nothing else of those rows.
pub(crate) struct RowIdentities(Rc<RowSnapshot>);

impl RowIdentities {
    pub(crate) fn of(rows: Rc<RowSnapshot>) -> Self {
        Self(rows)
    }

    /// The row's [`NodeFlag::IDENTITY`] flags.
    pub(crate) fn identity_flags(&self, id: NodeSlotId) -> u32 {
        self.0.flags(id) & NodeFlag::IDENTITY
    }

    pub(crate) fn style_node(&self, id: NodeSlotId) -> Option<StyleNodeID> {
        self.0.style_node(id)
    }

    pub(crate) fn generated_for(&self, id: NodeSlotId) -> u8 {
        self.0.generated_for(id)
    }

    pub(crate) fn shell_facts(&self, id: NodeSlotId) -> Option<ShellFacts> {
        self.0.shell_facts(id)
    }

    pub(crate) fn bound_row(&self, style_node: StyleNodeID) -> Option<NodeSlotId> {
        self.0.bound_row(style_node)
    }

    pub(crate) fn bound_pseudo_element_row(&self, generator: StyleNodeID, generated_for: u8) -> Option<NodeSlotId> {
        self.0.bound_pseudo_element_row(generator, generated_for)
    }

    pub(crate) fn bound_viewport_row(&self) -> Option<NodeSlotId> {
        self.0.bound_viewport_row()
    }
}

impl LayoutNodeArena {
    /// The rows as they are now, for the host to read, once every row's scrollable overflow is measured where
    /// `measure_overflow` says so. A host callback a layout pass makes reads the rows as the pass has left them so far,
    /// and measures nothing: the pass is not done with the geometry overflow is measured from.
    pub(crate) fn publish_row_snapshot(&mut self, measure_overflow: bool) -> RowSnapshot {
        if measure_overflow && !self.layout_pass_is_running() {
            self.measure_scrollable_overflow();
        }
        let paintable = self.publish_rows();
        let bound = self.bound_rows_mut().publish();
        RowSnapshot {
            paintable,
            bound,
            image_maps: self.image_map_areas().snapshot(),
            visual_context_tree: self.paint_state().borrow().visual_context.tree.clone(),
            version: self.rows_version(),
            overflow_is_measured: self.scrollable_overflow_is_measured(),
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
        let before = arena.publish_row_snapshot(false);
        assert!(before.reads_as(arena.rows_version()));
        arena.write_shape(row).set_kind(NodeKind::InlineNode);
        assert!(!before.reads_as(arena.rows_version()));
        let after = arena.publish_row_snapshot(false);
        assert_eq!(before.node(row).map(|node| node.kind), Some(NodeKind::BlockContainer));
        assert_eq!(after.node(row).map(|node| node.kind), Some(NodeKind::InlineNode));
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.publish_row_snapshot(false).node(row).is_none());
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
        let rows = arena.publish_row_snapshot(false);
        assert_eq!(rows.link(parent, FfiNodeLink::FirstChild), first);
        assert_eq!(rows.link(parent, FfiNodeLink::LastChild), last);
        assert_eq!(rows.link(last, FfiNodeLink::PreviousSibling), first);
        assert_eq!(rows.link(first, FfiNodeLink::NextSibling), last);
        assert_eq!(rows.link(last, FfiNodeLink::Parent), parent);
        arena.detach_child(parent, first);
        assert!(!rows.reads_as(arena.rows_version()));
        assert_eq!(
            arena
                .publish_row_snapshot(false)
                .link(last, FfiNodeLink::PreviousSibling),
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
