/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The reads the paint side makes of a document.
//!
//! [`GeometryRead`] names what a box's geometry is computed from: its committed rows and the few
//! facts of its layout node that say how to read them. [`PaintRead`] adds the layout tree shape
//! and style reads painting makes. Paint order, node painting and the paintable geometry helpers
//! are written against these traits rather than against the live [`LayoutNodeArena`], so the
//! types say which reads painting depends on.

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_pixels::CssPixelRect;
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::fragment_tree::FragmentLink;
use crate::layout::node_data::{CompositorAnimationFrameKind, DomPaintFact, NodeFlag, NodeKind, NodeSlotId, PaintNode};
use crate::layout::node_facts::{self, NodeShape};
use crate::layout::text_chunker::GraphemeSegmenter;
use crate::layout::{RenderedText, RenderedTextBoundary, TextFragments};
use crate::painting::host::FfiLayerImageList;
use crate::painting::layer_image_paint_facts::{LayerImagePaintFacts, layer_image_paint_facts_in};
use crate::painting::paint_order_plan::PaintOrderInputs;
use crate::painting::paintable_data::{CommittedSideData, PaintableData};
use crate::painting::published_frame::{PublishedFrame, PublishedRows};
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::replaced_paint_facts::ReplacedPaintFacts;
use crate::painting::stacking_context::entries::StackingContextEntries;
use crate::painting::svg_paint_resources::{
    PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind, published_filter_in, published_paint_server_in,
};
use crate::painting::visual_context::BoxVisualContextNodeHandles;
use std::cell::RefCell;
use std::ops::Deref;
use std::sync::Arc;

/// What a reader knows of a node of the layout tree that is live. Holding the row says so, so none
/// of its reads checks again.
pub(crate) trait PaintRow<'a>: NodeShape + Copy {
    fn generated_for(self) -> u8;
    fn dom_paint_facts(self) -> u8;
    fn compositor_animation_frame_kinds(self) -> u8;
    fn parent(self) -> NodeSlotId;
    fn first_child(self) -> NodeSlotId;
    fn next_sibling(self) -> NodeSlotId;
    fn style(self) -> Option<ComputedValuesView<'a>>;
    /// The node whose style the row carries.
    fn style_node(self) -> Option<StyleNodeID>;

    fn is_out_of_flow(self) -> bool {
        node_facts::node_is_out_of_flow(&self, self.style())
    }

    fn is_atomic_inline(self) -> bool {
        node_facts::node_is_atomic_inline(&self, self.style())
    }

    fn is_positioned(self) -> bool {
        node_facts::node_is_positioned(&self, self.style())
    }

    fn is_floating(self) -> bool {
        node_facts::node_is_floating(&self, self.style())
    }

    fn is_fragmented_inline(self) -> bool {
        node_facts::node_is_fragmented_inline(&self, self.style())
    }
}

impl<'a> PaintRow<'a> for &'a PaintNode {
    fn generated_for(self) -> u8 {
        self.generated_for
    }

    fn dom_paint_facts(self) -> u8 {
        self.dom_paint_facts
    }

    fn compositor_animation_frame_kinds(self) -> u8 {
        self.compositor_animation_frame_kinds
    }

    fn parent(self) -> NodeSlotId {
        self.parent
    }

    fn first_child(self) -> NodeSlotId {
        self.first_child
    }

    fn next_sibling(self) -> NodeSlotId {
        self.next_sibling
    }

    fn style(self) -> Option<ComputedValuesView<'a>> {
        PaintNode::style(self)
    }

    fn style_node(self) -> Option<StyleNodeID> {
        self.style_node
    }
}

/// A node's link to another, which is none for an invalid slot.
fn link(link: NodeSlotId) -> Option<NodeSlotId> {
    (!link.is_invalid()).then_some(link)
}

pub(crate) trait GeometryRead: Sized {
    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData;
    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool;
    /// Reads the fragment link a populated row committed.
    fn with_committed_fragment_link<R>(&self, id: NodeSlotId, read: impl FnOnce(Option<&FragmentLink>) -> R) -> R;
    /// The side data a populated row committed.
    fn committed_side_data(&self, id: NodeSlotId) -> impl Deref<Target = CommittedSideData> + '_;

    type Row<'a>: PaintRow<'a>
    where
        Self: 'a;
    /// The row of the node `id` names, or `None` once the node is gone.
    fn node(&self, id: NodeSlotId) -> Option<Self::Row<'_>>;

    fn node_kind_if_live(&self, id: NodeSlotId) -> Option<NodeKind> {
        Some(self.node(id)?.kind())
    }

    fn node_flags_if_live(&self, id: NodeSlotId) -> u32 {
        self.node(id).map_or(0, |node| node.flags())
    }

    fn node_parent_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        link(self.node(id)?.parent())
    }

    /// The absolute rect memoized for a box, if any.
    fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<CssPixelRect>;
    fn memoize_absolute_rect(&self, id: NodeSlotId, rect: CssPixelRect);

    /// The line root whose side data holds an inline box's pieces.
    fn inline_pieces_root(&self, inline_paintable: NodeSlotId) -> Option<NodeSlotId> {
        if !self.paintable_row_is_populated(inline_paintable) {
            return None;
        }
        let root = self.paintable_data(inline_paintable).containing_block;
        (self.paintable_row_is_populated(root) && crate::painting::node_painting::has_lines(self, root)).then_some(root)
    }
}

/// The reads the display list recording makes of a document beyond a box's geometry: the shape of
/// the layout tree and the style of its nodes.
pub(crate) trait PaintRead: GeometryRead {
    fn slot_is_live(&self, id: NodeSlotId) -> bool {
        self.node(id).is_some()
    }

    fn node_first_child_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        link(self.node(id)?.first_child())
    }

    fn node_next_sibling_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        link(self.node(id)?.next_sibling())
    }

    /// The children of the node `id` names, in tree order.
    fn children(&self, id: NodeSlotId) -> impl Iterator<Item = NodeSlotId> + '_ {
        std::iter::successors(self.node_first_child_if_live(id), |&child| {
            self.node_next_sibling_if_live(child)
        })
    }

    fn node_generated_for(&self, id: NodeSlotId) -> u8 {
        self.node(id).map_or(0, |node| node.generated_for())
    }

    fn node_is_generated_for_pseudo_element(&self, id: NodeSlotId) -> bool {
        self.node_generated_for(id) != 0
    }

    fn node_has_compositor_animation_frame(&self, id: NodeSlotId, kind: CompositorAnimationFrameKind) -> bool {
        self.node(id)
            .is_some_and(|node| node.compositor_animation_frame_kinds() & kind as u8 != 0)
    }

    fn node_style_if_live(&self, id: NodeSlotId) -> Option<ComputedValuesView<'_>> {
        self.node(id)?.style()
    }

    /// The node whose style the row carries.
    fn node_style_node(&self, id: NodeSlotId) -> Option<StyleNodeID> {
        self.node(id)?.style_node()
    }
    /// The rendered text of a text row.
    fn rendered_text(&self, id: NodeSlotId) -> Option<&RenderedText>;
    /// Reads a text row's grapheme boundaries, for a text row with rendered text.
    fn with_grapheme_segmenter<R>(&self, id: NodeSlotId, read: impl FnOnce(&GraphemeSegmenter) -> R) -> Option<R>;
    fn published_svg_filter(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgFilter>>;
    fn published_svg_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgPaintServer>>;

    fn node_has_dom_paint_fact(&self, id: NodeSlotId, fact: DomPaintFact) -> bool {
        self.node(id)
            .is_some_and(|node| node.dom_paint_facts() & fact as u8 != 0)
    }
    /// The rows a text node is painted in: its first-letter row, if any, then its own.
    fn text_fragments(&self, primary: NodeSlotId) -> TextFragments;
    fn replaced_paint_facts(&self, id: NodeSlotId) -> Option<ReplacedPaintFacts>;
    fn layer_image_paint_facts(
        &self,
        id: NodeSlotId,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> Option<LayerImagePaintFacts>;
    fn with_paintable_visual_context_node_handles<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(&BoxVisualContextNodeHandles) -> R,
    ) -> R;
    /// The paint-order inputs paint preparation gathered for the row, if it gathered them.
    fn prepared_paint_order_inputs(&self, row: NodeSlotId) -> Option<PaintOrderInputs> {
        self.committed_side_data(row).prepared_order_inputs()
    }
    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_>;

    /// The box whose content box a node is laid out against: an in-flow node's is its nearest
    /// ancestor that forms a containing block for its children, and a positioned box's its nearest
    /// ancestor box that establishes one for its position, or the root for a fixed-position box
    /// without one. Layout and painting both walk to it with this.
    fn node_containing_block_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        use crate::css::css_enums::positioning;
        let kind = self.node_kind_if_live(id)?;
        let position = if node_facts::kind_is_text(kind) {
            positioning::STATIC
        } else {
            crate::painting::style_queries::position(self, id)
        };
        if position != positioning::ABSOLUTE && position != positioning::FIXED {
            let mut ancestor = self.node_parent_if_live(id);
            while let Some(candidate) = ancestor {
                let shape = (self.node_kind_if_live(candidate)?, self.node_flags_if_live(candidate));
                if node_facts::node_forms_containing_block_for_children(&shape, self.node_style_if_live(candidate)) {
                    return Some(candidate);
                }
                ancestor = self.node_parent_if_live(candidate);
            }
            return None;
        }
        let is_fixed_position = position == positioning::FIXED;
        let establishes_containing_block = node_facts::containing_block_establishment_flag(is_fixed_position) as u32;
        let mut current = id;
        while let Some(ancestor) = self.node_parent_if_live(current) {
            current = ancestor;
            if self.node_kind_if_live(current).is_some_and(node_facts::kind_is_box)
                && self.node_flags_if_live(current) & establishes_containing_block != 0
            {
                return Some(current);
            }
        }
        is_fixed_position.then_some(current)
    }

    /// Whether the row was built for a DOM node: an element, a text node or the document.
    /// Anonymous boxes and generated content were not.
    fn node_is_dom_backed(&self, id: NodeSlotId) -> bool {
        // A slot that has not been given a kind yet stands for nothing at all, and its flags do
        // not say so.
        self.node_kind_if_live(id).is_some_and(|kind| kind != NodeKind::Unset)
            && self.node_flags_if_live(id) & NodeFlag::Anonymous as u32 == 0
    }

    /// Whether the row was built for an element, as opposed to the document, a text node or
    /// nothing.
    fn node_is_element_backed(&self, id: NodeSlotId) -> bool {
        self.node_is_dom_backed(id)
            && self
                .node_kind_if_live(id)
                .is_some_and(|kind| kind != NodeKind::Viewport && !node_facts::kind_is_text(kind))
    }

    fn dom_offset_for_rendered_text_offset(
        &self,
        id: NodeSlotId,
        offset: usize,
        boundary: RenderedTextBoundary,
    ) -> usize {
        if !self.node_kind_if_live(id).is_some_and(node_facts::kind_is_text) {
            return offset;
        }
        self.rendered_text(id)
            .expect("text must be published before mapping rendered offsets")
            .dom_offset_for_rendered_text_offset(offset, boundary)
    }

    fn rendered_text_offset_for_dom_offset(
        &self,
        id: NodeSlotId,
        offset: usize,
        boundary: RenderedTextBoundary,
    ) -> usize {
        if !self.node_kind_if_live(id).is_some_and(node_facts::kind_is_text) {
            return offset;
        }
        self.rendered_text(id)
            .expect("text must be published before mapping DOM offsets")
            .rendered_text_offset_for_dom_offset(offset, boundary)
    }

    /// Visits the layout subtree under `root` in pre-order, descending into a node's children only
    /// when the visit asks to.
    fn for_each_node_in_layout_subtree_in_pre_order_with_pruning(
        &self,
        root: NodeSlotId,
        mut visit_node_and_report_whether_to_descend: impl FnMut(NodeSlotId) -> bool,
    ) {
        let mut current = root;
        loop {
            let descend_into_children = visit_node_and_report_whether_to_descend(current);
            if descend_into_children && let Some(first_child) = self.node_first_child_if_live(current) {
                current = first_child;
                continue;
            }
            if current == root {
                break;
            }
            if let Some(next_sibling) = self.node_next_sibling_if_live(current) {
                current = next_sibling;
                continue;
            }
            let Some(parent) = self.node_parent_if_live(current) else {
                debug_assert!(false, "a node under the walked root has no parent");
                return;
            };
            current = parent;
            while current != root {
                if let Some(next_sibling) = self.node_next_sibling_if_live(current) {
                    current = next_sibling;
                    break;
                }
                let Some(parent) = self.node_parent_if_live(current) else {
                    debug_assert!(false, "a node under the walked root has no parent");
                    return;
                };
                current = parent;
            }
            if current == root {
                break;
            }
        }
    }
}

impl AsRef<LayoutNodeArena> for LayoutNodeArena {
    fn as_ref(&self) -> &LayoutNodeArena {
        self
    }
}

// The live arena, and every view that borrows it, answers each read with the arena's own method of
// the same name.
impl<Live: AsRef<LayoutNodeArena>> GeometryRead for Live {
    type Row<'a>
        = crate::layout::LiveRow<'a>
    where
        Self: 'a;

    fn node(&self, id: NodeSlotId) -> Option<Self::Row<'_>> {
        self.as_ref().live_row(id)
    }

    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        self.as_ref().live_paintable_data(id)
    }

    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        self.as_ref().paintable_row_is_populated(id)
    }

    fn with_committed_fragment_link<R>(&self, id: NodeSlotId, read: impl FnOnce(Option<&FragmentLink>) -> R) -> R {
        self.as_ref().with_committed_fragment_link(id, read)
    }

    fn committed_side_data(&self, id: NodeSlotId) -> impl Deref<Target = CommittedSideData> + '_ {
        self.as_ref().committed_side_data(id)
    }

    fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<CssPixelRect> {
        self.as_ref().memoized_absolute_rect(id)
    }

    fn memoize_absolute_rect(&self, id: NodeSlotId, rect: CssPixelRect) {
        self.as_ref().memoize_absolute_rect(id, rect);
    }
}

impl<Live: AsRef<LayoutNodeArena>> PaintRead for Live {
    fn rendered_text(&self, id: NodeSlotId) -> Option<&RenderedText> {
        self.as_ref().text_content(id).map(|content| &**content)
    }

    fn with_grapheme_segmenter<R>(&self, id: NodeSlotId, read: impl FnOnce(&GraphemeSegmenter) -> R) -> Option<R> {
        self.as_ref()
            .text_content(id)
            .map(|content| read(content.grapheme_segmenter()))
    }

    fn text_fragments(&self, primary: NodeSlotId) -> TextFragments {
        self.as_ref().text_fragments(primary)
    }

    fn published_svg_filter(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgFilter>> {
        self.as_ref().svg_paint_resources().published_filter(slot, kind)
    }

    fn published_svg_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgPaintServer>> {
        self.as_ref().svg_paint_resources().published_paint_server(slot, kind)
    }

    fn replaced_paint_facts(&self, id: NodeSlotId) -> Option<ReplacedPaintFacts> {
        self.as_ref().replaced_paint_facts(id)
    }

    fn layer_image_paint_facts(
        &self,
        id: NodeSlotId,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> Option<LayerImagePaintFacts> {
        self.as_ref().layer_image_paint_facts(id, list, computed_index)
    }

    fn with_paintable_visual_context_node_handles<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(&BoxVisualContextNodeHandles) -> R,
    ) -> R {
        self.as_ref().with_paintable_visual_context_node_handles(id, read)
    }

    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_> {
        self.as_ref().stacking_context_entries(root)
    }
}

/// What the display list recording reads a document through: [`PaintRead`] and nothing else. It
/// has no `Deref` to the arena, so a read the trait does not name does not compile, and it reads
/// everything from the frame the document published when the recording started. The absolute rects
/// it computes go to the recorder's own memo, stamped with the geometry epoch of the frame. The
/// host reads the rows its document's render state published through one too, without a frame.
#[derive(Clone, Copy)]
pub(crate) struct PaintSource<'a> {
    rows: &'a PublishedRows,
    /// The frame a recording reads, or none for the host reading published rows.
    frame: Option<&'a PublishedFrame>,
    absolute_rects: &'a RefCell<AbsoluteRectMemo>,
}

impl<'a> PaintSource<'a> {
    pub(crate) fn new(frame: &'a PublishedFrame, absolute_rects: &'a RefCell<AbsoluteRectMemo>) -> Self {
        Self {
            rows: &frame.rows,
            frame: Some(frame),
            absolute_rects,
        }
    }

    /// Reads `rows` alone, as the host reads the rows its document's render state published.
    pub(crate) fn over_rows(rows: &'a PublishedRows, absolute_rects: &'a RefCell<AbsoluteRectMemo>) -> Self {
        Self {
            rows,
            frame: None,
            absolute_rects,
        }
    }

    /// The frame the recording reads.
    pub(crate) fn frame(&self) -> &'a PublishedFrame {
        self.frame.expect("only a recording reads a frame")
    }

    fn rows(&self) -> &'a PublishedRows {
        self.rows
    }

    pub(crate) fn paint_damage_of_row(&self, row: NodeSlotId) -> PaintDamage {
        self.frame().damage().of_row(row)
    }

    pub(crate) fn damaged_paint_rows(&self) -> impl Iterator<Item = NodeSlotId> + 'a {
        self.frame().damage().rows()
    }
}

impl GeometryRead for PaintSource<'_> {
    type Row<'a>
        = &'a PaintNode
    where
        Self: 'a;

    fn node(&self, id: NodeSlotId) -> Option<&PaintNode> {
        self.rows().node(id)
    }

    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        self.rows().paintable_data(id)
    }

    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        self.rows().paintable_row_is_populated(id)
    }

    fn with_committed_fragment_link<R>(&self, id: NodeSlotId, read: impl FnOnce(Option<&FragmentLink>) -> R) -> R {
        self.rows().with_committed_fragment_link(id, read)
    }

    fn committed_side_data(&self, id: NodeSlotId) -> impl Deref<Target = CommittedSideData> + '_ {
        self.rows().committed_side_data(id)
    }

    fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<CssPixelRect> {
        self.absolute_rects.borrow().get(id, self.rows().geometry_epoch)
    }

    fn memoize_absolute_rect(&self, id: NodeSlotId, rect: CssPixelRect) {
        self.absolute_rects
            .borrow_mut()
            .set(id, self.rows().geometry_epoch, rect);
    }
}

impl PaintRead for PaintSource<'_> {
    fn with_paintable_visual_context_node_handles<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(&BoxVisualContextNodeHandles) -> R,
    ) -> R {
        read(self.rows().visual_context_node_handles(id))
    }

    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_> {
        self.rows().stacking_context_entries(root)
    }

    fn rendered_text(&self, id: NodeSlotId) -> Option<&RenderedText> {
        self.rows().text(id)?.rendered.as_deref()
    }

    fn with_grapheme_segmenter<R>(&self, id: NodeSlotId, read: impl FnOnce(&GraphemeSegmenter) -> R) -> Option<R> {
        self.rendered_text(id)
            .map(|rendered| read(&GraphemeSegmenter::new(&rendered.text)))
    }

    fn text_fragments(&self, primary: NodeSlotId) -> TextFragments {
        let mut fragments = TextFragments {
            nodes: [NodeSlotId::INVALID; 2],
            length: 0,
        };
        if !self.node_kind_if_live(primary).is_some_and(node_facts::kind_is_text) {
            return fragments;
        }
        if let Some(text) = self.rows().text(primary)
            && self.slot_is_live(text.first_letter)
        {
            fragments.nodes[0] = text.first_letter;
            fragments.length = 1;
        }
        fragments.nodes[fragments.length] = primary;
        fragments.length += 1;
        fragments
    }

    fn published_svg_filter(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgFilter>> {
        published_filter_in(&self.rows().paint_facts.svg_paint_resources, slot, kind)
    }

    fn published_svg_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgPaintServer>> {
        published_paint_server_in(&self.rows().paint_facts.svg_paint_resources, slot, kind)
    }

    fn replaced_paint_facts(&self, id: NodeSlotId) -> Option<ReplacedPaintFacts> {
        self.rows().paint_facts.replaced.get(&id).cloned()
    }

    fn layer_image_paint_facts(
        &self,
        id: NodeSlotId,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> Option<LayerImagePaintFacts> {
        layer_image_paint_facts_in(&self.rows().paint_facts.layer_images, id, list, computed_index)
    }
}
