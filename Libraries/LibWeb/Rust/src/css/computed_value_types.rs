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
pub struct ComputedStyleValueHandle {
    pub pointer: *const std::ffi::c_void,
}

#[repr(C)]
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

/// Layout of the computed inset, margin, and padding properties.
#[repr(C)]
pub struct SurroundValues {
    pub inset: ComputedLengthBox,
    pub top_anchor_inset: ComputedStyleValueHandle,
    pub right_anchor_inset: ComputedStyleValueHandle,
    pub bottom_anchor_inset: ComputedStyleValueHandle,
    pub left_anchor_inset: ComputedStyleValueHandle,
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

/// Layout of the non-inherited SVG geometry and painting properties.
#[repr(C)]
pub struct SVGResetValues {
    pub cx: ComputedStyleValueHandle,
    pub cy: ComputedStyleValueHandle,
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
