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
use crate::layout::node_data::{CompositorAnimationFrameKind, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::layout::{RenderedTextBoundary, TextContent};
use crate::painting::paint_order_plan::PaintOrderInputs;
use crate::painting::paintable_data::{PaintableData, PaintableSideData};
use crate::painting::stacking_context::entries::StackingContextEntries;
use crate::painting::svg_paint_resources::{PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind};
use std::cell::Ref;
use std::ops::Deref;
use std::sync::Arc;

pub(crate) trait GeometryRead: Sized {
    fn paintable_data(&self, id: NodeSlotId) -> &PaintableData;
    fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool;
    /// Reads the fragment link a populated row committed.
    fn with_committed_fragment_link<R>(&self, id: NodeSlotId, read: impl FnOnce(Option<&FragmentLink>) -> R) -> R;
    /// The side data of a populated row.
    fn paintable_side_data(&self, id: NodeSlotId) -> Ref<'_, PaintableSideData>;

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

    /// The paint-order inputs paint preparation gathered for the row, if it gathered them.
    fn prepared_paint_order_inputs(&self, row: NodeSlotId) -> Option<PaintOrderInputs>;
    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_>;

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

    fn paintable_side_data(&self, id: NodeSlotId) -> Ref<'_, PaintableSideData> {
        self.as_ref().paintable_side_data(id)
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

    fn prepared_paint_order_inputs(&self, row: NodeSlotId) -> Option<PaintOrderInputs> {
        self.as_ref().row_paint_state(row).order_inputs()
    }

    fn stacking_context_entries(&self, root: NodeSlotId) -> Option<impl Deref<Target = StackingContextEntries> + '_> {
        self.as_ref().stacking_context_entries(root)
    }
}
