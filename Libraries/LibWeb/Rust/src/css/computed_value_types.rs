/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// The computed forms accepted by width and height sizing properties.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ComputedSizeKind {
    Auto,
    Calculated,
    Length,
    Percentage,
    MinContent,
    MaxContent,
    FitContent,
    None,
}

/// A computed sizing value. Scalar and calculated values retain their
/// immutable Rust style-value identity. Fit-content retains only its
/// argument, and keyword-only forms leave the handle empty.
#[repr(C)]
#[derive(Debug)]
pub struct ComputedStyleValueHandle {
    pub pointer: *const std::ffi::c_void,
}

#[repr(C)]
#[derive(Debug)]
pub struct ComputedSize {
    pub kind: ComputedSizeKind,
    pub value: ComputedStyleValueHandle,
}

/// Layout of the six computed sizing properties.
#[repr(C)]
pub struct SizingValues {
    pub width: ComputedSize,
    pub min_width: ComputedSize,
    pub max_width: ComputedSize,
    pub height: ComputedSize,
    pub min_height: ComputedSize,
    pub max_height: ComputedSize,
}

/// A computed flex-basis value. Content uses the flag; every other form
/// uses the same computed-size representation as width and height.
#[repr(C)]
pub struct ComputedFlexBasis {
    pub is_content: bool,
    pub size: ComputedSize,
}

/// A computed row-gap or column-gap value. Normal uses the flag; every
/// other form retains its immutable length-percentage identity.
#[repr(C)]
pub struct ComputedGap {
    pub is_normal: bool,
    pub value: ComputedStyleValueHandle,
}

/// Layout of the computed flexbox and box-alignment properties.
#[repr(C)]
pub struct AlignmentValues {
    pub flex_direction: u8,
    pub flex_wrap: u8,
    pub flex_basis: ComputedFlexBasis,
    pub flex_grow: f64,
    pub flex_shrink: f64,
    pub order: i32,
    pub align_content: u8,
    pub align_items: u8,
    pub align_self: u8,
    pub justify_content: u8,
    pub justify_items: u8,
    pub justify_self: u8,
    pub column_gap: ComputedGap,
    pub row_gap: ComputedGap,
}

/// A computed length-percentage-or-auto value.
#[repr(C)]
pub struct ComputedLengthPercentageOrAuto {
    pub is_auto: bool,
    pub value: ComputedStyleValueHandle,
}

/// Four physical computed length-percentage-or-auto sides.
#[repr(C)]
pub struct ComputedLengthBox {
    pub top: ComputedLengthPercentageOrAuto,
    pub right: ComputedLengthPercentageOrAuto,
    pub bottom: ComputedLengthPercentageOrAuto,
    pub left: ComputedLengthPercentageOrAuto,
}

/// Layout of the computed inset, margin, and padding properties, plus the
/// computed position-anchor name the anchor lookup beside the anchor insets
/// consults; the zero raw means position-anchor has no name. Each bare
/// anchor() inset also carries the calculated wrapper the layout pass
/// resolves, built with the payload so inset reads borrow it directly.
#[repr(C)]
pub struct SurroundValues {
    pub inset: ComputedLengthBox,
    pub top_anchor_inset: ComputedStyleValueHandle,
    pub right_anchor_inset: ComputedStyleValueHandle,
    pub bottom_anchor_inset: ComputedStyleValueHandle,
    pub left_anchor_inset: ComputedStyleValueHandle,
    pub top_anchor_inset_wrapper: ComputedStyleValueHandle,
    pub right_anchor_inset_wrapper: ComputedStyleValueHandle,
    pub bottom_anchor_inset_wrapper: ComputedStyleValueHandle,
    pub left_anchor_inset_wrapper: ComputedStyleValueHandle,
    pub position_anchor_name: crate::css::retained_fly_string::RetainedUtf16FlyString,
    pub margin: ComputedLengthBox,
    pub padding: ComputedLengthBox,
}

/// A computed vertical-align value: an alignment keyword or a retained
/// length-percentage offset.
#[repr(C)]
pub struct ComputedVerticalAlign {
    pub is_keyword: bool,
    pub keyword: u8,
    pub value: ComputedStyleValueHandle,
}

/// The computed aspect-ratio value, carrying both the used form and the
/// as-computed form the serialization path reports. Absent ratios keep both
/// components at zero so payload equality stays field-wise.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct ComputedAspectRatio {
    pub use_natural_aspect_ratio_if_available: bool,
    pub has_preferred_ratio: bool,
    pub preferred_ratio_numerator: f64,
    pub preferred_ratio_denominator: f64,
    pub computed_use_natural_aspect_ratio_if_available: bool,
    pub has_computed_ratio: bool,
    pub computed_ratio_numerator: f64,
    pub computed_ratio_denominator: f64,
}

/// Layout of the computed box-level properties.
#[repr(C)]
pub struct BoxValues {
    pub display: crate::css::display::FfiDisplay,
    pub display_before_box_type_transformation: crate::css::display::FfiDisplay,
    pub float_: u8,
    pub clear: u8,
    pub position: u8,
    pub overflow_x: u8,
    pub overflow_y: u8,
    pub box_sizing: u8,
    pub resize: u8,
    pub text_overflow: u8,
    pub unicode_bidi: u8,
    pub table_layout: u8,
    pub grid_auto_flow_row: bool,
    pub grid_auto_flow_dense: bool,
    pub column_width: ComputedSize,
    pub column_count_has_value: bool,
    pub column_count: i32,
    pub has_z_index: bool,
    pub z_index: i32,
    pub vertical_align: ComputedVerticalAlign,
    pub aspect_ratio: ComputedAspectRatio,
    pub size_containment: bool,
    pub inline_size_containment: bool,
    pub layout_containment: bool,
    pub style_containment: bool,
    pub paint_containment: bool,
    pub is_size_container: bool,
    pub is_inline_size_container: bool,
    pub is_scroll_state_container: bool,
    pub container_name: crate::css::retained_fly_string::RetainedUtf16FlyStringList,
}

/// One computed border side, mirroring the C++ BorderData member layout:
/// color, line style, used width. Layout reads the line style and width
/// everywhere, and table border conflict resolution also reads the color.
#[repr(C)]
pub struct ComputedBorderSide {
    pub color: u32,
    pub line_style: u8,
    pub width: crate::css::css_pixels::CssPixels,
}

/// The layout-facing prefix of the C++-owned border style group: its four
/// BorderData members lead the group in this order, pinned by static
/// asserts beside the C++ group definition.
#[repr(C)]
pub struct BorderLayoutFacts {
    pub border_left: ComputedBorderSide,
    pub border_top: ComputedBorderSide,
    pub border_right: ComputedBorderSide,
    pub border_bottom: ComputedBorderSide,
}

/// A computed text-indent value, mirroring the C++ TextIndentData layout:
/// a retained length-percentage plus the each-line and hanging flags.
#[repr(C)]
pub struct ComputedTextIndent {
    pub length_percentage: ComputedStyleValueHandle,
    pub each_line: bool,
    pub hanging: bool,
}

/// The layout-facing prefix of the C++-owned inherited-text style group,
/// pinned by static asserts beside the C++ group definition. The computed
/// tab-size is stored as an explicit length-or-number triple because the
/// C++ Variant it replaced has no FFI-stable layout.
#[repr(C)]
pub struct InheritedTextLayoutFacts {
    pub text_align: u8,
    pub text_justify: u8,
    pub white_space_collapse: u8,
    pub text_wrap_mode: u8,
    pub word_break: u8,
    pub tab_size_is_number: bool,
    pub letter_spacing: crate::css::css_pixels::CssPixels,
    pub word_spacing: crate::css::css_pixels::CssPixels,
    pub tab_size_length: crate::css::css_pixels::CssPixels,
    pub tab_size_number: f64,
    pub text_indent: ComputedTextIndent,
}

/// The layout-facing prefix of the C++-owned font style group, pinned by
/// static asserts beside the C++ group definition. The metric fields and the
/// first-available-font pointer are derived from the font cascade list when
/// it is installed on the group; the pointers borrow objects the same
/// payload keeps alive.
#[repr(C)]
pub struct FontLayoutFacts {
    pub font_size: crate::css::css_pixels::CssPixels,
    pub line_height_used: crate::css::css_pixels::CssPixels,
    pub font_variant_emoji: u8,
    pub font_ascent: f32,
    pub font_descent: f32,
    pub font_x_height: f32,
    pub first_available_font: *const std::ffi::c_void,
    pub font_cascade_list: *const std::ffi::c_void,
}

pub const GRID_NO_INDEX: u32 = u32::MAX;

#[repr(C)]
pub struct ComputedGridTrackBreadth {
    pub is_flex: bool,
    pub flex_factor: f64,
    pub size: ComputedSize,
}

#[repr(C)]
pub struct ComputedGridTrackEntry {
    pub kind: u8,
    pub next_sibling: u32,
    pub name_index_start: usize,
    pub name_index_count: usize,
    pub size: ComputedGridTrackBreadth,
    pub min_size: ComputedGridTrackBreadth,
    pub max_size: ComputedGridTrackBreadth,
    pub repeat_type: u8,
    pub repeat_count: usize,
    pub repeat_list: ComputedGridTrackList,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ComputedGridTrackEntryKind {
    LineNames,
    TrackSize,
    MinMax,
    Repeat,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ComputedGridTrackList {
    pub is_subgrid: bool,
    pub preserves_line_name_sets: bool,
    pub first_entry: u32,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ComputedGridPlacementKind {
    Auto,
    Line,
    Span,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ComputedGridPlacement {
    pub kind: u8,
    pub has_line_number: bool,
    pub line_number: i32,
    pub has_name: bool,
    pub name_index: u32,
    pub implicit_start_name_index: u32,
    pub implicit_end_name_index: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ComputedGridArea {
    pub name_index: u32,
    pub implicit_start_name_index: u32,
    pub implicit_end_name_index: u32,
    pub row_start: usize,
    pub row_end: usize,
    pub column_start: usize,
    pub column_end: usize,
}

#[repr(C)]
pub struct RetainedGridTrackEntryList {
    pub pointer: *mut ComputedGridTrackEntry,
    pub length: usize,
}

#[repr(C)]
pub struct RetainedGridNameIndexList {
    pub pointer: *mut u32,
    pub length: usize,
}

#[repr(C)]
pub struct RetainedGridAreaList {
    pub pointer: *mut ComputedGridArea,
    pub length: usize,
}

#[repr(C)]
pub struct GridValues {
    pub names: crate::css::retained_fly_string::RetainedUtf16FlyStringList,
    pub name_indices: RetainedGridNameIndexList,
    pub entries: RetainedGridTrackEntryList,
    pub areas: RetainedGridAreaList,
    pub template_columns: ComputedGridTrackList,
    pub template_rows: ComputedGridTrackList,
    pub auto_columns: ComputedGridTrackList,
    pub auto_rows: ComputedGridTrackList,
    pub column_start: ComputedGridPlacement,
    pub column_end: ComputedGridPlacement,
    pub row_start: ComputedGridPlacement,
    pub row_end: ComputedGridPlacement,
    pub grid_template_columns_style_value: ComputedStyleValueHandle,
    pub grid_template_rows_style_value: ComputedStyleValueHandle,
    pub grid_auto_columns_style_value: ComputedStyleValueHandle,
    pub grid_auto_rows_style_value: ComputedStyleValueHandle,
    pub grid_template_areas_style_value: ComputedStyleValueHandle,
    pub grid_column_start_style_value: ComputedStyleValueHandle,
    pub grid_column_end_style_value: ComputedStyleValueHandle,
    pub grid_row_start_style_value: ComputedStyleValueHandle,
    pub grid_row_end_style_value: ComputedStyleValueHandle,
}

/// Layout of the non-inherited SVG geometry and painting properties.
#[repr(C)]
pub struct SVGResetValues {
    pub cx: ComputedStyleValueHandle,
    pub cy: ComputedStyleValueHandle,
    pub d: ComputedStyleValueHandle,
    pub r: ComputedStyleValueHandle,
    pub rx: ComputedLengthPercentageOrAuto,
    pub ry: ComputedLengthPercentageOrAuto,
    pub x: ComputedStyleValueHandle,
    pub y: ComputedStyleValueHandle,
    pub stop_color: u32,
    pub stop_opacity: f32,
    pub flood_color: u32,
    pub flood_opacity: f32,
    pub vector_effect: u8,
    pub shape_rendering: u8,
}

// Registered indices of the style groups the computed-values view reads,
// pinned to the C++ StyleGroupIndex enum by static_asserts in
// LayoutRustBridge.cpp.
pub const STYLE_GROUP_INDEX_INHERITED_TABLE: usize = 0;
pub const STYLE_GROUP_INDEX_GRID: usize = 9;
pub const STYLE_GROUP_INDEX_INHERITED_TEXT: usize = 4;
pub const STYLE_GROUP_INDEX_INHERITED_BOX: usize = 5;
pub const STYLE_GROUP_INDEX_FONT: usize = 6;
pub const STYLE_GROUP_INDEX_SVG_RESET: usize = 8;
pub const STYLE_GROUP_INDEX_BORDER: usize = 17;
pub const STYLE_GROUP_INDEX_ALIGNMENT: usize = 18;
pub const STYLE_GROUP_INDEX_SIZING: usize = 20;
pub const STYLE_GROUP_INDEX_SURROUND: usize = 21;
pub const STYLE_GROUP_INDEX_BOX: usize = 22;
