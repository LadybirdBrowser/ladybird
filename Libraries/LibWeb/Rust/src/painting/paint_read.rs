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
use crate::layout::LayoutNodeArena;
use crate::layout::fragment_tree::FragmentLink;
use crate::layout::node_data::{CompositorAnimationFrameKind, DomPaintFact, NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::layout::{RenderedTextBoundary, TextContent, TextFragments};
use crate::painting::host::FfiLayerImageList;
use crate::painting::layer_image_paint_facts::LayerImagePaintFacts;
use crate::painting::paint_order_plan::PaintOrderInputs;
use crate::painting::paintable_data::{CommittedSideData, PaintableData};
use crate::painting::published_frame::PublishedRows;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::replaced_paint_facts::ReplacedPaintFacts;
use crate::painting::stacking_context::entries::StackingContextEntries;
use crate::painting::svg_paint_resources::{PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind};
use crate::painting::visual_context::BoxVisualContextNodeHandles;
use std::cell::RefCell;
use std::ops::Deref;
use std::sync::Arc;

pub(crate) trait GeometryRead: Sized {
    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData;
    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool;
    /// Reads the fragment link a populated row committed.
    fn with_committed_fragment_link<R>(&self, id: NodeSlotId, read: impl FnOnce(Option<&FragmentLink>) -> R) -> R;
    /// The side data a populated row committed.
    fn committed_side_data(&self, id: NodeSlotId) -> impl Deref<Target = CommittedSideData> + '_;

    fn node_kind_if_live(&self, id: NodeSlotId) -> Option<NodeKind>;
    fn node_flags_if_live(&self, id: NodeSlotId) -> u32;
    fn node_parent_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId>;
    fn node_is_fragmented_inline(&self, id: NodeSlotId) -> bool;

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
    fn slot_is_live(&self, id: NodeSlotId) -> bool;
    fn node_first_child_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId>;
    fn node_next_sibling_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId>;
    fn node_containing_block_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId>;
    fn node_generated_for(&self, id: NodeSlotId) -> u8;
    fn node_is_generated_for_pseudo_element(&self, id: NodeSlotId) -> bool;
    fn node_is_out_of_flow_if_live(&self, id: NodeSlotId) -> bool;
    fn node_is_atomic_inline(&self, id: NodeSlotId) -> bool;
    fn node_is_positioned(&self, id: NodeSlotId) -> bool;
    fn node_is_floating(&self, id: NodeSlotId) -> bool;
    fn node_has_compositor_animation_frame(&self, id: NodeSlotId, kind: CompositorAnimationFrameKind) -> bool;
    fn node_style_if_live(&self, id: NodeSlotId) -> Option<ComputedValuesView<'_>>;
    /// The rendered text of a text row.
    fn text_content(&self, id: NodeSlotId) -> Option<&TextContent>;
    fn published_svg_filter(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgFilter>>;
    fn published_svg_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgPaintServer>>;

    fn node_has_dom_paint_fact(&self, id: NodeSlotId, fact: DomPaintFact) -> bool;
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
    fn paint_damage_of_row(&self, row: NodeSlotId) -> PaintDamage;
    fn damaged_paint_rows(&self) -> Vec<NodeSlotId>;
    /// The paint-order inputs paint preparation gathered for the row, if it gathered them.
    fn prepared_paint_order_inputs(&self, row: NodeSlotId) -> Option<PaintOrderInputs> {
        self.committed_side_data(row).prepared_order_inputs()
    }
    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_>;

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
        self.text_content(id)
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
        self.text_content(id)
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

    fn node_kind_if_live(&self, id: NodeSlotId) -> Option<NodeKind> {
        self.as_ref().node_kind_if_live(id)
    }

    fn node_flags_if_live(&self, id: NodeSlotId) -> u32 {
        self.as_ref().node_flags_if_live(id)
    }

    fn node_parent_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.as_ref().node_parent_if_live(id)
    }

    fn node_is_fragmented_inline(&self, id: NodeSlotId) -> bool {
        self.as_ref().node_is_fragmented_inline(id)
    }

    fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<CssPixelRect> {
        self.as_ref().memoized_absolute_rect(id)
    }

    fn memoize_absolute_rect(&self, id: NodeSlotId, rect: CssPixelRect) {
        self.as_ref().memoize_absolute_rect(id, rect);
    }
}

impl<Live: AsRef<LayoutNodeArena>> PaintRead for Live {
    fn slot_is_live(&self, id: NodeSlotId) -> bool {
        self.as_ref().slot_is_live(id)
    }

    fn node_first_child_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.as_ref().node_first_child_if_live(id)
    }

    fn node_next_sibling_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.as_ref().node_next_sibling_if_live(id)
    }

    fn node_containing_block_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.as_ref().node_containing_block_if_live(id)
    }

    fn node_generated_for(&self, id: NodeSlotId) -> u8 {
        self.as_ref().node_generated_for(id)
    }

    fn node_is_generated_for_pseudo_element(&self, id: NodeSlotId) -> bool {
        self.as_ref().node_is_generated_for_pseudo_element(id)
    }

    fn node_is_out_of_flow_if_live(&self, id: NodeSlotId) -> bool {
        self.as_ref().node_is_out_of_flow_if_live(id)
    }

    fn node_is_atomic_inline(&self, id: NodeSlotId) -> bool {
        self.as_ref().node_is_atomic_inline(id)
    }

    fn node_is_positioned(&self, id: NodeSlotId) -> bool {
        self.as_ref().node_is_positioned(id)
    }

    fn node_is_floating(&self, id: NodeSlotId) -> bool {
        self.as_ref().node_is_floating(id)
    }

    fn node_has_compositor_animation_frame(&self, id: NodeSlotId, kind: CompositorAnimationFrameKind) -> bool {
        self.as_ref().node_has_compositor_animation_frame(id, kind)
    }

    fn node_style_if_live(&self, id: NodeSlotId) -> Option<ComputedValuesView<'_>> {
        self.as_ref().node_style_if_live(id)
    }

    fn text_content(&self, id: NodeSlotId) -> Option<&TextContent> {
        self.as_ref().text_content(id)
    }

    fn text_fragments(&self, primary: NodeSlotId) -> TextFragments {
        self.as_ref().text_fragments(primary)
    }

    fn node_has_dom_paint_fact(&self, id: NodeSlotId, fact: DomPaintFact) -> bool {
        self.as_ref().node_has_dom_paint_fact(id, fact)
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

    fn paint_damage_of_row(&self, row: NodeSlotId) -> PaintDamage {
        self.as_ref().paint_damage_of_row(row)
    }

    fn damaged_paint_rows(&self) -> Vec<NodeSlotId> {
        self.as_ref().damaged_paint_rows()
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
/// has no `Deref` to the arena, so a read the trait does not name does not compile. It reads the
/// rows, their fragment links and their side data from what the document published when the
/// recording started. The absolute rects it computes go to the recorder's own memo, stamped with
/// the geometry epoch the recording started at.
#[derive(Clone, Copy)]
pub(crate) struct PaintSource<'a> {
    arena: &'a LayoutNodeArena,
    rows: &'a PublishedRows,
    absolute_rects: &'a RefCell<AbsoluteRectMemo>,
    geometry_epoch: u64,
}

impl<'a> PaintSource<'a> {
    pub(crate) fn new(
        arena: &'a LayoutNodeArena,
        rows: &'a PublishedRows,
        absolute_rects: &'a RefCell<AbsoluteRectMemo>,
    ) -> Self {
        Self {
            arena,
            rows,
            absolute_rects,
            geometry_epoch: arena.absolute_rect_memo_epoch(),
        }
    }
}

impl GeometryRead for PaintSource<'_> {
    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        self.rows.paintable_data(id)
    }

    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        self.rows.paintable_row_is_populated(id)
    }

    fn with_committed_fragment_link<R>(&self, id: NodeSlotId, read: impl FnOnce(Option<&FragmentLink>) -> R) -> R {
        self.rows.with_committed_fragment_link(id, read)
    }

    fn committed_side_data(&self, id: NodeSlotId) -> impl Deref<Target = CommittedSideData> + '_ {
        self.rows.committed_side_data(id)
    }

    fn node_kind_if_live(&self, id: NodeSlotId) -> Option<NodeKind> {
        self.arena.node_kind_if_live(id)
    }

    fn node_flags_if_live(&self, id: NodeSlotId) -> u32 {
        self.arena.node_flags_if_live(id)
    }

    fn node_parent_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.arena.node_parent_if_live(id)
    }

    fn node_is_fragmented_inline(&self, id: NodeSlotId) -> bool {
        self.arena.node_is_fragmented_inline(id)
    }

    fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<CssPixelRect> {
        self.absolute_rects.borrow().get(id, self.geometry_epoch)
    }

    fn memoize_absolute_rect(&self, id: NodeSlotId, rect: CssPixelRect) {
        self.absolute_rects.borrow_mut().set(id, self.geometry_epoch, rect);
    }
}

impl PaintRead for PaintSource<'_> {
    fn slot_is_live(&self, id: NodeSlotId) -> bool {
        self.arena.slot_is_live(id)
    }

    fn node_first_child_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.arena.node_first_child_if_live(id)
    }

    fn node_next_sibling_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.arena.node_next_sibling_if_live(id)
    }

    fn node_containing_block_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        self.arena.node_containing_block_if_live(id)
    }

    fn node_generated_for(&self, id: NodeSlotId) -> u8 {
        self.arena.node_generated_for(id)
    }

    fn node_is_generated_for_pseudo_element(&self, id: NodeSlotId) -> bool {
        self.arena.node_is_generated_for_pseudo_element(id)
    }

    fn node_is_out_of_flow_if_live(&self, id: NodeSlotId) -> bool {
        self.arena.node_is_out_of_flow_if_live(id)
    }

    fn node_is_atomic_inline(&self, id: NodeSlotId) -> bool {
        self.arena.node_is_atomic_inline(id)
    }

    fn node_is_positioned(&self, id: NodeSlotId) -> bool {
        self.arena.node_is_positioned(id)
    }

    fn node_is_floating(&self, id: NodeSlotId) -> bool {
        self.arena.node_is_floating(id)
    }

    fn node_has_compositor_animation_frame(&self, id: NodeSlotId, kind: CompositorAnimationFrameKind) -> bool {
        self.arena.node_has_compositor_animation_frame(id, kind)
    }

    fn node_style_if_live(&self, id: NodeSlotId) -> Option<ComputedValuesView<'_>> {
        self.arena.node_style_if_live(id)
    }

    fn text_content(&self, id: NodeSlotId) -> Option<&TextContent> {
        self.arena.text_content(id)
    }

    fn text_fragments(&self, primary: NodeSlotId) -> TextFragments {
        self.arena.text_fragments(primary)
    }

    fn node_has_dom_paint_fact(&self, id: NodeSlotId, fact: DomPaintFact) -> bool {
        self.arena.node_has_dom_paint_fact(id, fact)
    }

    fn published_svg_filter(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgFilter>> {
        self.arena.svg_paint_resources().published_filter(slot, kind)
    }

    fn published_svg_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgPaintServer>> {
        self.arena.svg_paint_resources().published_paint_server(slot, kind)
    }

    fn replaced_paint_facts(&self, id: NodeSlotId) -> Option<ReplacedPaintFacts> {
        self.arena.replaced_paint_facts(id)
    }

    fn layer_image_paint_facts(
        &self,
        id: NodeSlotId,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> Option<LayerImagePaintFacts> {
        self.arena.layer_image_paint_facts(id, list, computed_index)
    }

    fn paint_damage_of_row(&self, row: NodeSlotId) -> PaintDamage {
        self.arena.paint_damage_of_row(row)
    }

    fn damaged_paint_rows(&self) -> Vec<NodeSlotId> {
        self.arena.damaged_paint_rows()
    }

    fn with_paintable_visual_context_node_handles<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(&BoxVisualContextNodeHandles) -> R,
    ) -> R {
        self.arena.with_paintable_visual_context_node_handles(id, read)
    }

    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_> {
        self.arena.stacking_context_entries(root)
    }
}
