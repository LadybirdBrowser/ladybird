/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values;
use crate::painting::display_list::commands::{ContextRef, SpatialNodeIndex};
use std::cell::Cell;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum PaintableFlag {
    HasNonInvertibleCssTransform = 1 << 0,
    HorizontalScrollbarEnlarged = 1 << 1,
    VerticalScrollbarEnlarged = 1 << 2,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiPixelBox {
    pub top: CssPixels,
    pub right: CssPixels,
    pub bottom: CssPixels,
    pub left: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiOverflowData {
    pub rect: used_values::FfiCssPixelRect,
    pub has_scrollable_overflow: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintableData {
    pub containing_block: NodeSlotId,
    pub selection_state: u8,
    pub slot_generation: u8,
    pub flags: u32,

    pub offset: used_values::FfiCssPixelPoint,
    pub content_size: used_values::FfiCssPixelSize,
    pub local_padding_box_union: used_values::FfiCssPixelRect,
    pub local_border_box_union: used_values::FfiCssPixelRect,

    pub overflow_relative_to_padding_box: FfiOverflowData,
    pub overflow_measured_this_commit: bool,
    pub overflow_valid_across_recommits: bool,

    pub establishes_stacking_context: bool,

    pub enclosing_scroll_node_index: SpatialNodeIndex,
    pub own_scroll_node_index: SpatialNodeIndex,
    pub has_accumulated_visual_context: bool,
    pub accumulated_visual_context: ContextRef,
    pub accumulated_visual_context_for_descendants: ContextRef,
    pub fixed_background_visual_context: ContextRef,
    pub has_fixed_background_visual_context: bool,
    pub has_scroll_offset_dependent_background: bool,
}

impl Default for PaintableData {
    fn default() -> Self {
        Self {
            containing_block: NodeSlotId::INVALID,
            selection_state: 0,
            slot_generation: 0,
            flags: 0,
            offset: used_values::FfiCssPixelPoint::default(),
            content_size: used_values::FfiCssPixelSize::default(),
            local_padding_box_union: used_values::FfiCssPixelRect::default(),
            local_border_box_union: used_values::FfiCssPixelRect::default(),
            overflow_relative_to_padding_box: FfiOverflowData::default(),
            overflow_measured_this_commit: false,
            overflow_valid_across_recommits: false,
            establishes_stacking_context: false,
            enclosing_scroll_node_index: SpatialNodeIndex::default(),
            own_scroll_node_index: SpatialNodeIndex::default(),
            has_accumulated_visual_context: false,
            accumulated_visual_context: ContextRef::default(),
            accumulated_visual_context_for_descendants: ContextRef::default(),
            fixed_background_visual_context: ContextRef::default(),
            has_fixed_background_visual_context: false,
            has_scroll_offset_dependent_background: false,
        }
    }
}

impl PaintableData {
    pub fn has_flag(&self, flag: PaintableFlag) -> bool {
        self.flags & flag as u32 != 0
    }

    pub fn set_flag(&mut self, flag: PaintableFlag, value: bool) {
        if value {
            self.flags |= flag as u32;
        } else {
            self.flags &= !(flag as u32);
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSelectionEntry {
    pub is_text_node_entry: bool,
    pub layout_node: NodeSlotId,
    pub state: u8,
}

pub const SELECTION_STATE_NONE: u8 = 0;
pub const SELECTION_STATE_START: u8 = 1;
pub const SELECTION_STATE_END: u8 = 2;
pub const SELECTION_STATE_START_AND_END: u8 = 3;
pub const SELECTION_STATE_FULL: u8 = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PaintableRowResetKind {
    Recommitted = 0,
    Cleared = 1,
    Freed = 2,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LineRecord {
    pub rect: used_values::FfiCssPixelRect,
    pub baseline: CssPixels,
    pub fragment_count: u32,
}

pub struct GlyphRunRecord {
    pub glyphs: Vec<libgfx_rust::text_layout::DrawGlyph>,
    pub font: libgfx_rust::font::RetainedFont,
}

impl GlyphRunRecord {
    fn font_ref(&self) -> libgfx_rust::font::FontRef<'_> {
        // SAFETY: The record's RetainedFont keeps the font live for the
        // lifetime of the returned borrow.
        unsafe { libgfx_rust::font::FontRef::from_raw(self.font.as_raw()) }
    }

    pub fn bounding_box(&self, scale: f32) -> [f32; 4] {
        libgfx_rust::text_layout::glyph_run_bounding_box(self.font_ref(), &self.glyphs, scale)
    }

    pub fn glyph_intercepts(&self, scale: f32, y_top: f32, y_bottom: f32) -> Vec<f32> {
        libgfx_rust::text_layout::glyph_run_glyph_intercepts(self.font_ref(), &self.glyphs, scale, y_top, y_bottom)
    }
}

pub struct FragmentRecord {
    pub layout_node: NodeSlotId,
    pub style_source: NodeSlotId,
    pub offset: used_values::FfiCssPixelPoint,
    pub size: used_values::FfiCssPixelSize,
    pub line_index: u32,
    pub start_offset: usize,
    pub length_in_code_units: usize,
    pub dom_start_offset_in_node: usize,
    pub dom_end_offset_in_node: usize,
    pub dom_end_offset_with_trailing_whitespace: usize,
    pub trailing_whitespace_length_in_code_units: usize,
    pub baseline: CssPixels,
    pub accumulated_vertical_shift: CssPixels,
    pub writing_mode: u8,
    pub is_block_ellipsis: bool,
    pub selection_state: u8,
    pub glyph_run: Option<GlyphRunRecord>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InlineBoxPieceRecord {
    pub node: NodeSlotId,
    pub first_fragment_index: u32,
    pub fragment_count: u32,
    pub line_index: u32,
    pub border_box_rect: used_values::FfiCssPixelRect,
    pub baseline: CssPixels,
    pub accumulated_vertical_shift: CssPixels,
    pub present_edges: u8,
    pub is_geometry_only_placeholder: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum BorderEdge {
    Top,
    Right,
    Bottom,
    Left,
}

impl BorderEdge {
    pub const ALL: [BorderEdge; 4] = [BorderEdge::Top, BorderEdge::Right, BorderEdge::Bottom, BorderEdge::Left];

    pub const fn index(self) -> usize {
        self as usize
    }
}

pub const PIECE_EDGE_TOP: u8 = 1 << 0;
pub const PIECE_EDGE_RIGHT: u8 = 1 << 1;
pub const PIECE_EDGE_BOTTOM: u8 = 1 << 2;
pub const PIECE_EDGE_LEFT: u8 = 1 << 3;

impl InlineBoxPieceRecord {
    pub fn shrunken_by_present_edges(
        &self,
        rect: crate::css::css_pixels::CssPixelRect,
        widths: FfiPixelBox,
    ) -> crate::css::css_pixels::CssPixelRect {
        let zero = CssPixels::from_raw(0);
        let has = |edge: u8| self.present_edges & edge != 0;
        rect.shrunken(
            if has(PIECE_EDGE_TOP) { widths.top } else { zero },
            if has(PIECE_EDGE_RIGHT) { widths.right } else { zero },
            if has(PIECE_EDGE_BOTTOM) { widths.bottom } else { zero },
            if has(PIECE_EDGE_LEFT) { widths.left } else { zero },
        )
    }
}

#[derive(Default)]
pub struct PaintableSideData {
    pub(crate) lines: Vec<LineRecord>,
    pub(crate) fragments: Vec<FragmentRecord>,
    pub(crate) inline_box_pieces: Vec<InlineBoxPieceRecord>,
    pub(crate) piece_indices: Vec<u32>,
    pub(crate) svg_filter_bounds: Cell<Option<used_values::FfiCssPixelRect>>,
    // Only meaningful while is_self_painting(); assigned by the containing block's
    // assign_fragment_ownership().
    pub(crate) fragment_ownership: Option<crate::painting::fragment_ownership::FragmentOwnershipFilter>,
}

impl PaintableSideData {
    pub(crate) fn clear_committed_records(&mut self) {
        self.lines.clear();
        self.fragments.clear();
        self.inline_box_pieces.clear();
        self.piece_indices.clear();
        self.fragment_ownership = None;
    }
}
