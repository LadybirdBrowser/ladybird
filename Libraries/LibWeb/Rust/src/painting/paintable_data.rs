/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::layout::{FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize, FfiDrawGlyph};
use std::cell::Cell;

pub const NO_STACKING_CONTEXT: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(u8)]
pub enum PaintableKind {
    #[default]
    None = 0,
    Paintable,
    PaintableWithLines,
    InlinePaintable,
    ViewportPaintable,
    ImagePaintable,
    CanvasPaintable,
    VideoPaintable,
    CheckBoxPaintable,
    RadioButtonPaintable,
    FieldSetPaintable,
    NavigableContainerViewportPaintable,
    SVGSVGPaintable,
    SVGPathPaintable,
    SVGGraphicsPaintable,
    SVGImagePaintable,
    SVGMaskPaintable,
    SVGClipPaintable,
    SVGPatternPaintable,
    SVGForeignObjectPaintable,
}

impl PaintableKind {
    pub const fn class_name(self) -> &'static str {
        match self {
            Self::None => "(none)",
            Self::Paintable => "Paintable",
            Self::PaintableWithLines => "PaintableWithLines",
            Self::InlinePaintable => "InlinePaintable",
            Self::ViewportPaintable => "ViewportPaintable",
            Self::ImagePaintable => "ImagePaintable",
            Self::CanvasPaintable => "CanvasPaintable",
            Self::VideoPaintable => "VideoPaintable",
            Self::CheckBoxPaintable => "CheckBoxPaintable",
            Self::RadioButtonPaintable => "RadioButtonPaintable",
            Self::FieldSetPaintable => "FieldSetPaintable",
            Self::NavigableContainerViewportPaintable => "NavigableContainerViewportPaintable",
            Self::SVGSVGPaintable => "SVGSVGPaintable",
            Self::SVGPathPaintable => "SVGPathPaintable",
            Self::SVGGraphicsPaintable => "SVGGraphicsPaintable",
            Self::SVGImagePaintable => "SVGImagePaintable",
            Self::SVGMaskPaintable => "SVGMaskPaintable",
            Self::SVGClipPaintable => "SVGClipPaintable",
            Self::SVGPatternPaintable => "SVGPatternPaintable",
            Self::SVGForeignObjectPaintable => "SVGForeignObjectPaintable",
        }
    }

    pub const fn has_lines(self) -> bool {
        matches!(
            self,
            Self::PaintableWithLines | Self::ViewportPaintable | Self::SVGForeignObjectPaintable
        )
    }

    pub const fn foreground_is_never_cached(self) -> bool {
        matches!(self, Self::NavigableContainerViewportPaintable)
    }

    pub const fn forms_unconnected_subtree(self) -> bool {
        matches!(
            self,
            Self::SVGMaskPaintable | Self::SVGClipPaintable | Self::SVGPatternPaintable
        )
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum PaintableFlag {
    HasNonInvertibleCssTransform = 1 << 0,
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
pub struct FfiStickyInsets {
    pub top: CssPixels,
    pub right: CssPixels,
    pub bottom: CssPixels,
    pub left: CssPixels,
    pub has_top: bool,
    pub has_right: bool,
    pub has_bottom: bool,
    pub has_left: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiOverflowData {
    pub rect: FfiCssPixelRect,
    pub has_scrollable_overflow: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PaintableData {
    pub containing_block: NodeSlotId,
    pub kind: PaintableKind,
    pub selection_state: u8,
    pub slot_generation: u8,
    pub flags: u32,

    pub offset: FfiCssPixelPoint,
    pub content_size: FfiCssPixelSize,
    pub local_padding_box_union: FfiCssPixelRect,
    pub local_border_box_union: FfiCssPixelRect,

    pub overflow: FfiOverflowData,
    pub has_overflow: bool,
    pub cached_overflow: FfiOverflowData,
    pub has_cached_overflow: bool,
    pub sticky_insets: FfiStickyInsets,
    pub has_sticky_insets: bool,

    pub stacking_context: u32,

    pub enclosing_scroll_node_index: usize,
    pub own_scroll_node_index: usize,
    pub has_accumulated_visual_context: bool,
    pub accumulated_visual_context_index: usize,
    pub accumulated_visual_context_for_descendants_index: usize,
    pub fixed_background_visual_context: usize,
    pub has_fixed_background_visual_context: bool,
    /// Range of visual context nodes this box appended during the last tree build.
    pub visual_context_nodes_begin: usize,
    pub visual_context_nodes_end: usize,
}

impl Default for PaintableData {
    fn default() -> Self {
        Self {
            containing_block: NodeSlotId::INVALID,
            kind: PaintableKind::None,
            selection_state: 0,
            slot_generation: 0,
            flags: 0,
            offset: FfiCssPixelPoint::default(),
            content_size: FfiCssPixelSize::default(),
            local_padding_box_union: FfiCssPixelRect::default(),
            local_border_box_union: FfiCssPixelRect::default(),
            overflow: FfiOverflowData::default(),
            has_overflow: false,
            cached_overflow: FfiOverflowData::default(),
            has_cached_overflow: false,
            sticky_insets: FfiStickyInsets::default(),
            has_sticky_insets: false,
            stacking_context: u32::MAX,
            enclosing_scroll_node_index: 0,
            own_scroll_node_index: 0,
            has_accumulated_visual_context: false,
            accumulated_visual_context_index: 0,
            accumulated_visual_context_for_descendants_index: 0,
            fixed_background_visual_context: 0,
            has_fixed_background_visual_context: false,
            visual_context_nodes_begin: 0,
            visual_context_nodes_end: 0,
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
    pub rect: FfiCssPixelRect,
    pub baseline: CssPixels,
    pub fragment_count: u32,
}

pub struct GlyphRunRecord {
    pub glyphs: Vec<FfiDrawGlyph>,
    pub font: libgfx_rust::font::RetainedFont,
    pub retained: libgfx_rust::text_layout::RetainedGlyphRun,
}

pub struct FragmentRecord {
    pub layout_node: NodeSlotId,
    pub offset: FfiCssPixelPoint,
    pub size: FfiCssPixelSize,
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
    pub selection_state: u8,
    pub glyph_run: Option<GlyphRunRecord>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InlineBoxPieceRecord {
    pub node: NodeSlotId,
    pub first_fragment_index: u32,
    pub fragment_count: u32,
    pub line_index: u32,
    pub border_box_rect: FfiCssPixelRect,
    pub baseline: CssPixels,
    pub accumulated_vertical_shift: CssPixels,
    pub present_edges: u8,
    pub is_geometry_only_placeholder: bool,
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
    pub(crate) svg_filter_bounds: Cell<Option<FfiCssPixelRect>>,
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
