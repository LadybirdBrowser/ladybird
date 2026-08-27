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
/// computed position-anchor value the anchor lookup beside the anchor insets
/// consults; an empty handle means position-anchor has no name. Each bare
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
    pub position_anchor: ComputedStyleValueHandle,
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
#[derive(Clone, PartialEq)]
pub struct ComputedBorderSide {
    pub color: u32,
    pub line_style: u8,
    pub width: crate::css::css_pixels::CssPixels,
}

/// The layout-facing prefix of the Rust-owned border style group. Its four
/// side facts lead the group in this order, pinned by static asserts beside
/// the C++ compatibility view.
#[repr(C)]
pub struct BorderLayoutFacts {
    pub border_left: ComputedBorderSide,
    pub border_top: ComputedBorderSide,
    pub border_right: ComputedBorderSide,
    pub border_bottom: ComputedBorderSide,
}

/// The border computed values. C++ materializes radii and border-image views
/// from the canonical handles only when consumers request them.
#[repr(C)]
pub struct BorderValues {
    pub border_left: ComputedBorderSide,
    pub border_top: ComputedBorderSide,
    pub border_right: ComputedBorderSide,
    pub border_bottom: ComputedBorderSide,
    pub border_left_color_style_value: ComputedStyleValueHandle,
    pub border_top_color_style_value: ComputedStyleValueHandle,
    pub border_right_color_style_value: ComputedStyleValueHandle,
    pub border_bottom_color_style_value: ComputedStyleValueHandle,
    pub border_left_computed_width: crate::css::css_pixels::CssPixels,
    pub border_top_computed_width: crate::css::css_pixels::CssPixels,
    pub border_right_computed_width: crate::css::css_pixels::CssPixels,
    pub border_bottom_computed_width: crate::css::css_pixels::CssPixels,
    pub border_bottom_left_radius: ComputedStyleValueHandle,
    pub border_bottom_right_radius: ComputedStyleValueHandle,
    pub border_top_left_radius: ComputedStyleValueHandle,
    pub border_top_right_radius: ComputedStyleValueHandle,
    pub has_noninitial_border_radii: bool,
    pub corner_bottom_left_shape: f64,
    pub corner_bottom_right_shape: f64,
    pub corner_top_left_shape: f64,
    pub corner_top_right_shape: f64,
    pub border_image_source: ComputedStyleValueHandle,
    pub border_image_slice: ComputedStyleValueHandle,
    pub border_image_width: ComputedStyleValueHandle,
    pub border_image_outset: ComputedStyleValueHandle,
    pub border_image_repeat: ComputedStyleValueHandle,
}

/// Canonical generated-content and counter values. C++ materializes its
/// presentation structures only when a consumer requests them.
#[repr(C)]
pub struct ContentValues {
    pub content: ComputedStyleValueHandle,
    pub counter_increment: ComputedStyleValueHandle,
    pub counter_reset: ComputedStyleValueHandle,
    pub counter_set: ComputedStyleValueHandle,
}

/// Canonical inherited list values. Counter-style and image presentation
/// objects are resolved lazily by consumers with their current style scope.
#[repr(C)]
pub struct InheritedListValues {
    pub list_style_type: ComputedStyleValueHandle,
    pub list_style_position: u8,
    pub list_style_image: ComputedStyleValueHandle,
    pub quotes: ComputedStyleValueHandle,
}

/// One physical overflow-clip-margin side.
#[repr(C)]
pub struct ComputedOverflowClipMarginSide {
    pub has_visual_box: bool,
    pub visual_box: u8,
    pub offset: crate::css::css_pixels::CssPixels,
}

/// Four physical overflow-clip-margin sides.
#[repr(C)]
pub struct ComputedOverflowClipMargin {
    pub left: ComputedOverflowClipMarginSide,
    pub top: ComputedOverflowClipMarginSide,
    pub right: ComputedOverflowClipMarginSide,
    pub bottom: ComputedOverflowClipMarginSide,
}

/// Canonical non-inherited values which do not form a more specific group.
#[repr(C)]
pub struct MiscResetValues {
    pub outline_offset_style_value: ComputedStyleValueHandle,
    pub scroll_margin: ComputedLengthBox,
    pub scroll_padding: ComputedLengthBox,
    pub overflow_clip_margin: ComputedOverflowClipMargin,
    pub column_span: u8,
    pub appearance: u8,
    pub computed_appearance: u8,
    pub outline_style: u8,
    pub object_fit: u8,
    pub column_height: ComputedSize,
    pub outline_color: u32,
    pub outline_width: crate::css::css_pixels::CssPixels,
    pub outline_offset: crate::css::css_pixels::CssPixels,
    pub user_select: u8,
    pub object_position_x: ComputedStyleValueHandle,
    pub object_position_y: ComputedStyleValueHandle,
    pub view_transition_name: ComputedStyleValueHandle,
    pub touch_action_allow_left: bool,
    pub touch_action_allow_right: bool,
    pub touch_action_allow_up: bool,
    pub touch_action_allow_down: bool,
    pub touch_action_allow_pinch_zoom: bool,
    pub touch_action_allow_other: bool,
    pub scroll_behavior: u8,
    pub scroll_snap_align_block: u8,
    pub scroll_snap_align_inline: u8,
    pub scroll_snap_stop: u8,
    pub scroll_snap_axis: u8,
    pub scroll_snap_strictness: u8,
    pub scrollbar_gutter: u8,
    pub scrollbar_width: u8,
    pub shape_image_threshold: f64,
    pub shape_margin: ComputedStyleValueHandle,
    pub shape_outside: ComputedStyleValueHandle,
    pub will_change: ComputedStyleValueHandle,
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

/// A computed text-underline-position pair.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct ComputedTextUnderlinePosition {
    pub horizontal: u8,
    pub vertical: u8,
}

/// A computed text-underline-offset and the pixel value used by layout.
#[repr(C)]
pub struct ComputedTextUnderlineOffset {
    pub used_value: crate::css::css_pixels::CssPixels,
    pub is_auto: bool,
    pub value: ComputedStyleValueHandle,
}

/// Layout of the inherited text computed values.
#[repr(C)]
pub struct InheritedTextValues {
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
    pub color: u32,
    pub color_style_value: ComputedStyleValueHandle,
    pub webkit_text_fill_color: u32,
    pub webkit_text_fill_color_is_current_color: bool,
    pub text_shadow: RetainedComputedShadowList,
    pub text_transform: u8,
    pub text_wrap_style: u8,
    pub text_decoration_skip_ink: u8,
    pub text_underline_position: ComputedTextUnderlinePosition,
    pub text_underline_offset: ComputedTextUnderlineOffset,
    pub overflow_wrap: u8,
    pub word_spacing_style_value: ComputedStyleValueHandle,
    pub letter_spacing_style_value: ComputedStyleValueHandle,
    pub orphans: u64,
    pub widows: u64,
}

/// The animation and transition computed values. Each handle retains the
/// canonical Rust longhand value; typed C++ views decode coordinated list
/// items only when a consumer asks for them.
#[repr(C)]
pub struct AnimationValues {
    pub animation_name: ComputedStyleValueHandle,
    pub animation_composition: ComputedStyleValueHandle,
    pub animation_delay: ComputedStyleValueHandle,
    pub animation_direction: ComputedStyleValueHandle,
    pub animation_duration: ComputedStyleValueHandle,
    pub animation_fill_mode: ComputedStyleValueHandle,
    pub animation_iteration_count: ComputedStyleValueHandle,
    pub animation_play_state: ComputedStyleValueHandle,
    pub animation_timeline: ComputedStyleValueHandle,
    pub animation_timing_function: ComputedStyleValueHandle,
    pub scroll_timeline_name: ComputedStyleValueHandle,
    pub scroll_timeline_axis: ComputedStyleValueHandle,
    pub timeline_scope: ComputedStyleValueHandle,
    pub view_timeline_name: ComputedStyleValueHandle,
    pub view_timeline_axis: ComputedStyleValueHandle,
    pub view_timeline_inset: ComputedStyleValueHandle,
    pub transition_property: ComputedStyleValueHandle,
    pub transition_duration: ComputedStyleValueHandle,
    pub transition_timing_function: ComputedStyleValueHandle,
    pub transition_delay: ComputedStyleValueHandle,
    pub transition_behavior: ComputedStyleValueHandle,
    pub transition_delay_and_duration_are_single_zero: bool,
}

/// The mask computed values. Each handle retains a canonical Rust longhand;
/// typed C++ views coordinate repeatable lists only when consumed.
#[repr(C)]
pub struct MaskValues {
    pub mask_image: ComputedStyleValueHandle,
    pub mask_type: ComputedStyleValueHandle,
    pub clip_path: ComputedStyleValueHandle,
    pub mask_mode: ComputedStyleValueHandle,
    pub mask_repeat: ComputedStyleValueHandle,
    pub mask_position: ComputedStyleValueHandle,
    pub mask_clip: ComputedStyleValueHandle,
    pub mask_origin: ComputedStyleValueHandle,
    pub mask_size: ComputedStyleValueHandle,
    pub mask_composite: ComputedStyleValueHandle,
}

/// The background computed values. Rust keeps every canonical longhand while
/// C++ coordinates the repeatable lists only when painting consumes them.
#[repr(C)]
pub struct BackgroundValues {
    pub background_color: u32,
    pub background_color_style_value: ComputedStyleValueHandle,
    pub background_color_clip: u8,
    pub background_image: ComputedStyleValueHandle,
    pub background_attachment: ComputedStyleValueHandle,
    pub background_blend_mode: ComputedStyleValueHandle,
    pub background_clip: ComputedStyleValueHandle,
    pub background_origin: ComputedStyleValueHandle,
    pub background_position_x: ComputedStyleValueHandle,
    pub background_position_y: ComputedStyleValueHandle,
    pub background_repeat: ComputedStyleValueHandle,
    pub background_size: ComputedStyleValueHandle,
}

/// The layout-facing prefix of the Rust-owned font style group. The metric
/// fields and pointers borrow a font cascade pinned by the document's font
/// computer for the lifetime of its style records.
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

/// Canonical inherited font values and their derived platform font facts.
#[repr(C)]
pub struct FontValues {
    pub font_size: crate::css::css_pixels::CssPixels,
    pub line_height_used: crate::css::css_pixels::CssPixels,
    pub font_variant_emoji: u8,
    pub font_ascent: f32,
    pub font_descent: f32,
    pub font_x_height: f32,
    pub first_available_font: *const std::ffi::c_void,
    pub font_cascade_list: *const std::ffi::c_void,
    pub font_weight: f64,
    pub font_width: f64,
    pub math_shift: u8,
    pub math_style: u8,
    pub math_depth: i32,
    pub font_family: ComputedStyleValueHandle,
    pub font_style: ComputedStyleValueHandle,
    pub font_optical_sizing: ComputedStyleValueHandle,
    pub font_feature_settings: ComputedStyleValueHandle,
    pub font_kerning: ComputedStyleValueHandle,
    pub font_language_override: ComputedStyleValueHandle,
    pub font_variant_alternates: ComputedStyleValueHandle,
    pub font_variant_caps: ComputedStyleValueHandle,
    pub font_variant_east_asian: ComputedStyleValueHandle,
    pub font_variant_ligatures: ComputedStyleValueHandle,
    pub font_variant_numeric: ComputedStyleValueHandle,
    pub font_variant_position: ComputedStyleValueHandle,
    pub font_variation_settings: ComputedStyleValueHandle,
    pub text_rendering: ComputedStyleValueHandle,
    pub line_height: ComputedStyleValueHandle,
    pub math_shift_value: ComputedStyleValueHandle,
    pub math_style_value: ComputedStyleValueHandle,
    pub math_depth_value: ComputedStyleValueHandle,
    pub font_size_value: ComputedStyleValueHandle,
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
}

/// A Rust-owned list of computed TextDecorationLine enum codes.
#[repr(C)]
pub struct RetainedTextDecorationLineList {
    pub pointer: *mut u8,
    pub length: usize,
}

/// Layout of the computed text-decoration and white-space-trim values.
#[repr(C)]
pub struct TextResetValues {
    pub text_decoration_lines: RetainedTextDecorationLineList,
    /// 0 = auto, 1 = from-font, 2 = length-percentage.
    pub text_decoration_thickness_kind: u8,
    pub text_decoration_thickness: ComputedStyleValueHandle,
    pub text_decoration_style: u8,
    pub text_decoration_color: u32,
    pub white_space_trim_discard_before: bool,
    pub white_space_trim_discard_after: bool,
    pub white_space_trim_discard_inner: bool,
}

/// One transform function lowered for paint. Percentage-bearing translate
/// axes retain their computed values; every other transform stores a matrix.
#[repr(C)]
pub struct ComputedResolvedTransform {
    pub is_translate: bool,
    pub matrix: [f32; 16],
    pub x_px: f32,
    pub y_px: f32,
    pub z_px: f32,
    pub x_percentage: ComputedStyleValueHandle,
    pub y_percentage: ComputedStyleValueHandle,
}

/// A Rust-owned array of paint-ready transforms.
#[repr(C)]
pub struct RetainedComputedResolvedTransformList {
    pub pointer: *mut ComputedResolvedTransform,
    pub length: usize,
}

/// Layout of the computed transform properties.
#[repr(C)]
pub struct TransformValues {
    pub transformations: ComputedStyleValueHandle,
    pub resolved_transforms: RetainedComputedResolvedTransformList,
    pub transform_box: u8,
    pub transform_origin_x: ComputedStyleValueHandle,
    pub transform_origin_y: ComputedStyleValueHandle,
    pub transform_origin_z: ComputedStyleValueHandle,
    pub transform_style: u8,
    pub backface_visibility: u8,
    pub rotate: ComputedStyleValueHandle,
    pub translate: ComputedStyleValueHandle,
    pub scale: ComputedStyleValueHandle,
    pub has_perspective: bool,
    pub perspective_px: i32,
    pub perspective_origin_x: ComputedStyleValueHandle,
    pub perspective_origin_y: ComputedStyleValueHandle,
}

/// One paint-ready CSS filter operation.
#[repr(C)]
pub struct ComputedFilterOperation {
    pub kind: u8,
    pub color_operation: u8,
    pub amount: f32,
    pub shadow_offset_x: i32,
    pub shadow_offset_y: i32,
    pub shadow_radius: i32,
    pub shadow_color: u32,
    pub url_value: ComputedStyleValueHandle,
}

#[repr(C)]
pub struct RetainedComputedFilterOperationList {
    pub pointer: *mut ComputedFilterOperation,
    pub length: usize,
}

#[repr(C)]
pub struct ComputedFilter {
    pub filter_list: ComputedStyleValueHandle,
    pub operations: RetainedComputedFilterOperationList,
}

/// Layout-compatible with CSS::ShadowData.
#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct ComputedShadow {
    pub offset_x: i32,
    pub offset_y: i32,
    pub blur_radius: i32,
    pub spread_distance: i32,
    pub color: u32,
    pub color_syntax: u8,
    pub placement: u32,
}

impl ComputedShadow {
    pub fn is_inner(&self) -> bool {
        self.placement == 1
    }
}

#[repr(C)]
pub struct RetainedComputedShadowList {
    pub pointer: *mut ComputedShadow,
    pub length: usize,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq)]
pub struct ComputedClipEdge {
    pub is_auto: bool,
    pub value: f64,
    pub unit: u8,
}

/// Layout of the computed effects properties.
#[repr(C)]
pub struct EffectsValues {
    pub opacity: f32,
    pub filter: ComputedFilter,
    pub backdrop_filter: ComputedFilter,
    pub mix_blend_mode: u8,
    pub isolation: u8,
    pub box_shadows: RetainedComputedShadowList,
    pub clip_is_rect: bool,
    pub clip_edges: [ComputedClipEdge; 4],
    /// Canonical longhands retained so payload equality implies computed-value
    /// equality even when distinct syntax lowers to the same used facts.
    pub opacity_style_value: ComputedStyleValueHandle,
    pub filter_style_value: ComputedStyleValueHandle,
    pub backdrop_filter_style_value: ComputedStyleValueHandle,
    pub mix_blend_mode_style_value: ComputedStyleValueHandle,
    pub isolation_style_value: ComputedStyleValueHandle,
    pub box_shadow_style_value: ComputedStyleValueHandle,
    pub clip_style_value: ComputedStyleValueHandle,
}

#[repr(C)]
pub struct RetainedPositionAreaList {
    pub pointer: *mut u8,
    pub length: usize,
}

#[repr(C)]
pub struct ComputedPositionTryFallback {
    pub name: crate::css::retained_fly_string::RetainedUtf16FlyString,
    pub tactics: [u8; 3],
    pub tactic_count: usize,
    pub has_position_area: bool,
    pub position_area: RetainedPositionAreaList,
}

#[repr(C)]
pub struct RetainedPositionTryFallbackList {
    pub pointer: *mut ComputedPositionTryFallback,
    pub length: usize,
}

/// Layout of the computed anchor-positioning properties.
#[repr(C)]
pub struct AnchorValues {
    pub anchor_names: crate::css::retained_fly_string::RetainedUtf16FlyStringList,
    pub anchor_scope_all: bool,
    pub anchor_scope_names: crate::css::retained_fly_string::RetainedUtf16FlyStringList,
    pub position_anchor_type: u8,
    pub position_anchor_name: crate::css::retained_fly_string::RetainedUtf16FlyString,
    pub position_area: RetainedPositionAreaList,
    pub position_try_fallbacks: RetainedPositionTryFallbackList,
    pub has_position_try_order: bool,
    pub position_try_order: u8,
    pub position_visibility_always: bool,
    pub position_visibility_anchors_valid: bool,
    pub position_visibility_anchors_visible: bool,
    pub position_visibility_no_overflow: bool,
}

/// A computed caret-color or accent-color, keeping both the computed auto
/// distinction and the resolved used color.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct ComputedColorOrAuto {
    pub is_auto: bool,
    pub computed_color: u32,
    pub used_color: u32,
}

/// One cursor list item: either a retained cursor() value or a predefined
/// cursor enum code.
#[repr(C)]
pub struct ComputedCursor {
    pub is_cursor_value: bool,
    pub cursor: ComputedStyleValueHandle,
    pub predefined: u8,
}

#[repr(C)]
pub struct RetainedComputedCursorList {
    pub pointer: *mut ComputedCursor,
    pub length: usize,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct ComputedScrollbarColor {
    pub thumb_color: u32,
    pub track_color: u32,
    pub is_auto: bool,
}

/// Layout of the inherited UI computed values.
#[repr(C)]
pub struct InheritedUIValues {
    pub caret_color: ComputedColorOrAuto,
    pub accent_color: ComputedColorOrAuto,
    pub cursor: RetainedComputedCursorList,
    pub pointer_events: u8,
    pub scrollbar_color: ComputedScrollbarColor,
    pub color_scheme: u8,
    pub color_schemes: crate::css::retained_fly_string::RetainedUtf16FlyStringList,
    pub color_scheme_only: bool,
}

/// One computed SVG paint. A color paint uses `color`; a URL paint retains
/// the URL style value and may also carry a fallback color.
#[repr(C)]
pub struct ComputedSvgPaint {
    /// 0 none, 1 color, 2 URL.
    pub kind: u8,
    pub url: ComputedStyleValueHandle,
    pub has_color: bool,
    pub color: u32,
    pub color_is_currentcolor: bool,
}

/// One computed stroke-dasharray item: either a plain SVG number or a
/// retained length-percentage value.
#[repr(C)]
pub struct ComputedSvgDash {
    pub is_number: bool,
    pub number: f64,
    pub value: ComputedStyleValueHandle,
}

#[repr(C)]
pub struct RetainedComputedSvgDashList {
    pub pointer: *mut ComputedSvgDash,
    pub length: usize,
}

/// Layout of the inherited SVG computed values.
#[repr(C)]
pub struct InheritedSVGValues {
    pub fill: ComputedSvgPaint,
    pub stroke: ComputedSvgPaint,
    pub fill_rule: u8,
    pub clip_rule: u8,
    pub fill_opacity: f32,
    pub stroke_opacity: f32,
    pub stroke_linecap: u8,
    pub stroke_linejoin: u8,
    pub stroke_dasharray: RetainedComputedSvgDashList,
    pub stroke_dashoffset: ComputedStyleValueHandle,
    pub stroke_miterlimit: f64,
    pub stroke_width: ComputedStyleValueHandle,
    pub color_interpolation: u8,
    pub color_interpolation_filters: u8,
    pub paint_order: [u8; 3],
    pub paint_order_serialization_length: u8,
    pub paint_order_is_normal: bool,
    pub text_anchor: u8,
    pub has_dominant_baseline: bool,
    pub dominant_baseline: u8,
    pub shape_rendering: u8,
}

// Registered indices of the style groups the computed-values view reads,
// pinned to the C++ StyleGroupIndex enum by static_asserts in
// LayoutRustBridge.cpp.
pub const STYLE_GROUP_INDEX_INHERITED_TABLE: usize = 0;
pub const STYLE_GROUP_INDEX_INHERITED_LIST: usize = 1;
pub const STYLE_GROUP_INDEX_INHERITED_UI: usize = 2;
pub const STYLE_GROUP_INDEX_INHERITED_SVG: usize = 3;
pub const STYLE_GROUP_INDEX_INHERITED_TEXT: usize = 4;
pub const STYLE_GROUP_INDEX_INHERITED_BOX: usize = 5;
pub const STYLE_GROUP_INDEX_FONT: usize = 6;
pub const STYLE_GROUP_INDEX_SVG_RESET: usize = 8;
pub const STYLE_GROUP_INDEX_GRID: usize = 9;
pub const STYLE_GROUP_INDEX_ANCHOR: usize = 10;
pub const STYLE_GROUP_INDEX_EFFECTS: usize = 11;
pub const STYLE_GROUP_INDEX_MASK: usize = 12;
pub const STYLE_GROUP_INDEX_TEXT_RESET: usize = 13;
pub const STYLE_GROUP_INDEX_TRANSFORM: usize = 15;
pub const STYLE_GROUP_INDEX_BACKGROUND: usize = 16;
pub const STYLE_GROUP_INDEX_BORDER: usize = 17;
pub const STYLE_GROUP_INDEX_ALIGNMENT: usize = 18;
pub const STYLE_GROUP_INDEX_MISC_RESET: usize = 19;
pub const STYLE_GROUP_INDEX_SIZING: usize = 20;
pub const STYLE_GROUP_INDEX_SURROUND: usize = 21;
pub const STYLE_GROUP_INDEX_BOX: usize = 22;
