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
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paintable_data::{PaintableData, PaintableSideData};
use std::cell::Ref;

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
    fn node_style_if_live(&self, id: NodeSlotId) -> Option<ComputedValuesView<'_>>;
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

    fn node_style_if_live(&self, id: NodeSlotId) -> Option<ComputedValuesView<'_>> {
        self.as_ref().node_style_if_live(id)
    }
}
