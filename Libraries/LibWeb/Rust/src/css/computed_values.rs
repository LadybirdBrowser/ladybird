/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust ownership of the ComputedValues style group payloads.
//!
//! Every style group defines its payload layout and lifecycle here. Rust owns
//! allocation and reference counting; the atomic header is placed immediately
//! before the payload, which the C++ side reads and updates inline so that
//! sharing never crosses the FFI boundary.
//!
//! Layout contract with the C++ side (StyleStructRef):
//!
//!   [ ArcHeader | payload ]
//!               ^-- pointers exchanged over FFI point at the payload
//!
//! The header occupies max(size_of::<usize>(), payload alignment) bytes so the
//! payload stays properly aligned; the reference count lives in the first
//! usize of the header. A reference count of STYLE_GROUP_STATIC_REFCOUNT marks
//! an intentionally leaked payload (the per-group defaults) that must never be
//! reference counted or freed.

use std::alloc::{Layout, alloc, dealloc};
use std::ffi::c_void;
use std::hash::Hasher;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::abort_on_panic;
pub use crate::css::computed_value_types::{
    AlignmentValues, AnchorValues, AnimationValues, BackgroundValues, BorderValues, BoxValues, ComputedAspectRatio,
    ComputedClipEdge, ComputedColorOrAuto, ComputedContainIntrinsicSize, ComputedCursor, ComputedFilter,
    ComputedFilterOperation, ComputedFlexBasis, ComputedGap, ComputedGridArea, ComputedGridPlacement,
    ComputedGridPlacementKind, ComputedGridTrackBreadth, ComputedGridTrackEntry, ComputedGridTrackEntryKind,
    ComputedGridTrackList, ComputedLengthBox, ComputedLengthPercentageOrAuto, ComputedOverflowClipMargin,
    ComputedOverflowClipMarginSide, ComputedPositionTryFallback, ComputedResolvedTransform, ComputedSize,
    ComputedSizeKind, ComputedStyleValueHandle, ComputedSvgDash, ComputedSvgPaint, ComputedTextIndent,
    ComputedTextUnderlineOffset, ComputedTextUnderlinePosition, ComputedVerticalAlign, ContentValues, EffectsValues,
    FontValues, GRID_NO_INDEX, GridValues, InheritedListValues, InheritedSVGValues, InheritedTextValues,
    InheritedUIValues, MaskValues, MiscResetValues, RetainedComputedCursorList, RetainedComputedFilterOperationList,
    RetainedComputedResolvedTransformList, RetainedComputedShadowList, RetainedComputedSvgDashList,
    RetainedGridAreaList, RetainedGridNameIndexList, RetainedGridTrackEntryList, RetainedPositionAreaList,
    RetainedPositionTryFallbackList, RetainedTextDecorationLineList, SVGResetValues, SizingValues, SurroundValues,
    TextResetValues, TransformValues,
};
use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};
use crate::css::style::fast_hash::{FastHasher, fast_hasher};
use crate::css::style_value::StyleValueData;
use crate::css::style_value::{retained_list_drop, retained_list_partial_eq};
use crate::css::table_group_builder::group_index;

/// Reference count value marking an intentionally leaked payload.
pub const STYLE_GROUP_STATIC_REFCOUNT: usize = usize::MAX;

/// Layout of the inherited box style value group.
///
/// This is the source of truth for the group's payload layout: C++ derives its
/// group struct from the cbindgen mirror of this type, adding its C++ identity
/// and typed accessors on top. The fields hold C++ `enum class : u8` values
/// generated from CSS/Enums.json, the same source as the C++ enums.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InheritedBoxValues {
    pub visibility: u8,
    pub direction: u8,
    pub writing_mode: u8,
    pub content_visibility: u8,
    pub image_rendering: u8,
}

impl ComputedStyleValueHandle {
    pub(crate) fn empty() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    pub(crate) fn retained(data: *const crate::css::style_value::StyleValueData) -> Self {
        Self {
            pointer: unsafe { crate::css::style_value::retain_style_value(data) }.cast(),
        }
    }

    pub(crate) fn length(value: f64) -> Self {
        Self {
            pointer: crate::css::style_value::rust_style_value_create_length(
                value,
                crate::css::style_compute::px_length_unit(),
            )
            .cast(),
        }
    }

    pub(crate) fn percentage(value: f64) -> Self {
        Self {
            pointer: crate::css::style_value::rust_style_value_create_percentage(value).cast(),
        }
    }

    fn keyword(value: u16) -> Self {
        Self {
            pointer: crate::css::style_value::rust_style_value_create_keyword(value).cast(),
        }
    }

    pub(crate) fn data(&self) -> Option<&crate::css::style_value::StyleValueData> {
        unsafe { self.pointer.cast::<crate::css::style_value::StyleValueData>().as_ref() }
    }
}

/// Writes one value's content into `hasher` in a way consistent with its `PartialEq`: two values
/// that compare equal always write the same bytes.
///
/// Consistency is what makes a content hash usable as an identity key. It holds by construction
/// as long as every hashed field is hashed by the same notion of equality the comparison uses,
/// and it survives omitting fields (the hash only gets coarser, and equality settles whatever
/// shares a bucket). For the host-interned handles - fly strings, the font cascade list, the
/// first available font - that notion *is* the address, because their `PartialEq` compares
/// addresses. Floats hash by bit pattern with negative zero normalized; NaN never equals
/// anything, so its hash is unconstrained.
pub(crate) trait ContentHash {
    fn write_content_hash(&self, hasher: &mut FastHasher);
}

macro_rules! impl_content_hash_by_field {
    ($type:ty { $($field:ident),+ $(,)? }) => {
        impl ContentHash for $type {
            fn write_content_hash(&self, hasher: &mut FastHasher) {
                $(self.$field.write_content_hash(hasher);)+
            }
        }
    };
}

macro_rules! impl_content_hash_integers {
    ($($type:ty => $write:ident),+ $(,)?) => {
        $(impl ContentHash for $type {
            fn write_content_hash(&self, hasher: &mut FastHasher) {
                hasher.$write(*self);
            }
        })+
    };
}

impl_content_hash_integers!(u8 => write_u8, u32 => write_u32, i32 => write_i32, u64 => write_u64, usize => write_usize);

impl ContentHash for bool {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_u8(u8::from(*self));
    }
}

impl ContentHash for f32 {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_u32((if *self == 0.0 { 0.0_f32 } else { *self }).to_bits());
    }
}

impl ContentHash for f64 {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_u64((if *self == 0.0 { 0.0_f64 } else { *self }).to_bits());
    }
}

impl<T: ContentHash, const N: usize> ContentHash for [T; N] {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        for value in self {
            value.write_content_hash(hasher);
        }
    }
}

impl ContentHash for *const c_void {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_usize(*self as usize);
    }
}

impl ContentHash for crate::css::css_pixels::CssPixels {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_i32(self.raw_value());
    }
}

impl ContentHash for libgfx_rust::font::FontCascadeListHandle {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_usize(self.as_raw() as usize);
    }
}

impl ContentHash for libgfx_rust::font::FrozenFontListRef {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_usize(self.as_raw() as usize);
    }
}

impl ContentHash for RetainedUtf16FlyString {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_usize(self.raw());
    }
}

impl ContentHash for RetainedUtf16FlyStringList {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        let raws = self.raws();
        hasher.write_usize(raws.len());
        for &raw in raws {
            hasher.write_usize(raw);
        }
    }
}

impl ContentHash for ComputedStyleValueHandle {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        match self.data() {
            None => hasher.write_u8(0),
            Some(data) => {
                hasher.write_u8(1);
                hasher.write_u64(data.content_hash());
            }
        }
    }
}

impl ContentHash for ComputedSizeKind {
    fn write_content_hash(&self, hasher: &mut FastHasher) {
        hasher.write_u8(*self as u8);
    }
}

/// Hashes a Rust-owned retained list beside the equality `retained_list_partial_eq!` gives it:
/// the length and then every element.
macro_rules! impl_retained_list_content_hash {
    ($list:ty, $element:ty) => {
        impl ContentHash for $list {
            fn write_content_hash(&self, hasher: &mut FastHasher) {
                let values: &[$element] = self.as_slice();
                hasher.write_usize(values.len());
                for value in values {
                    value.write_content_hash(hasher);
                }
            }
        }
    };
}

impl_content_hash_by_field!(ComputedSize { kind, value });
impl_content_hash_by_field!(crate::css::display::FfiDisplay {
    tag,
    outside,
    inside,
    list_item,
    internal,
    box_value,
});
impl_content_hash_by_field!(ComputedAspectRatio {
    use_natural_aspect_ratio_if_available,
    has_preferred_ratio,
    preferred_ratio_numerator,
    preferred_ratio_denominator,
    computed_use_natural_aspect_ratio_if_available,
    has_computed_ratio,
    computed_ratio_numerator,
    computed_ratio_denominator,
});
impl_content_hash_by_field!(ComputedContainIntrinsicSize {
    has_auto,
    has_length,
    length_px,
});
impl_content_hash_by_field!(crate::css::computed_value_types::ComputedBorderSide {
    color,
    line_style,
    width,
});
impl_content_hash_by_field!(ComputedTextUnderlinePosition { horizontal, vertical });
impl_content_hash_by_field!(ComputedGridTrackList {
    is_subgrid,
    preserves_line_name_sets,
    first_entry,
});
impl_content_hash_by_field!(ComputedGridPlacement {
    kind,
    has_line_number,
    line_number,
    has_name,
    name_index,
    implicit_start_name_index,
    implicit_end_name_index,
});
impl_content_hash_by_field!(ComputedGridArea {
    name_index,
    implicit_start_name_index,
    implicit_end_name_index,
    row_start,
    row_end,
    column_start,
    column_end,
});
impl_content_hash_by_field!(crate::css::computed_value_types::ComputedShadow {
    offset_x,
    offset_y,
    blur_radius,
    spread_distance,
    color,
    color_syntax,
    placement,
});
impl_content_hash_by_field!(ComputedClipEdge { is_auto, value, unit });
impl_content_hash_by_field!(ComputedColorOrAuto {
    is_auto,
    computed_color,
    used_color,
});
impl_content_hash_by_field!(crate::css::computed_value_types::ComputedScrollbarColor {
    thumb_color,
    track_color,
    is_auto,
});
impl_content_hash_by_field!(InheritedTableValues {
    border_collapse,
    caption_side,
    empty_cells,
    border_spacing_horizontal,
    border_spacing_vertical,
});
impl_content_hash_by_field!(InheritedBoxValues {
    visibility,
    direction,
    writing_mode,
    content_visibility,
    image_rendering,
});
// The anchor-inset wrappers are pure derivatives of the anchor-inset fields, so payload equality
// excludes them and the hash follows it field for field.
impl_content_hash_by_field!(SurroundValues {
    inset,
    top_anchor_inset,
    right_anchor_inset,
    bottom_anchor_inset,
    left_anchor_inset,
    position_anchor,
    margin,
    padding,
});

impl Clone for ComputedStyleValueHandle {
    fn clone(&self) -> Self {
        Self {
            pointer: unsafe { crate::css::style_value::retain_style_value(self.pointer.cast()) }.cast(),
        }
    }
}

impl Drop for ComputedStyleValueHandle {
    fn drop(&mut self) {
        unsafe { crate::css::style_value::release_style_value(self.pointer.cast()) };
    }
}

impl PartialEq for ComputedStyleValueHandle {
    fn eq(&self, other: &Self) -> bool {
        match (self.data(), other.data()) {
            (Some(first), Some(second)) => std::ptr::eq(first, second) || first == second,
            (None, None) => true,
            _ => false,
        }
    }
}

impl Clone for ComputedSize {
    fn clone(&self) -> Self {
        Self {
            kind: self.kind,
            value: self.value.clone(),
        }
    }
}

impl PartialEq for ComputedSize {
    fn eq(&self, other: &Self) -> bool {
        self.kind == other.kind && self.value == other.value
    }
}

macro_rules! impl_computed_payload_clone_and_eq {
    ($type:ty { $($field:ident),+ $(,)? }) => {
        impl Clone for $type {
            fn clone(&self) -> Self {
                Self {
                    $($field: self.$field.clone(),)+
                }
            }
        }

        impl PartialEq for $type {
            fn eq(&self, other: &Self) -> bool {
                true $(&& self.$field == other.$field)+
            }
        }

        impl_content_hash_by_field!($type { $($field),+ });
    };
}

impl_computed_payload_clone_and_eq!(SizingValues {
    width,
    min_width,
    max_width,
    height,
    min_height,
    max_height,
});
impl_computed_payload_clone_and_eq!(ComputedFlexBasis { is_content, size });
impl_computed_payload_clone_and_eq!(ComputedGap { is_normal, value });
impl_computed_payload_clone_and_eq!(AlignmentValues {
    webkit_box_orient,
    flex_direction,
    flex_wrap,
    flex_basis,
    flex_grow,
    flex_shrink,
    order,
    align_content,
    align_items,
    align_self,
    justify_content,
    justify_items,
    justify_self,
    column_gap,
    row_gap,
});
impl_computed_payload_clone_and_eq!(ComputedLengthPercentageOrAuto { is_auto, value });
impl_computed_payload_clone_and_eq!(ComputedLengthBox {
    top,
    right,
    bottom,
    left,
});
impl Clone for SurroundValues {
    fn clone(&self) -> Self {
        Self {
            inset: self.inset.clone(),
            top_anchor_inset: self.top_anchor_inset.clone(),
            right_anchor_inset: self.right_anchor_inset.clone(),
            bottom_anchor_inset: self.bottom_anchor_inset.clone(),
            left_anchor_inset: self.left_anchor_inset.clone(),
            top_anchor_inset_wrapper: self.top_anchor_inset_wrapper.clone(),
            right_anchor_inset_wrapper: self.right_anchor_inset_wrapper.clone(),
            bottom_anchor_inset_wrapper: self.bottom_anchor_inset_wrapper.clone(),
            left_anchor_inset_wrapper: self.left_anchor_inset_wrapper.clone(),
            position_anchor: self.position_anchor.clone(),
            margin: self.margin.clone(),
            padding: self.padding.clone(),
        }
    }
}

// The anchor-inset wrappers are pure derivatives of the anchor-inset fields,
// so payload equality excludes them.
impl PartialEq for SurroundValues {
    fn eq(&self, other: &Self) -> bool {
        self.inset == other.inset
            && self.top_anchor_inset == other.top_anchor_inset
            && self.right_anchor_inset == other.right_anchor_inset
            && self.bottom_anchor_inset == other.bottom_anchor_inset
            && self.left_anchor_inset == other.left_anchor_inset
            && self.position_anchor == other.position_anchor
            && self.margin == other.margin
            && self.padding == other.padding
    }
}
impl_computed_payload_clone_and_eq!(SVGResetValues {
    cx,
    cy,
    d,
    r,
    rx,
    ry,
    x,
    y,
    stop_color,
    stop_opacity,
    flood_color,
    flood_opacity,
    vector_effect,
});
impl RetainedTextDecorationLineList {
    fn from_vec(values: Vec<u8>) -> Self {
        let slice = values.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut u8;
        Self { pointer, length }
    }

    pub(crate) fn as_slice(&self) -> &[u8] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

impl Clone for RetainedTextDecorationLineList {
    fn clone(&self) -> Self {
        Self::from_vec(self.as_slice().to_vec())
    }
}

impl Drop for RetainedTextDecorationLineList {
    fn drop(&mut self) {
        if !self.pointer.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.pointer, self.length)) });
        }
    }
}

impl PartialEq for RetainedTextDecorationLineList {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

impl_retained_list_content_hash!(RetainedTextDecorationLineList, u8);

impl_computed_payload_clone_and_eq!(TextResetValues {
    text_decoration_lines,
    text_decoration_thickness_kind,
    text_decoration_thickness,
    text_decoration_style,
    text_decoration_color,
    white_space_trim_discard_before,
    white_space_trim_discard_after,
    white_space_trim_discard_inner,
});
impl_computed_payload_clone_and_eq!(ComputedResolvedTransform {
    is_translate,
    matrix,
    x_px,
    y_px,
    z_px,
    x_percentage,
    y_percentage,
});
impl_computed_payload_clone_and_eq!(ComputedFilterOperation {
    kind,
    color_operation,
    amount,
    shadow_offset_x,
    shadow_offset_y,
    shadow_radius,
    shadow_color,
    url_value,
});

macro_rules! impl_retained_computed_list {
    ($list:ident, $element:ty) => {
        impl $list {
            pub(crate) fn from_vec(values: Vec<$element>) -> Self {
                let slice = values.into_boxed_slice();
                let length = slice.len();
                let pointer = Box::into_raw(slice) as *mut $element;
                Self { pointer, length }
            }

            pub(crate) fn as_slice(&self) -> &[$element] {
                if self.pointer.is_null() {
                    return &[];
                }
                unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
            }
        }

        impl Clone for $list {
            fn clone(&self) -> Self {
                Self::from_vec(self.as_slice().to_vec())
            }
        }

        retained_list_drop!($list);
        retained_list_partial_eq!($list, $element);
        impl_retained_list_content_hash!($list, $element);
    };
}

impl_retained_computed_list!(RetainedComputedFilterOperationList, ComputedFilterOperation);
impl_retained_computed_list!(
    RetainedComputedShadowList,
    crate::css::computed_value_types::ComputedShadow
);
impl_computed_payload_clone_and_eq!(ComputedCursor {
    is_cursor_value,
    cursor,
    predefined,
});
impl_retained_computed_list!(RetainedComputedCursorList, ComputedCursor);
impl_computed_payload_clone_and_eq!(ComputedSvgPaint {
    kind,
    url,
    has_color,
    color,
    color_is_currentcolor,
});
impl_computed_payload_clone_and_eq!(ComputedSvgDash {
    is_number,
    number,
    value,
});
impl_retained_computed_list!(RetainedComputedSvgDashList, ComputedSvgDash);
impl_computed_payload_clone_and_eq!(ComputedTextIndent {
    length_percentage,
    each_line,
    hanging,
});
impl_computed_payload_clone_and_eq!(ComputedTextUnderlineOffset {
    used_value,
    is_auto,
    value,
});
impl_computed_payload_clone_and_eq!(InheritedTextValues {
    text_align,
    text_justify,
    white_space_collapse,
    text_wrap_mode,
    word_break,
    tab_size_is_number,
    letter_spacing,
    word_spacing,
    tab_size_length,
    tab_size_number,
    text_indent,
    color,
    color_style_value,
    font_smoothing,
    webkit_text_fill_color,
    webkit_text_fill_color_is_current_color,
    text_shadow,
    text_transform,
    text_wrap_style,
    text_decoration_skip_ink,
    text_underline_position,
    text_underline_offset,
    overflow_wrap,
    block_ellipsis,
    word_spacing_style_value,
    letter_spacing_style_value,
    orphans,
    widows,
});
impl_computed_payload_clone_and_eq!(AnimationValues {
    animation_name,
    animation_composition,
    animation_delay,
    animation_direction,
    animation_duration,
    animation_fill_mode,
    animation_iteration_count,
    animation_play_state,
    animation_timeline,
    animation_timing_function,
    scroll_timeline_name,
    scroll_timeline_axis,
    timeline_scope,
    view_timeline_name,
    view_timeline_axis,
    view_timeline_inset,
    transition_property,
    transition_duration,
    transition_timing_function,
    transition_delay,
    transition_behavior,
    transition_delay_and_duration_are_single_zero,
});
impl_computed_payload_clone_and_eq!(MaskValues {
    mask_image,
    mask_type,
    clip_path,
    mask_mode,
    mask_repeat,
    mask_position,
    mask_clip,
    mask_origin,
    mask_size,
    mask_composite,
});
impl_computed_payload_clone_and_eq!(BackgroundValues {
    background_color,
    background_color_style_value,
    background_color_clip,
    background_image,
    background_attachment,
    background_blend_mode,
    background_clip,
    background_origin,
    background_position_x,
    background_position_y,
    background_repeat,
    background_size,
});
impl_computed_payload_clone_and_eq!(BorderValues {
    border_left,
    border_top,
    border_right,
    border_bottom,
    border_left_color_style_value,
    border_top_color_style_value,
    border_right_color_style_value,
    border_bottom_color_style_value,
    border_left_computed_width,
    border_top_computed_width,
    border_right_computed_width,
    border_bottom_computed_width,
    border_bottom_left_radius,
    border_bottom_right_radius,
    border_top_left_radius,
    border_top_right_radius,
    has_noninitial_border_radii,
    corner_bottom_left_shape,
    corner_bottom_right_shape,
    corner_top_left_shape,
    corner_top_right_shape,
    border_image_source,
    border_image_slice,
    border_image_width,
    border_image_outset,
    border_image_repeat,
});
impl_computed_payload_clone_and_eq!(ContentValues {
    content,
    counter_increment,
    counter_reset,
    counter_set,
});
impl_computed_payload_clone_and_eq!(InheritedListValues {
    list_style_type,
    list_style_position,
    list_style_image,
    quotes,
});
impl_computed_payload_clone_and_eq!(ComputedOverflowClipMarginSide {
    has_visual_box,
    visual_box,
    offset,
});
impl_computed_payload_clone_and_eq!(ComputedOverflowClipMargin {
    left,
    top,
    right,
    bottom,
});
impl_computed_payload_clone_and_eq!(MiscResetValues {
    outline_offset_style_value,
    scroll_margin,
    scroll_padding,
    overflow_clip_margin,
    column_span,
    break_before,
    break_after,
    break_inside,
    column_rule_style,
    box_decoration_break,
    column_fill,
    appearance,
    computed_appearance,
    outline_style,
    object_fit,
    column_height,
    outline_color,
    outline_width,
    column_rule_color,
    column_rule_width,
    outline_offset,
    user_select,
    object_position_x,
    object_position_y,
    view_transition_name,
    scroll_behavior,
    scroll_snap_align_block,
    scroll_snap_align_inline,
    scroll_snap_stop,
    scroll_snap_axis,
    scroll_snap_strictness,
    scrollbar_gutter,
    scrollbar_width,
    shape_image_threshold,
    shape_margin,
    shape_outside,
    will_change,
});
impl_computed_payload_clone_and_eq!(FontValues {
    font_size,
    line_height_used,
    font_variant_emoji,
    font_ascent,
    font_descent,
    font_x_height,
    font_zero_advance,
    first_available_font,
    font_cascade_list,
    frozen_font_list,
    font_weight,
    font_width,
    math_shift,
    math_style,
    math_depth,
    font_family,
    font_style,
    font_optical_sizing,
    font_feature_settings,
    font_kerning,
    font_language_override,
    font_variant_alternates,
    font_variant_caps,
    font_variant_east_asian,
    font_variant_ligatures,
    font_variant_numeric,
    font_variant_position,
    font_variation_settings,
    text_rendering,
    line_height,
    math_shift_value,
    math_style_value,
    math_depth_value,
    font_size_value,
});
impl_computed_payload_clone_and_eq!(InheritedSVGValues {
    fill,
    stroke,
    fill_rule,
    clip_rule,
    fill_opacity,
    stroke_opacity,
    stroke_linecap,
    stroke_linejoin,
    stroke_dasharray,
    stroke_dashoffset,
    stroke_miterlimit,
    stroke_width,
    color_interpolation,
    color_interpolation_filters,
    paint_order,
    paint_order_serialization_length,
    paint_order_is_normal,
    text_anchor,
    has_dominant_baseline,
    dominant_baseline,
    shape_rendering,
});
impl_retained_computed_list!(RetainedPositionAreaList, u8);
impl_computed_payload_clone_and_eq!(ComputedPositionTryFallback {
    name,
    tactics,
    tactic_count,
    has_position_area,
    position_area,
});
impl_retained_computed_list!(RetainedPositionTryFallbackList, ComputedPositionTryFallback);
impl_computed_payload_clone_and_eq!(ComputedFilter {
    filter_list,
    operations
});
impl_computed_payload_clone_and_eq!(EffectsValues {
    opacity,
    filter,
    backdrop_filter,
    mix_blend_mode,
    isolation,
    box_shadows,
    clip_is_rect,
    clip_edges,
    opacity_style_value,
    filter_style_value,
    backdrop_filter_style_value,
    mix_blend_mode_style_value,
    isolation_style_value,
    box_shadow_style_value,
    clip_style_value,
});
impl_computed_payload_clone_and_eq!(AnchorValues {
    anchor_names,
    anchor_scope_all,
    anchor_scope_names,
    position_anchor_type,
    position_anchor_name,
    position_area,
    position_try_fallbacks,
    has_position_try_order,
    position_try_order,
    position_visibility_always,
    position_visibility_anchors_valid,
    position_visibility_anchors_visible,
    position_visibility_no_overflow,
});
impl_computed_payload_clone_and_eq!(InheritedUIValues {
    caret_color,
    accent_color,
    cursor,
    pointer_events,
    scrollbar_color,
    color_scheme,
    color_schemes,
    color_scheme_only,
});

impl RetainedComputedResolvedTransformList {
    pub(crate) fn from_vec(values: Vec<ComputedResolvedTransform>) -> Self {
        let slice = values.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut ComputedResolvedTransform;
        Self { pointer, length }
    }

    pub(crate) fn as_slice(&self) -> &[ComputedResolvedTransform] {
        if self.pointer.is_null() {
            return &[];
        }
        // SAFETY: A non-null pointer/length pair always comes from
        // from_vec's boxed slice.
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

impl Clone for RetainedComputedResolvedTransformList {
    fn clone(&self) -> Self {
        Self::from_vec(self.as_slice().to_vec())
    }
}

retained_list_drop!(RetainedComputedResolvedTransformList);
retained_list_partial_eq!(RetainedComputedResolvedTransformList, ComputedResolvedTransform);
impl_retained_list_content_hash!(RetainedComputedResolvedTransformList, ComputedResolvedTransform);

impl_computed_payload_clone_and_eq!(TransformValues {
    transformations,
    resolved_transforms,
    transform_box,
    transform_origin_x,
    transform_origin_y,
    transform_origin_z,
    transform_style,
    backface_visibility,
    rotate,
    translate,
    scale,
    has_perspective,
    perspective_px,
    perspective_origin_x,
    perspective_origin_y,
});
impl_computed_payload_clone_and_eq!(ComputedVerticalAlign {
    is_keyword,
    keyword,
    value,
});
impl_computed_payload_clone_and_eq!(BoxValues {
    display,
    display_before_box_type_transformation,
    float_,
    clear,
    position,
    overflow_x,
    overflow_y,
    box_sizing,
    resize,
    text_overflow,
    unicode_bidi,
    table_layout,
    grid_auto_flow_row,
    grid_auto_flow_dense,
    column_width,
    column_count_has_value,
    column_count,
    continue_,
    max_lines,
    has_z_index,
    z_index,
    vertical_align,
    aspect_ratio,
    contain_intrinsic_width,
    contain_intrinsic_height,
    size_containment,
    inline_size_containment,
    layout_containment,
    style_containment,
    paint_containment,
    is_size_container,
    is_inline_size_container,
    is_scroll_state_container,
    container_name,
});

impl_computed_payload_clone_and_eq!(ComputedGridTrackBreadth {
    is_flex,
    flex_factor,
    size,
});
impl_computed_payload_clone_and_eq!(ComputedGridTrackEntry {
    kind,
    next_sibling,
    name_index_start,
    name_index_count,
    size,
    min_size,
    max_size,
    repeat_type,
    repeat_count,
    repeat_list,
});
impl_computed_payload_clone_and_eq!(GridValues {
    names,
    name_indices,
    entries,
    areas,
    template_columns,
    template_rows,
    auto_columns,
    auto_rows,
    column_start,
    column_end,
    row_start,
    row_end,
    grid_template_columns_style_value,
    grid_template_rows_style_value,
    grid_auto_columns_style_value,
    grid_auto_rows_style_value,
    grid_template_areas_style_value,
    grid_column_start_style_value,
    grid_column_end_style_value,
    grid_row_start_style_value,
    grid_row_end_style_value,
});

macro_rules! impl_retained_grid_list {
    ($list:ident, $element:ty) => {
        impl $list {
            pub(crate) fn from_vec(values: Vec<$element>) -> Self {
                let slice = values.into_boxed_slice();
                let length = slice.len();
                let pointer = Box::into_raw(slice) as *mut $element;
                Self { pointer, length }
            }

            pub(crate) fn as_slice(&self) -> &[$element] {
                if self.pointer.is_null() {
                    return &[];
                }
                // SAFETY: A non-null pointer/length pair always comes from
                // from_vec's boxed slice.
                unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
            }
        }

        impl Clone for $list {
            fn clone(&self) -> Self {
                Self::from_vec(self.as_slice().to_vec())
            }
        }

        retained_list_drop!($list);
        retained_list_partial_eq!($list, $element);
        impl_retained_list_content_hash!($list, $element);
    };
}

impl_retained_grid_list!(RetainedGridTrackEntryList, ComputedGridTrackEntry);
impl_retained_grid_list!(RetainedGridNameIndexList, u32);
impl_retained_grid_list!(RetainedGridAreaList, ComputedGridArea);

const EMPTY_GRID_TRACK_LIST: ComputedGridTrackList = ComputedGridTrackList {
    is_subgrid: false,
    preserves_line_name_sets: false,
    first_entry: GRID_NO_INDEX,
};

const AUTO_GRID_PLACEMENT: ComputedGridPlacement = ComputedGridPlacement {
    kind: ComputedGridPlacementKind::Auto as u8,
    has_line_number: false,
    line_number: 0,
    has_name: false,
    name_index: GRID_NO_INDEX,
    implicit_start_name_index: GRID_NO_INDEX,
    implicit_end_name_index: GRID_NO_INDEX,
};

impl GridValues {
    fn auto_track_breadth() -> ComputedGridTrackBreadth {
        ComputedGridTrackBreadth {
            is_flex: false,
            flex_factor: 0.0,
            size: ComputedSize {
                kind: ComputedSizeKind::Auto,
                value: ComputedStyleValueHandle::empty(),
            },
        }
    }

    fn auto_track_entry() -> ComputedGridTrackEntry {
        ComputedGridTrackEntry {
            kind: ComputedGridTrackEntryKind::TrackSize as u8,
            next_sibling: GRID_NO_INDEX,
            name_index_start: 0,
            name_index_count: 0,
            size: Self::auto_track_breadth(),
            min_size: Self::auto_track_breadth(),
            max_size: Self::auto_track_breadth(),
            repeat_type: 0,
            repeat_count: 0,
            repeat_list: EMPTY_GRID_TRACK_LIST,
        }
    }

    fn initial() -> Self {
        Self {
            names: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
            name_indices: RetainedGridNameIndexList::from_vec(Vec::new()),
            entries: RetainedGridTrackEntryList::from_vec(vec![Self::auto_track_entry(), Self::auto_track_entry()]),
            areas: RetainedGridAreaList::from_vec(Vec::new()),
            template_columns: EMPTY_GRID_TRACK_LIST,
            template_rows: EMPTY_GRID_TRACK_LIST,
            auto_columns: ComputedGridTrackList {
                is_subgrid: false,
                preserves_line_name_sets: false,
                first_entry: 0,
            },
            auto_rows: ComputedGridTrackList {
                is_subgrid: false,
                preserves_line_name_sets: false,
                first_entry: 1,
            },
            column_start: AUTO_GRID_PLACEMENT,
            column_end: AUTO_GRID_PLACEMENT,
            row_start: AUTO_GRID_PLACEMENT,
            row_end: AUTO_GRID_PLACEMENT,
            grid_template_columns_style_value: ComputedStyleValueHandle::empty(),
            grid_template_rows_style_value: ComputedStyleValueHandle::empty(),
            grid_auto_columns_style_value: ComputedStyleValueHandle::empty(),
            grid_auto_rows_style_value: ComputedStyleValueHandle::empty(),
            grid_template_areas_style_value: ComputedStyleValueHandle::empty(),
            grid_column_start_style_value: ComputedStyleValueHandle::empty(),
            grid_column_end_style_value: ComputedStyleValueHandle::empty(),
            grid_row_start_style_value: ComputedStyleValueHandle::empty(),
            grid_row_end_style_value: ComputedStyleValueHandle::empty(),
        }
    }

    fn intern_name(&mut self, name: &RetainedUtf16FlyString) -> u32 {
        let raw = name.raw();
        assert_ne!(raw, 0, "cannot intern the no-name sentinel");
        if let Some(index) = self.names.as_slice().iter().position(|name| name.raw() == raw) {
            return index as u32;
        }
        let mut names: Vec<RetainedUtf16FlyString> = self.names.as_slice().to_vec();
        names.push(name.clone());
        let index = (names.len() - 1) as u32;
        self.names = RetainedUtf16FlyStringList::from_retained_strings(names);
        index
    }
}

/// The payload type of one computed-value style group.
#[derive(Clone, Copy)]
pub(crate) enum StyleGroup {
    Font,
    InheritedTable,
    InheritedBox,
    Sizing,
    Alignment,
    SVGReset,
    Surround,
    Box,
    Grid,
    TextReset,
    Transform,
    Effects,
    Anchor,
    InheritedUI,
    InheritedSVG,
    InheritedText,
    Animation,
    Mask,
    Background,
    Border,
    Content,
    InheritedList,
    MiscReset,
}

/// Every style group, in the order of the group indices both sides use (`group_index`, and
/// LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS in C++).
const STYLE_GROUPS: [StyleGroup; group_index::COUNT] = [
    StyleGroup::InheritedTable,
    StyleGroup::InheritedList,
    StyleGroup::InheritedUI,
    StyleGroup::InheritedSVG,
    StyleGroup::InheritedText,
    StyleGroup::InheritedBox,
    StyleGroup::Font,
    StyleGroup::Animation,
    StyleGroup::SVGReset,
    StyleGroup::Grid,
    StyleGroup::Anchor,
    StyleGroup::Effects,
    StyleGroup::Mask,
    StyleGroup::TextReset,
    StyleGroup::Content,
    StyleGroup::Transform,
    StyleGroup::Background,
    StyleGroup::Border,
    StyleGroup::Alignment,
    StyleGroup::MiscReset,
    StyleGroup::Sizing,
    StyleGroup::Surround,
    StyleGroup::Box,
];

fn style_group(group_index: usize) -> StyleGroup {
    STYLE_GROUPS[group_index]
}

fn payload_size(group: StyleGroup) -> usize {
    match group {
        StyleGroup::InheritedTable => size_of::<InheritedTableValues>(),
        StyleGroup::InheritedBox => size_of::<InheritedBoxValues>(),
        StyleGroup::Sizing => size_of::<SizingValues>(),
        StyleGroup::Alignment => size_of::<AlignmentValues>(),
        StyleGroup::SVGReset => size_of::<SVGResetValues>(),
        StyleGroup::Surround => size_of::<SurroundValues>(),
        StyleGroup::Box => size_of::<BoxValues>(),
        StyleGroup::Grid => size_of::<GridValues>(),
        StyleGroup::TextReset => size_of::<TextResetValues>(),
        StyleGroup::Transform => size_of::<TransformValues>(),
        StyleGroup::Effects => size_of::<EffectsValues>(),
        StyleGroup::Anchor => size_of::<AnchorValues>(),
        StyleGroup::InheritedUI => size_of::<InheritedUIValues>(),
        StyleGroup::InheritedSVG => size_of::<InheritedSVGValues>(),
        StyleGroup::InheritedText => size_of::<InheritedTextValues>(),
        StyleGroup::Animation => size_of::<AnimationValues>(),
        StyleGroup::Mask => size_of::<MaskValues>(),
        StyleGroup::Background => size_of::<BackgroundValues>(),
        StyleGroup::Border => size_of::<BorderValues>(),
        StyleGroup::Content => size_of::<ContentValues>(),
        StyleGroup::InheritedList => size_of::<InheritedListValues>(),
        StyleGroup::MiscReset => size_of::<MiscResetValues>(),
        StyleGroup::Font => size_of::<FontValues>(),
    }
}

fn payload_align(group: StyleGroup) -> usize {
    match group {
        StyleGroup::InheritedTable => align_of::<InheritedTableValues>(),
        StyleGroup::InheritedBox => align_of::<InheritedBoxValues>(),
        StyleGroup::Sizing => align_of::<SizingValues>(),
        StyleGroup::Alignment => align_of::<AlignmentValues>(),
        StyleGroup::SVGReset => align_of::<SVGResetValues>(),
        StyleGroup::Surround => align_of::<SurroundValues>(),
        StyleGroup::Box => align_of::<BoxValues>(),
        StyleGroup::Grid => align_of::<GridValues>(),
        StyleGroup::TextReset => align_of::<TextResetValues>(),
        StyleGroup::Transform => align_of::<TransformValues>(),
        StyleGroup::Effects => align_of::<EffectsValues>(),
        StyleGroup::Anchor => align_of::<AnchorValues>(),
        StyleGroup::InheritedUI => align_of::<InheritedUIValues>(),
        StyleGroup::InheritedSVG => align_of::<InheritedSVGValues>(),
        StyleGroup::InheritedText => align_of::<InheritedTextValues>(),
        StyleGroup::Animation => align_of::<AnimationValues>(),
        StyleGroup::Mask => align_of::<MaskValues>(),
        StyleGroup::Background => align_of::<BackgroundValues>(),
        StyleGroup::Border => align_of::<BorderValues>(),
        StyleGroup::Content => align_of::<ContentValues>(),
        StyleGroup::InheritedList => align_of::<InheritedListValues>(),
        StyleGroup::MiscReset => align_of::<MiscResetValues>(),
        StyleGroup::Font => align_of::<FontValues>(),
    }
}

unsafe fn default_construct(group: StyleGroup, payload: *mut c_void) {
    match group {
        StyleGroup::InheritedTable => unsafe {
            (payload as *mut InheritedTableValues).write(InheritedTableValues::initial());
        },
        StyleGroup::InheritedBox => unsafe {
            (payload as *mut InheritedBoxValues).write(InheritedBoxValues::initial());
        },
        StyleGroup::Sizing => unsafe {
            (payload as *mut SizingValues).write(SizingValues::initial());
        },
        StyleGroup::Alignment => unsafe {
            (payload as *mut AlignmentValues).write(AlignmentValues::initial());
        },
        StyleGroup::SVGReset => unsafe {
            (payload as *mut SVGResetValues).write(SVGResetValues::initial());
        },
        StyleGroup::Surround => unsafe {
            (payload as *mut SurroundValues).write(SurroundValues::initial());
        },
        StyleGroup::Box => unsafe {
            (payload as *mut BoxValues).write(BoxValues::initial());
        },
        StyleGroup::Grid => unsafe {
            (payload as *mut GridValues).write(GridValues::initial());
        },
        StyleGroup::TextReset => unsafe {
            (payload as *mut TextResetValues).write(TextResetValues::initial());
        },
        StyleGroup::Transform => unsafe {
            (payload as *mut TransformValues).write(TransformValues::initial());
        },
        StyleGroup::Effects => unsafe {
            (payload as *mut EffectsValues).write(EffectsValues::initial());
        },
        StyleGroup::Anchor => unsafe {
            (payload as *mut AnchorValues).write(AnchorValues::initial());
        },
        StyleGroup::InheritedUI => unsafe {
            (payload as *mut InheritedUIValues).write(InheritedUIValues::initial());
        },
        StyleGroup::InheritedSVG => unsafe {
            (payload as *mut InheritedSVGValues).write(InheritedSVGValues::initial());
        },
        StyleGroup::InheritedText => unsafe {
            (payload as *mut InheritedTextValues).write(InheritedTextValues::initial());
        },
        StyleGroup::Animation => unsafe {
            (payload as *mut AnimationValues).write(AnimationValues::initial());
        },
        StyleGroup::Mask => unsafe {
            (payload as *mut MaskValues).write(MaskValues::initial());
        },
        StyleGroup::Background => unsafe {
            (payload as *mut BackgroundValues).write(BackgroundValues::initial());
        },
        StyleGroup::Border => unsafe {
            (payload as *mut BorderValues).write(BorderValues::initial());
        },
        StyleGroup::Content => unsafe {
            (payload as *mut ContentValues).write(ContentValues::initial());
        },
        StyleGroup::InheritedList => unsafe {
            (payload as *mut InheritedListValues).write(InheritedListValues::initial());
        },
        StyleGroup::MiscReset => unsafe {
            (payload as *mut MiscResetValues).write(MiscResetValues::initial());
        },
        StyleGroup::Font => unsafe {
            (payload as *mut FontValues).write(FontValues::initial());
        },
    }
}

unsafe fn copy_construct(group: StyleGroup, payload: *mut c_void, source: *const c_void) {
    match group {
        StyleGroup::InheritedTable => unsafe {
            (payload as *mut InheritedTableValues).write(*(source as *const InheritedTableValues));
        },
        StyleGroup::InheritedBox => unsafe {
            (payload as *mut InheritedBoxValues).write(*(source as *const InheritedBoxValues));
        },
        StyleGroup::Sizing => unsafe {
            (payload as *mut SizingValues).write((*(source as *const SizingValues)).clone());
        },
        StyleGroup::Alignment => unsafe {
            (payload as *mut AlignmentValues).write((*(source as *const AlignmentValues)).clone());
        },
        StyleGroup::SVGReset => unsafe {
            (payload as *mut SVGResetValues).write((*(source as *const SVGResetValues)).clone());
        },
        StyleGroup::Surround => unsafe {
            (payload as *mut SurroundValues).write((*(source as *const SurroundValues)).clone());
        },
        StyleGroup::Box => unsafe {
            (payload as *mut BoxValues).write((*(source as *const BoxValues)).clone());
        },
        StyleGroup::Grid => unsafe {
            (payload as *mut GridValues).write((*(source as *const GridValues)).clone());
        },
        StyleGroup::TextReset => unsafe {
            (payload as *mut TextResetValues).write((*(source as *const TextResetValues)).clone());
        },
        StyleGroup::Transform => unsafe {
            (payload as *mut TransformValues).write((*(source as *const TransformValues)).clone());
        },
        StyleGroup::Effects => unsafe {
            (payload as *mut EffectsValues).write((*(source as *const EffectsValues)).clone());
        },
        StyleGroup::Anchor => unsafe {
            (payload as *mut AnchorValues).write((*(source as *const AnchorValues)).clone());
        },
        StyleGroup::InheritedUI => unsafe {
            (payload as *mut InheritedUIValues).write((*(source as *const InheritedUIValues)).clone());
        },
        StyleGroup::InheritedSVG => unsafe {
            (payload as *mut InheritedSVGValues).write((*(source as *const InheritedSVGValues)).clone());
        },
        StyleGroup::InheritedText => unsafe {
            (payload as *mut InheritedTextValues).write((*(source as *const InheritedTextValues)).clone());
        },
        StyleGroup::Animation => unsafe {
            (payload as *mut AnimationValues).write((*(source as *const AnimationValues)).clone());
        },
        StyleGroup::Mask => unsafe {
            (payload as *mut MaskValues).write((*(source as *const MaskValues)).clone());
        },
        StyleGroup::Background => unsafe {
            (payload as *mut BackgroundValues).write((*(source as *const BackgroundValues)).clone());
        },
        StyleGroup::Border => unsafe {
            (payload as *mut BorderValues).write((*(source as *const BorderValues)).clone());
        },
        StyleGroup::Content => unsafe {
            (payload as *mut ContentValues).write((*(source as *const ContentValues)).clone());
        },
        StyleGroup::InheritedList => unsafe {
            (payload as *mut InheritedListValues).write((*(source as *const InheritedListValues)).clone());
        },
        StyleGroup::MiscReset => unsafe {
            (payload as *mut MiscResetValues).write((*(source as *const MiscResetValues)).clone());
        },
        StyleGroup::Font => unsafe {
            (payload as *mut FontValues).write((*(source as *const FontValues)).clone());
        },
    }
}

unsafe fn destruct(group: StyleGroup, payload: *mut c_void) {
    match group {
        StyleGroup::InheritedTable => unsafe { std::ptr::drop_in_place(payload as *mut InheritedTableValues) },
        StyleGroup::InheritedBox => unsafe { std::ptr::drop_in_place(payload as *mut InheritedBoxValues) },
        StyleGroup::Sizing => unsafe { std::ptr::drop_in_place(payload as *mut SizingValues) },
        StyleGroup::Alignment => unsafe { std::ptr::drop_in_place(payload as *mut AlignmentValues) },
        StyleGroup::SVGReset => unsafe { std::ptr::drop_in_place(payload as *mut SVGResetValues) },
        StyleGroup::Surround => unsafe { std::ptr::drop_in_place(payload as *mut SurroundValues) },
        StyleGroup::Box => unsafe { std::ptr::drop_in_place(payload as *mut BoxValues) },
        StyleGroup::Grid => unsafe { std::ptr::drop_in_place(payload as *mut GridValues) },
        StyleGroup::TextReset => unsafe { std::ptr::drop_in_place(payload as *mut TextResetValues) },
        StyleGroup::Transform => unsafe { std::ptr::drop_in_place(payload as *mut TransformValues) },
        StyleGroup::Effects => unsafe { std::ptr::drop_in_place(payload as *mut EffectsValues) },
        StyleGroup::Anchor => unsafe { std::ptr::drop_in_place(payload as *mut AnchorValues) },
        StyleGroup::InheritedUI => unsafe { std::ptr::drop_in_place(payload as *mut InheritedUIValues) },
        StyleGroup::InheritedSVG => unsafe { std::ptr::drop_in_place(payload as *mut InheritedSVGValues) },
        StyleGroup::InheritedText => unsafe { std::ptr::drop_in_place(payload as *mut InheritedTextValues) },
        StyleGroup::Animation => unsafe { std::ptr::drop_in_place(payload as *mut AnimationValues) },
        StyleGroup::Mask => unsafe { std::ptr::drop_in_place(payload as *mut MaskValues) },
        StyleGroup::Background => unsafe { std::ptr::drop_in_place(payload as *mut BackgroundValues) },
        StyleGroup::Border => unsafe { std::ptr::drop_in_place(payload as *mut BorderValues) },
        StyleGroup::Content => unsafe { std::ptr::drop_in_place(payload as *mut ContentValues) },
        StyleGroup::InheritedList => unsafe { std::ptr::drop_in_place(payload as *mut InheritedListValues) },
        StyleGroup::MiscReset => unsafe { std::ptr::drop_in_place(payload as *mut MiscResetValues) },
        StyleGroup::Font => unsafe { std::ptr::drop_in_place(payload as *mut FontValues) },
    }
}

unsafe fn payloads_equal(group: StyleGroup, a: *const c_void, b: *const c_void) -> bool {
    match group {
        StyleGroup::InheritedTable => unsafe {
            *(a as *const InheritedTableValues) == *(b as *const InheritedTableValues)
        },
        StyleGroup::InheritedBox => unsafe { *(a as *const InheritedBoxValues) == *(b as *const InheritedBoxValues) },
        StyleGroup::Sizing => unsafe { *(a as *const SizingValues) == *(b as *const SizingValues) },
        StyleGroup::Alignment => unsafe { *(a as *const AlignmentValues) == *(b as *const AlignmentValues) },
        StyleGroup::SVGReset => unsafe { *(a as *const SVGResetValues) == *(b as *const SVGResetValues) },
        StyleGroup::Surround => unsafe { *(a as *const SurroundValues) == *(b as *const SurroundValues) },
        StyleGroup::Box => unsafe { *(a as *const BoxValues) == *(b as *const BoxValues) },
        StyleGroup::Grid => unsafe { *(a as *const GridValues) == *(b as *const GridValues) },
        StyleGroup::TextReset => unsafe { *(a as *const TextResetValues) == *(b as *const TextResetValues) },
        StyleGroup::Transform => unsafe { *(a as *const TransformValues) == *(b as *const TransformValues) },
        StyleGroup::Effects => unsafe { *(a as *const EffectsValues) == *(b as *const EffectsValues) },
        StyleGroup::Anchor => unsafe { *(a as *const AnchorValues) == *(b as *const AnchorValues) },
        StyleGroup::InheritedUI => unsafe { *(a as *const InheritedUIValues) == *(b as *const InheritedUIValues) },
        StyleGroup::InheritedSVG => unsafe { *(a as *const InheritedSVGValues) == *(b as *const InheritedSVGValues) },
        StyleGroup::InheritedText => unsafe {
            *(a as *const InheritedTextValues) == *(b as *const InheritedTextValues)
        },
        StyleGroup::Animation => unsafe { *(a as *const AnimationValues) == *(b as *const AnimationValues) },
        StyleGroup::Mask => unsafe { *(a as *const MaskValues) == *(b as *const MaskValues) },
        StyleGroup::Background => unsafe { *(a as *const BackgroundValues) == *(b as *const BackgroundValues) },
        StyleGroup::Border => unsafe { *(a as *const BorderValues) == *(b as *const BorderValues) },
        StyleGroup::Content => unsafe { *(a as *const ContentValues) == *(b as *const ContentValues) },
        StyleGroup::InheritedList => unsafe {
            *(a as *const InheritedListValues) == *(b as *const InheritedListValues)
        },
        StyleGroup::MiscReset => unsafe { *(a as *const MiscResetValues) == *(b as *const MiscResetValues) },
        StyleGroup::Font => unsafe { *(a as *const FontValues) == *(b as *const FontValues) },
    }
}

unsafe fn payload_content_hash(group: StyleGroup, payload: *const c_void, hasher: &mut FastHasher) {
    match group {
        StyleGroup::InheritedTable => unsafe {
            (*(payload as *const InheritedTableValues)).write_content_hash(hasher);
        },
        StyleGroup::InheritedBox => unsafe {
            (*(payload as *const InheritedBoxValues)).write_content_hash(hasher);
        },
        StyleGroup::Sizing => unsafe {
            (*(payload as *const SizingValues)).write_content_hash(hasher);
        },
        StyleGroup::Alignment => unsafe {
            (*(payload as *const AlignmentValues)).write_content_hash(hasher);
        },
        StyleGroup::SVGReset => unsafe {
            (*(payload as *const SVGResetValues)).write_content_hash(hasher);
        },
        StyleGroup::Surround => unsafe {
            (*(payload as *const SurroundValues)).write_content_hash(hasher);
        },
        StyleGroup::Box => unsafe {
            (*(payload as *const BoxValues)).write_content_hash(hasher);
        },
        StyleGroup::Grid => unsafe {
            (*(payload as *const GridValues)).write_content_hash(hasher);
        },
        StyleGroup::TextReset => unsafe {
            (*(payload as *const TextResetValues)).write_content_hash(hasher);
        },
        StyleGroup::Transform => unsafe {
            (*(payload as *const TransformValues)).write_content_hash(hasher);
        },
        StyleGroup::Effects => unsafe {
            (*(payload as *const EffectsValues)).write_content_hash(hasher);
        },
        StyleGroup::Anchor => unsafe {
            (*(payload as *const AnchorValues)).write_content_hash(hasher);
        },
        StyleGroup::InheritedUI => unsafe {
            (*(payload as *const InheritedUIValues)).write_content_hash(hasher);
        },
        StyleGroup::InheritedSVG => unsafe {
            (*(payload as *const InheritedSVGValues)).write_content_hash(hasher);
        },
        StyleGroup::InheritedText => unsafe {
            (*(payload as *const InheritedTextValues)).write_content_hash(hasher);
        },
        StyleGroup::Animation => unsafe {
            (*(payload as *const AnimationValues)).write_content_hash(hasher);
        },
        StyleGroup::Mask => unsafe {
            (*(payload as *const MaskValues)).write_content_hash(hasher);
        },
        StyleGroup::Background => unsafe {
            (*(payload as *const BackgroundValues)).write_content_hash(hasher);
        },
        StyleGroup::Border => unsafe {
            (*(payload as *const BorderValues)).write_content_hash(hasher);
        },
        StyleGroup::Content => unsafe {
            (*(payload as *const ContentValues)).write_content_hash(hasher);
        },
        StyleGroup::InheritedList => unsafe {
            (*(payload as *const InheritedListValues)).write_content_hash(hasher);
        },
        StyleGroup::MiscReset => unsafe {
            (*(payload as *const MiscResetValues)).write_content_hash(hasher);
        },
        StyleGroup::Font => unsafe {
            (*(payload as *const FontValues)).write_content_hash(hasher);
        },
    }
}

pub(crate) fn style_group_affects_layout(group_index: usize) -> bool {
    !matches!(
        style_group(group_index),
        StyleGroup::Mask | StyleGroup::TextReset | StyleGroup::Background
    )
}

pub(crate) fn style_group_payloads_equal(group_index: usize, a: *const c_void, b: *const c_void) -> bool {
    assert!(!a.is_null());
    assert!(!b.is_null());
    // SAFETY: Published style-group payloads remain live for the call and both use the registered
    // group type at `group_index`.
    unsafe { payloads_equal(style_group(group_index), a, b) }
}

/// A content hash for one style-group payload, consistent with `style_group_payloads_equal`:
/// two payloads that compare equal always hash equal, whoever built them and wherever they live.
/// That is what lets a payload's identity be its content instead of its address. The group
/// index is hashed with the content so payloads of different groups never share a bucket.
pub(crate) fn style_group_payloads_hash(group_index: usize, payload: *const c_void) -> u64 {
    assert!(!payload.is_null());
    let mut hasher = fast_hasher();
    hasher.write_usize(group_index);
    // SAFETY: Published style-group payloads remain live for the call and use the registered
    // group type at `group_index`.
    unsafe { payload_content_hash(style_group(group_index), payload, &mut hasher) };
    hasher.finish()
}

/// Whether a layered image property holds an `<image>` in any layer. The value is one layer or a
/// comma-separated list of them.
fn layer_values_hold_image(handle: &ComputedStyleValueHandle) -> bool {
    // StyleValueList::Separator: Space is 0, Comma is 1.
    const COMMA: u8 = 1;
    match handle.data() {
        Some(StyleValueData::ValueList { values, separator, .. }) if *separator == COMMA => {
            values.as_slice().iter().any(|layer| layer.data().is_image())
        }
        Some(value) => value.is_image(),
        None => false,
    }
}

unsafe fn payload_holds_image_values(group: StyleGroup, payload: *const c_void) -> bool {
    match group {
        StyleGroup::Background => unsafe {
            layer_values_hold_image(&(*(payload as *const BackgroundValues)).background_image)
        },
        StyleGroup::Mask => unsafe { layer_values_hold_image(&(*(payload as *const MaskValues)).mask_image) },
        StyleGroup::Border => unsafe {
            (*(payload as *const BorderValues))
                .border_image_source
                .data()
                .is_some_and(StyleValueData::is_image)
        },
        StyleGroup::InheritedList => unsafe {
            (*(payload as *const InheritedListValues))
                .list_style_image
                .data()
                .is_some_and(StyleValueData::is_image)
        },
        StyleGroup::InheritedUI => unsafe {
            let cursors = &(*(payload as *const InheritedUIValues)).cursor;
            !cursors.pointer.is_null()
                && std::slice::from_raw_parts(cursors.pointer, cursors.length)
                    .iter()
                    .any(|cursor| cursor.is_cursor_value)
        },
        _ => false,
    }
}

/// Whether any of a record's group payloads holds an `<image>` in a property whose images a layout
/// node loads and observes: the background and mask layers, the cursor, the border-image source and
/// the list-style image. Nearly every style holds none, and a record answers that with one flag
/// instead of materializing its layers to find out.
pub(crate) fn style_group_payloads_hold_image_values(payloads: &[*const c_void]) -> bool {
    payloads.iter().enumerate().any(|(group_index, &payload)| {
        // SAFETY: Published style-group payloads remain live for the call and use the registered
        // group type at `group_index`.
        !payload.is_null() && unsafe { payload_holds_image_values(style_group(group_index), payload) }
    })
}

/// Whether the group payloads `old` and `new`, of two records, hold different `<image>` values in the properties whose
/// images a layout node loads and observes, by identity. Where they hold the same ones, a box that moves from one
/// record to the other keeps the images it loads and the facts it paints them with.
pub(crate) fn style_group_payloads_move_image_values(old: &[*const c_void], new: &[*const c_void]) -> bool {
    old.iter()
        .zip(new)
        .enumerate()
        .any(|(group_index, (&old_payload, &new_payload))| {
            if old_payload == new_payload {
                return false;
            }
            let group = style_group(group_index);
            if old_payload.is_null() || new_payload.is_null() {
                // SAFETY: As in `style_group_payloads_hold_image_values`.
                return [old_payload, new_payload]
                    .into_iter()
                    .any(|payload| !payload.is_null() && unsafe { payload_holds_image_values(group, payload) });
            }
            // SAFETY: As in `style_group_payloads_hold_image_values`.
            unsafe { payload_image_values_differ(group, old_payload, new_payload) }
        })
}

/// Whether `old` and `new`, two payloads of `group`, hold different images, by identity.
unsafe fn payload_image_values_differ(group: StyleGroup, old: *const c_void, new: *const c_void) -> bool {
    fn handles_differ(
        old: &ComputedStyleValueHandle,
        new: &ComputedStyleValueHandle,
        holds_image: fn(&ComputedStyleValueHandle) -> bool,
    ) -> bool {
        old.pointer != new.pointer && (holds_image(old) || holds_image(new))
    }
    fn holds_image(handle: &ComputedStyleValueHandle) -> bool {
        handle.data().is_some_and(StyleValueData::is_image)
    }
    match group {
        StyleGroup::Background => unsafe {
            handles_differ(
                &(*(old as *const BackgroundValues)).background_image,
                &(*(new as *const BackgroundValues)).background_image,
                layer_values_hold_image,
            )
        },
        StyleGroup::Mask => unsafe {
            handles_differ(
                &(*(old as *const MaskValues)).mask_image,
                &(*(new as *const MaskValues)).mask_image,
                layer_values_hold_image,
            )
        },
        StyleGroup::Border => unsafe {
            handles_differ(
                &(*(old as *const BorderValues)).border_image_source,
                &(*(new as *const BorderValues)).border_image_source,
                holds_image,
            )
        },
        StyleGroup::InheritedList => unsafe {
            handles_differ(
                &(*(old as *const InheritedListValues)).list_style_image,
                &(*(new as *const InheritedListValues)).list_style_image,
                holds_image,
            )
        },
        StyleGroup::InheritedUI => unsafe {
            let [old_cursors, new_cursors] = [old, new].map(|payload| &(*(payload as *const InheritedUIValues)).cursor);
            (old_cursors.pointer != new_cursors.pointer || old_cursors.length != new_cursors.length)
                && (payload_holds_image_values(group, old) || payload_holds_image_values(group, new))
        },
        _ => false,
    }
}

/// The intentionally leaked per-group default payloads, for building groups without allocating when
/// every field holds its initial value.
struct DefaultPayloads([*const c_void; group_index::COUNT]);

// SAFETY: The defaults are immortal, immutable payloads.
unsafe impl Send for DefaultPayloads {}
unsafe impl Sync for DefaultPayloads {}

pub(crate) fn default_group_payload(group_index: usize) -> *const c_void {
    static DEFAULTS: OnceLock<DefaultPayloads> = OnceLock::new();
    DEFAULTS
        .get_or_init(|| {
            DefaultPayloads(STYLE_GROUPS.map(|group| {
                let payload = allocate_payload(group, STYLE_GROUP_STATIC_REFCOUNT);
                // SAFETY: The payload was just allocated for this group's layout.
                unsafe { default_construct(group, payload) };
                payload.cast_const()
            }))
        })
        .0[group_index]
}

/// The default payload of a style group, which StyleStructRef shares instead of allocating.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_group_default_payload(group_index: usize) -> *const c_void {
    default_group_payload(group_index)
}

/// Retains one reference to a payload, mirroring StyleStructRef::ref():
/// intentionally leaked payloads are never counted.
pub(crate) fn retain_group_payload(group_index: usize, payload: *const c_void) {
    let refcount = refcount_of(payload, payload_align(style_group(group_index)));
    if refcount.load(Ordering::Relaxed) == STYLE_GROUP_STATIC_REFCOUNT {
        return;
    }
    refcount.fetch_add(1, Ordering::Relaxed);
}

pub(crate) fn retained_group_payload_bytes(group_index: usize, payload: *const c_void) -> usize {
    let group = style_group(group_index);
    let refcount = refcount_of(payload, payload_align(group));
    if refcount.load(Ordering::Relaxed) == STYLE_GROUP_STATIC_REFCOUNT {
        return 0;
    }
    allocation_layout(group).size()
}

fn header_size(align: usize) -> usize {
    align.max(size_of::<usize>())
}

fn allocation_layout(group: StyleGroup) -> Layout {
    let payload_align = payload_align(group);
    let align = payload_align.max(align_of::<usize>());
    Layout::from_size_align(header_size(payload_align) + payload_size(group), align)
        .expect("style group layout overflow")
}

fn refcount_of(payload: *const c_void, align: usize) -> &'static AtomicUsize {
    // SAFETY: Every payload pointer handed out by this module is preceded by
    // its header, whose first usize is the reference count.
    unsafe {
        let header = (payload as *const u8).sub(header_size(align)) as *const AtomicUsize;
        &*header
    }
}

/// One payload's live reference count, for tests that check which side of a publication ends up
/// owning the reference it handed over.
#[cfg(test)]
pub(crate) fn group_payload_refcount(group_index: usize, payload: *const c_void) -> usize {
    refcount_of(payload, payload_align(style_group(group_index))).load(Ordering::Relaxed)
}

fn allocate_payload(group: StyleGroup, initial_refcount: usize) -> *mut c_void {
    // SAFETY: The layout is never zero-sized (the header is at least a usize).
    unsafe {
        let allocation = alloc(allocation_layout(group));
        if allocation.is_null() {
            std::process::abort();
        }
        let header = allocation as *mut AtomicUsize;
        (*header).store(initial_refcount, Ordering::Relaxed);
        allocation.add(header_size(payload_align(group))) as *mut c_void
    }
}

/// Allocates a new payload for `group_index` with a reference count of one,
/// copy-constructed from `source`.
///
/// # Safety
/// `source` must be a valid payload of the same group type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_clone(group_index: usize, source: *const c_void) -> *mut c_void {
    unsafe { clone_group_payload(group_index, source) }
}

/// The source must be a live payload of the registered group type.
pub(crate) unsafe fn clone_group_payload(group_index: usize, source: *const c_void) -> *mut c_void {
    let group = style_group(group_index);
    let payload = allocate_payload(group, 1);
    unsafe { copy_construct(group, payload, source) };
    payload
}

/// Destroys and deallocates a payload whose reference count has reached zero.
///
/// # Safety
/// `payload` must be a payload of the given group type with no remaining
/// references, and must not be a static default payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_free(group_index: usize, payload: *mut c_void) {
    unsafe {
        let group = style_group(group_index);
        debug_assert!(refcount_of(payload, payload_align(group)).load(Ordering::Relaxed) == 0);
        destruct(group, payload);
        let allocation = (payload as *mut u8).sub(header_size(payload_align(group)));
        dealloc(allocation, allocation_layout(group));
    };
}

pub(crate) fn release_group_payload(group_index: usize, payload: *const c_void) {
    let group = style_group(group_index);
    let refcount = refcount_of(payload, payload_align(group));
    if refcount.load(Ordering::Relaxed) == STYLE_GROUP_STATIC_REFCOUNT {
        return;
    }
    if refcount.fetch_sub(1, Ordering::AcqRel) == 1 {
        // SAFETY: The count reached zero, so this reference was the last one.
        unsafe {
            destruct(group, payload.cast_mut());
            let allocation = (payload as *mut u8).sub(header_size(payload_align(group)));
            dealloc(allocation, allocation_layout(group));
        }
    }
}

/// Compares two payloads of the same style group for value equality, letting
/// C++ group structs that inherit a Rust-native payload layout reuse the Rust
/// field-wise equality instead of hand-writing a second one.
///
/// # Safety
/// `a` and `b` must be valid payloads of the given group type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_payloads_equal(
    group_index: usize,
    a: *const c_void,
    b: *const c_void,
) -> bool {
    style_group_payloads_equal(group_index, a, b)
}

/// One field of a style group the generic builder can populate or check: a pokeable simple field
/// at `offset` in the group's payload, or a constraint requiring the value to be one that leaves
/// the payload's initial value standing.
#[derive(Clone, Copy)]
pub(crate) struct GroupFieldDescriptor {
    pub(crate) property_id: u16,
    pub(crate) offset: usize,
    pub(crate) kind: GroupFieldKind,
}

#[derive(Clone, Copy)]
pub(crate) enum GroupFieldKind {
    /// An enum stored as u8: the keyword's code through the converter. A keyword it rejects stays
    /// at the payload default for the Rust group builder to complete.
    EnumKeyword(fn(u16) -> Option<u8>),
    /// A number stored as f64.
    F64,
    /// A pixel length stored as raw CSSPixels (i32).
    CssPixels,
    /// A pixel length stored as raw CSSPixels, clamped at zero.
    CssPixelsNonNegative,
    /// An integer stored as u64.
    U64,
    /// A color stored as the C++ Color's raw 32-bit value, resolved by the gather loop, which owns
    /// the color resolution context.
    Color,
    /// A color like `Color`, except that this keyword leaves the initial value standing, for fields
    /// like outline-color whose auto keyword is not a resolvable color.
    ColorOrKeyword(u16),
    /// A number resolved by the gather loop, for values whose normalization has not moved into the
    /// core, like opacity: stored as f32, as f64, or as one byte for derived enum fields like the
    /// used color-scheme.
    ResolvedF32,
    ResolvedF64,
    ResolvedU8,
    /// The value's data stored into a single-pointer handle slot, retaining one reference; the
    /// slot's initial value must be null.
    RetainedData,
    /// A bool stored as one byte: whether the value is this keyword.
    KeywordEqualsBool(u16),
    /// Constraints, which write nothing: the value must be this keyword, a zero pixel length, or
    /// the property's initial value. The last compares data pointers, which holds exactly for
    /// untouched properties since the driver selects the initial table's entries directly.
    RequireKeyword(u16),
    RequireZeroPx,
    RequireInitialValue,
}

/// The appearance keywords without the compatibility ones, which normalize to auto for the
/// appearance field but stay raw for computed_appearance: the misc-reset group builder completes
/// both fields for them.
fn appearance_without_compat(keyword: u16) -> Option<u8> {
    use crate::css::css_enums::appearance::{AUTO, MENULIST_BUTTON, NONE, TEXTFIELD};
    crate::css::css_enums::keyword_to_appearance(keyword)
        .filter(|appearance| matches!(*appearance, AUTO | MENULIST_BUTTON | NONE | TEXTFIELD))
}

/// The fields of each group the generic builder decodes, in the order the gather loop reads their
/// values. Groups that build through bespoke calls have none.
pub(crate) fn group_field_descriptors(group: usize) -> &'static [GroupFieldDescriptor] {
    use crate::css::css_enums as enums;
    use crate::css::css_enums::keyword as k;
    use GroupFieldKind::*;

    // `PROPERTY @ field => kind` describes a field of the group's payload, `PROPERTY => kind` a
    // constraint.
    macro_rules! fields {
        ($group:ty: $($property:ident $(@ $($field:ident).+)? => $kind:expr),* $(,)?) => {
            const {
                &[$(GroupFieldDescriptor {
                    property_id: crate::css::property_metadata::property_id::$property,
                    offset: 0 $(+ std::mem::offset_of!($group, $($field).+))?,
                    kind: $kind,
                }),*]
            }
        };
    }

    match group {
        group_index::TEXT_RESET => fields! { TextResetValues:
            TEXT_DECORATION_LINE => RequireKeyword(k::NONE),
            TEXT_DECORATION_THICKNESS => RequireKeyword(k::AUTO),
            TEXT_DECORATION_STYLE @ text_decoration_style => EnumKeyword(enums::keyword_to_text_decoration_style),
            TEXT_DECORATION_COLOR @ text_decoration_color => Color,
            WHITE_SPACE_TRIM => RequireKeyword(k::NONE),
        },
        group_index::EFFECTS => fields! { EffectsValues:
            OPACITY @ opacity => ResolvedF32,
            FILTER => RequireKeyword(k::NONE),
            BACKDROP_FILTER => RequireKeyword(k::NONE),
            MIX_BLEND_MODE @ mix_blend_mode => EnumKeyword(enums::keyword_to_mix_blend_mode),
            ISOLATION @ isolation => EnumKeyword(enums::keyword_to_isolation),
            BOX_SHADOW => RequireKeyword(k::NONE),
            CLIP => RequireKeyword(k::AUTO),
        },
        group_index::MISC_RESET => fields! { MiscResetValues:
            SCROLL_MARGIN_TOP => RequireZeroPx,
            SCROLL_MARGIN_RIGHT => RequireZeroPx,
            SCROLL_MARGIN_BOTTOM => RequireZeroPx,
            SCROLL_MARGIN_LEFT => RequireZeroPx,
            SCROLL_PADDING_TOP => RequireKeyword(k::AUTO),
            SCROLL_PADDING_RIGHT => RequireKeyword(k::AUTO),
            SCROLL_PADDING_BOTTOM => RequireKeyword(k::AUTO),
            SCROLL_PADDING_LEFT => RequireKeyword(k::AUTO),
            OVERFLOW_CLIP_MARGIN_TOP => RequireInitialValue,
            OVERFLOW_CLIP_MARGIN_RIGHT => RequireInitialValue,
            OVERFLOW_CLIP_MARGIN_BOTTOM => RequireInitialValue,
            OVERFLOW_CLIP_MARGIN_LEFT => RequireInitialValue,
            COLUMN_SPAN @ column_span => EnumKeyword(enums::keyword_to_column_span),
            BREAK_BEFORE @ break_before => EnumKeyword(enums::keyword_to_break_between),
            BREAK_AFTER @ break_after => EnumKeyword(enums::keyword_to_break_between),
            BREAK_INSIDE @ break_inside => EnumKeyword(enums::keyword_to_break_inside),
            COLUMN_RULE_STYLE @ column_rule_style => EnumKeyword(enums::keyword_to_line_style),
            BOX_DECORATION_BREAK @ box_decoration_break => EnumKeyword(enums::keyword_to_box_decoration_break),
            COLUMN_FILL @ column_fill => EnumKeyword(enums::keyword_to_column_fill),
            APPEARANCE @ appearance => EnumKeyword(appearance_without_compat),
            APPEARANCE @ computed_appearance => EnumKeyword(enums::keyword_to_appearance),
            OUTLINE_STYLE @ outline_style => EnumKeyword(enums::keyword_to_outline_style),
            OBJECT_FIT @ object_fit => EnumKeyword(enums::keyword_to_object_fit),
            COLUMN_HEIGHT => RequireKeyword(k::AUTO),
            OUTLINE_COLOR @ outline_color => ColorOrKeyword(k::AUTO),
            OUTLINE_OFFSET => RequireZeroPx,
            OUTLINE_WIDTH @ outline_width => CssPixelsNonNegative,
            COLUMN_RULE_COLOR @ column_rule_color => Color,
            COLUMN_RULE_WIDTH @ column_rule_width => CssPixelsNonNegative,
            USER_SELECT @ user_select => EnumKeyword(enums::keyword_to_user_select),
            OBJECT_POSITION => RequireInitialValue,
            VIEW_TRANSITION_NAME => RequireKeyword(k::NONE),
            TOUCH_ACTION => RequireInitialValue,
            SCROLL_BEHAVIOR @ scroll_behavior => EnumKeyword(enums::keyword_to_scroll_behavior),
            SCROLL_SNAP_ALIGN => RequireInitialValue,
            SCROLL_SNAP_STOP @ scroll_snap_stop => EnumKeyword(enums::keyword_to_scroll_snap_stop),
            SCROLL_SNAP_TYPE => RequireInitialValue,
            SCROLLBAR_GUTTER => RequireInitialValue,
            SCROLLBAR_WIDTH @ scrollbar_width => EnumKeyword(enums::keyword_to_scrollbar_width),
            SHAPE_IMAGE_THRESHOLD @ shape_image_threshold => ResolvedF64,
            SHAPE_MARGIN => RequireZeroPx,
            SHAPE_OUTSIDE => RequireKeyword(k::NONE),
            WILL_CHANGE => RequireKeyword(k::AUTO),
        },
        group_index::INHERITED_TEXT => fields! { InheritedTextValues:
            _WEBKIT_FONT_SMOOTHING @ font_smoothing => EnumKeyword(enums::keyword_to_font_smoothing),
            COLOR @ color => Color,
            COLOR @ color_style_value => RetainedData,
            _WEBKIT_TEXT_FILL_COLOR @ webkit_text_fill_color => Color,
            _WEBKIT_TEXT_FILL_COLOR @ webkit_text_fill_color_is_current_color => KeywordEqualsBool(k::CURRENTCOLOR),
            TEXT_SHADOW => RequireKeyword(k::NONE),
            TEXT_ALIGN @ text_align => EnumKeyword(enums::keyword_to_text_align),
            TEXT_JUSTIFY @ text_justify => EnumKeyword(enums::keyword_to_text_justify),
            TEXT_TRANSFORM @ text_transform => EnumKeyword(enums::keyword_to_text_transform),
            TEXT_WRAP_MODE @ text_wrap_mode => EnumKeyword(enums::keyword_to_text_wrap_mode),
            TEXT_WRAP_STYLE @ text_wrap_style => EnumKeyword(enums::keyword_to_text_wrap_style),
            TEXT_DECORATION_SKIP_INK @ text_decoration_skip_ink => EnumKeyword(enums::keyword_to_text_decoration_skip_ink),
            TEXT_UNDERLINE_POSITION => RequireKeyword(k::AUTO),
            TEXT_UNDERLINE_OFFSET => RequireKeyword(k::AUTO),
            TEXT_INDENT => RequireInitialValue,
            TAB_SIZE => RequireInitialValue,
            WHITE_SPACE_COLLAPSE @ white_space_collapse => EnumKeyword(enums::keyword_to_white_space_collapse),
            WORD_BREAK @ word_break => EnumKeyword(enums::keyword_to_word_break),
            OVERFLOW_WRAP @ overflow_wrap => EnumKeyword(enums::keyword_to_overflow_wrap),
            BLOCK_ELLIPSIS @ block_ellipsis => RetainedData,
            WORD_SPACING @ word_spacing => CssPixels,
            WORD_SPACING @ word_spacing_style_value => RetainedData,
            LETTER_SPACING @ letter_spacing => CssPixels,
            LETTER_SPACING @ letter_spacing_style_value => RetainedData,
            ORPHANS @ orphans => U64,
            WIDOWS @ widows => U64,
        },
        group_index::INHERITED_UI => fields! { InheritedUIValues:
            CARET_COLOR @ caret_color.used_color => Color,
            CARET_COLOR => RequireKeyword(k::AUTO),
            ACCENT_COLOR @ accent_color.used_color => Color,
            ACCENT_COLOR => RequireKeyword(k::AUTO),
            CURSOR => RequireKeyword(k::AUTO),
            POINTER_EVENTS @ pointer_events => EnumKeyword(enums::keyword_to_pointer_events),
            SCROLLBAR_COLOR => RequireKeyword(k::AUTO),
            COLOR_SCHEME @ color_scheme => ResolvedU8,
            COLOR_SCHEME => RequireInitialValue,
        },
        group_index::TRANSFORM => fields! { TransformValues:
            TRANSFORM => RequireKeyword(k::NONE),
            TRANSFORM_BOX @ transform_box => EnumKeyword(enums::keyword_to_transform_box),
            TRANSFORM_ORIGIN => RequireInitialValue,
            TRANSFORM_STYLE @ transform_style => EnumKeyword(enums::keyword_to_transform_style),
            BACKFACE_VISIBILITY @ backface_visibility => EnumKeyword(enums::keyword_to_backface_visibility),
            ROTATE => RequireKeyword(k::NONE),
            TRANSLATE => RequireKeyword(k::NONE),
            SCALE => RequireKeyword(k::NONE),
            PERSPECTIVE => RequireKeyword(k::NONE),
            PERSPECTIVE_ORIGIN => RequireInitialValue,
        },
        group_index::MASK => fields! { MaskValues:
            MASK_IMAGE => RequireKeyword(k::NONE),
            MASK_TYPE => RequireInitialValue,
            CLIP_PATH => RequireKeyword(k::NONE),
            MASK_MODE => RequireInitialValue,
            MASK_REPEAT => RequireInitialValue,
            MASK_POSITION => RequireInitialValue,
            MASK_CLIP => RequireInitialValue,
            MASK_ORIGIN => RequireInitialValue,
            MASK_SIZE => RequireInitialValue,
            MASK_COMPOSITE => RequireInitialValue,
        },
        // Elements without animations, timelines or transitions adopt a shared payload.
        group_index::ANIMATION => fields! { AnimationValues:
            ANIMATION_NAME => RequireInitialValue,
            ANIMATION_COMPOSITION => RequireInitialValue,
            ANIMATION_DELAY => RequireInitialValue,
            ANIMATION_DIRECTION => RequireInitialValue,
            ANIMATION_DURATION => RequireInitialValue,
            ANIMATION_FILL_MODE => RequireInitialValue,
            ANIMATION_ITERATION_COUNT => RequireInitialValue,
            ANIMATION_PLAY_STATE => RequireInitialValue,
            ANIMATION_TIMELINE => RequireInitialValue,
            ANIMATION_TIMING_FUNCTION => RequireInitialValue,
            SCROLL_TIMELINE_NAME => RequireInitialValue,
            SCROLL_TIMELINE_AXIS => RequireInitialValue,
            TIMELINE_SCOPE => RequireInitialValue,
            VIEW_TIMELINE_NAME => RequireInitialValue,
            VIEW_TIMELINE_AXIS => RequireInitialValue,
            VIEW_TIMELINE_INSET => RequireInitialValue,
            TRANSITION_PROPERTY => RequireInitialValue,
            TRANSITION_DURATION => RequireInitialValue,
            TRANSITION_TIMING_FUNCTION => RequireInitialValue,
            TRANSITION_DELAY => RequireInitialValue,
            TRANSITION_BEHAVIOR => RequireInitialValue,
        },
        group_index::INHERITED_SVG => fields! { InheritedSVGValues:
            FILL => RequireInitialValue,
            STROKE => RequireInitialValue,
            FILL_RULE @ fill_rule => EnumKeyword(enums::keyword_to_fill_rule),
            CLIP_RULE @ clip_rule => EnumKeyword(enums::keyword_to_fill_rule),
            FILL_OPACITY @ fill_opacity => ResolvedF32,
            STROKE_OPACITY @ stroke_opacity => ResolvedF32,
            STROKE_LINECAP @ stroke_linecap => EnumKeyword(enums::keyword_to_stroke_linecap),
            STROKE_LINEJOIN @ stroke_linejoin => EnumKeyword(enums::keyword_to_stroke_linejoin),
            STROKE_DASHARRAY => RequireKeyword(k::NONE),
            STROKE_DASHOFFSET => RequireInitialValue,
            STROKE_MITERLIMIT @ stroke_miterlimit => F64,
            STROKE_WIDTH => RequireInitialValue,
            COLOR_INTERPOLATION @ color_interpolation => EnumKeyword(enums::keyword_to_color_interpolation),
            COLOR_INTERPOLATION_FILTERS @ color_interpolation_filters => EnumKeyword(enums::keyword_to_color_interpolation),
            PAINT_ORDER => RequireKeyword(k::NORMAL),
            TEXT_ANCHOR @ text_anchor => EnumKeyword(enums::keyword_to_text_anchor),
            DOMINANT_BASELINE => RequireKeyword(k::AUTO),
            SHAPE_RENDERING @ shape_rendering => EnumKeyword(enums::keyword_to_shape_rendering),
        },
        group_index::INHERITED_LIST => fields! { InheritedListValues:
            LIST_STYLE_TYPE => RequireInitialValue,
            LIST_STYLE_POSITION @ list_style_position => EnumKeyword(enums::keyword_to_list_style_position),
            LIST_STYLE_IMAGE => RequireKeyword(k::NONE),
            QUOTES => RequireKeyword(k::AUTO),
        },
        group_index::CONTENT => fields! { ContentValues:
            CONTENT => RequireKeyword(k::NORMAL),
            COUNTER_INCREMENT => RequireKeyword(k::NONE),
            COUNTER_RESET => RequireKeyword(k::NONE),
            COUNTER_SET => RequireKeyword(k::NONE),
        },
        group_index::ANCHOR => fields! { AnchorValues:
            ANCHOR_NAME => RequireKeyword(k::NONE),
            ANCHOR_SCOPE => RequireKeyword(k::NONE),
            POSITION_ANCHOR => RequireKeyword(k::AUTO),
            POSITION_AREA => RequireKeyword(k::NONE),
            POSITION_TRY_FALLBACKS => RequireKeyword(k::NONE),
            POSITION_TRY_ORDER => RequireKeyword(k::NORMAL),
            POSITION_VISIBILITY => RequireKeyword(k::ALWAYS),
        },
        // A none border-style keeps the used width at zero; the Rust group builder completes styled
        // borders.
        group_index::BORDER => fields! { BorderValues:
            BORDER_LEFT_COLOR @ border_left.color => Color,
            BORDER_LEFT_COLOR => RequireInitialValue,
            BORDER_LEFT_STYLE => RequireKeyword(k::NONE),
            BORDER_LEFT_WIDTH @ border_left_computed_width => CssPixelsNonNegative,
            BORDER_TOP_COLOR @ border_top.color => Color,
            BORDER_TOP_COLOR => RequireInitialValue,
            BORDER_TOP_STYLE => RequireKeyword(k::NONE),
            BORDER_TOP_WIDTH @ border_top_computed_width => CssPixelsNonNegative,
            BORDER_RIGHT_COLOR @ border_right.color => Color,
            BORDER_RIGHT_COLOR => RequireInitialValue,
            BORDER_RIGHT_STYLE => RequireKeyword(k::NONE),
            BORDER_RIGHT_WIDTH @ border_right_computed_width => CssPixelsNonNegative,
            BORDER_BOTTOM_COLOR @ border_bottom.color => Color,
            BORDER_BOTTOM_COLOR => RequireInitialValue,
            BORDER_BOTTOM_STYLE => RequireKeyword(k::NONE),
            BORDER_BOTTOM_WIDTH @ border_bottom_computed_width => CssPixelsNonNegative,
            BORDER_BOTTOM_LEFT_RADIUS => RequireInitialValue,
            BORDER_BOTTOM_RIGHT_RADIUS => RequireInitialValue,
            BORDER_TOP_LEFT_RADIUS => RequireInitialValue,
            BORDER_TOP_RIGHT_RADIUS => RequireInitialValue,
            CORNER_BOTTOM_LEFT_SHAPE @ corner_bottom_left_shape => ResolvedF64,
            CORNER_BOTTOM_RIGHT_SHAPE @ corner_bottom_right_shape => ResolvedF64,
            CORNER_TOP_LEFT_SHAPE @ corner_top_left_shape => ResolvedF64,
            CORNER_TOP_RIGHT_SHAPE @ corner_top_right_shape => ResolvedF64,
            BORDER_IMAGE_SOURCE => RequireKeyword(k::NONE),
            BORDER_IMAGE_OUTSET => RequireInitialValue,
            BORDER_IMAGE_REPEAT => RequireInitialValue,
            BORDER_IMAGE_SLICE => RequireInitialValue,
            BORDER_IMAGE_WIDTH => RequireInitialValue,
        },
        group_index::BACKGROUND => fields! { BackgroundValues:
            BACKGROUND_COLOR @ background_color => Color,
            BACKGROUND_COLOR => RequireInitialValue,
            BACKGROUND_IMAGE => RequireInitialValue,
            BACKGROUND_CLIP => RequireInitialValue,
            BACKGROUND_ATTACHMENT => RequireInitialValue,
            BACKGROUND_ORIGIN => RequireInitialValue,
            BACKGROUND_POSITION_X => RequireInitialValue,
            BACKGROUND_POSITION_Y => RequireInitialValue,
            BACKGROUND_REPEAT => RequireInitialValue,
            BACKGROUND_SIZE => RequireInitialValue,
            BACKGROUND_BLEND_MODE => RequireInitialValue,
        },
        _ => &[],
    }
}

/// The computed style groups each longhand reaches.
pub(crate) struct StyleGroupMasks {
    /// Per longhand, the groups a change of its specified winner may change.
    masks: Box<[u32]>,
    /// Per longhand, the group that owns its output.
    output_masks: Box<[u32]>,
}

impl StyleGroupMasks {
    pub(crate) fn get() -> &'static Self {
        static MASKS: OnceLock<StyleGroupMasks> = OnceLock::new();
        MASKS.get_or_init(Self::compute)
    }

    fn compute() -> Self {
        use crate::css::property_metadata::property_id::{DIRECTION, WRITING_MODE};
        use crate::css::property_metadata::{
            FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, longhand_is_logical_alias,
            property_style_group_index,
        };
        use crate::css::style_compute::{DIRECTION_COUNT, WRITING_MODE_COUNT, map_logical_alias_to_physical};

        let group_mask = |property| property_style_group_index(property).map_or(0, |group| 1u32 << group);
        let index = |property: u16| usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
        let longhands = FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID;
        // Direction and writing-mode select the physical winners for every logical property group.
        // Those aliases own no payload, so they reach the groups of every physical property they
        // map to.
        let output_masks: Box<[u32]> = longhands
            .clone()
            .map(|property| {
                if longhand_is_logical_alias(property) {
                    0
                } else {
                    group_mask(property)
                }
            })
            .collect();
        let mut masks = output_masks.clone();

        let mut logical_mapping_mask = 0;
        for property in longhands.filter(|&property| longhand_is_logical_alias(property)) {
            let mut alias_mask = 0;
            for writing_mode in 0..WRITING_MODE_COUNT as u8 {
                for direction in 0..DIRECTION_COUNT as u8 {
                    alias_mask |= group_mask(map_logical_alias_to_physical(property, writing_mode, direction));
                }
            }
            masks[index(property)] |= alias_mask;
            logical_mapping_mask |= alias_mask;
        }
        masks[index(DIRECTION)] |= logical_mapping_mask;
        masks[index(WRITING_MODE)] |= logical_mapping_mask;
        Self { masks, output_masks }
    }

    fn longhand_index(property: u16) -> Option<usize> {
        use crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID;
        property.checked_sub(FIRST_LONGHAND_PROPERTY_ID).map(usize::from)
    }

    /// The computed style groups which may change when `property`'s specified winner changes; none
    /// for a longhand that feeds no group of its own.
    pub(crate) fn dependencies(&self, property: u16) -> u32 {
        Self::longhand_index(property)
            .and_then(|index| self.masks.get(index))
            .copied()
            .unwrap_or(0)
    }
}

/// The computed style group which directly owns one longhand's output.
pub(crate) fn computed_group_output_mask(property: u16) -> Option<u32> {
    let masks = StyleGroupMasks::get();
    StyleGroupMasks::longhand_index(property)
        .and_then(|index| masks.output_masks.get(index))
        .copied()
        .filter(|mask| *mask != 0)
}

/// One gathered value for the generic group builder: the computed value's
/// data, plus the resolved raw color for color-kind fields.
pub(crate) struct GroupValueEntry {
    pub(crate) data: *const c_void,
    pub(crate) resolved_color: u32,
    pub(crate) has_resolved_color: bool,
    pub(crate) resolved_number: f64,
    pub(crate) has_resolved_number: bool,
}

/// One decoded write into a scratch payload, at an offset into it.
enum GroupFieldPoke {
    U8(usize, u8),
    F32(usize, f32),
    F64(usize, f64),
    I32(usize, i32),
    U64(usize, u64),
    U32(usize, u32),
    Data(usize, *const crate::css::style_value::StyleValueData),
}

pub(crate) const MAX_GROUP_FIELD_COUNT: usize = 48;

struct GroupFieldPokes {
    entries: [std::mem::MaybeUninit<GroupFieldPoke>; MAX_GROUP_FIELD_COUNT],
    len: usize,
}

impl GroupFieldPokes {
    fn new() -> Self {
        Self {
            entries: [const { std::mem::MaybeUninit::uninit() }; MAX_GROUP_FIELD_COUNT],
            len: 0,
        }
    }

    fn push(&mut self, poke: GroupFieldPoke) {
        assert!(
            self.len < self.entries.len(),
            "a computed style group has too many fields"
        );
        self.entries[self.len].write(poke);
        self.len += 1;
    }
}

impl std::ops::Deref for GroupFieldPokes {
    type Target = [GroupFieldPoke];

    fn deref(&self) -> &Self::Target {
        // SAFETY: push initializes every entry below len and GroupFieldPoke has no drop glue.
        unsafe { std::slice::from_raw_parts(self.entries.as_ptr().cast(), self.len) }
    }
}

/// Decodes one group's gathered values against its descriptors into scratch
/// pokes. Constraint descriptors (the Require kinds) fail the decode when the
/// value diverges and constraints are enforced. A Rust group builder that
/// completes complex members itself skips those constraints.
fn decode_group_field_pokes(
    descriptors: &[GroupFieldDescriptor],
    values: &[GroupValueEntry],
    enforce_constraints: bool,
) -> Option<GroupFieldPokes> {
    use crate::css::style_compute::px_length_unit;
    use crate::css::style_value::StyleValueData;
    use GroupFieldKind as Kind;
    use GroupFieldPoke as Poke;

    assert!(values.len() <= MAX_GROUP_FIELD_COUNT);
    let mut pokes = GroupFieldPokes::new();
    for (descriptor, value) in descriptors.iter().zip(values) {
        let data = unsafe { (value.data as *const StyleValueData).as_ref() }?;
        let offset = descriptor.offset;
        let resolved_number = || value.has_resolved_number.then_some(value.resolved_number);
        match descriptor.kind {
            Kind::EnumKeyword(keyword_to_code) => {
                match data {
                    StyleValueData::Keyword { keyword } => match keyword_to_code(*keyword) {
                        Some(code) => pokes.push(Poke::U8(offset, code)),
                        // A keyword the converter rejects (the appearance compat keywords) stays
                        // at the payload default; the Rust group builder owns the field.
                        None if enforce_constraints => return None,
                        None => {}
                    },
                    _ if enforce_constraints => return None,
                    _ => {}
                }
            }
            Kind::F64 => {
                let StyleValueData::Number { value } = data else {
                    return None;
                };
                pokes.push(Poke::F64(offset, *value));
            }
            Kind::CssPixels => match data {
                StyleValueData::Length { value, unit } if *unit == px_length_unit() => pokes.push(Poke::I32(
                    offset,
                    crate::css::css_pixels::CssPixels::nearest_value_for(*value).raw_value(),
                )),
                // A value that is not a plain pixel length (the normal keyword, a percentage
                // against font metrics) stays at the payload default; the Rust group builder owns
                // the field.
                _ if enforce_constraints => return None,
                _ => {}
            },
            Kind::CssPixelsNonNegative => {
                let StyleValueData::Length { value, unit } = data else {
                    return None;
                };
                if *unit != px_length_unit() {
                    return None;
                }
                pokes.push(Poke::I32(
                    offset,
                    crate::css::css_pixels::CssPixels::nearest_value_for(value.max(0.0)).raw_value(),
                ));
            }
            Kind::U64 => {
                // A plain integer, or a calculation that resolves to one without context.
                let value = match data {
                    StyleValueData::Integer { value } => *value,
                    StyleValueData::Calculated { .. } => {
                        crate::css::calc::resolve_calculated_integer_without_context(data)?
                    }
                    _ => return None,
                };
                pokes.push(Poke::U64(offset, u64::try_from(value).ok()?));
            }
            Kind::Color => {
                if value.has_resolved_color {
                    pokes.push(Poke::U32(offset, value.resolved_color));
                } else if enforce_constraints {
                    return None;
                }
                // Otherwise a color the generic decoder cannot resolve stays at the payload
                // default; the Rust group builder owns the field.
            }
            Kind::ColorOrKeyword(keyword) => match data {
                StyleValueData::Keyword { keyword: value_keyword } if *value_keyword == keyword => {}
                _ if value.has_resolved_color => pokes.push(Poke::U32(offset, value.resolved_color)),
                // As for Color, the Rust group builder owns colors the generic decoder cannot
                // resolve.
                _ if enforce_constraints => return None,
                _ => {}
            },
            Kind::ResolvedF32 => pokes.push(Poke::F32(offset, resolved_number()? as f32)),
            Kind::ResolvedF64 => pokes.push(Poke::F64(offset, resolved_number()?)),
            Kind::ResolvedU8 => pokes.push(Poke::U8(offset, resolved_number()? as u8)),
            Kind::RetainedData => pokes.push(Poke::Data(offset, value.data.cast())),
            Kind::KeywordEqualsBool(keyword) => {
                let is_keyword =
                    matches!(data, StyleValueData::Keyword { keyword: value_keyword } if *value_keyword == keyword);
                pokes.push(Poke::U8(offset, is_keyword as u8));
            }
            Kind::RequireKeyword(_) | Kind::RequireZeroPx | Kind::RequireInitialValue if !enforce_constraints => {}
            Kind::RequireKeyword(keyword) => {
                // NB: Repeatable-list properties keep even a single computed item in a value list.
                let data = match data {
                    StyleValueData::ValueList { values, .. } if values.as_slice().len() == 1 => {
                        values.as_slice()[0].data()
                    }
                    data => data,
                };
                if !matches!(data, StyleValueData::Keyword { keyword: value_keyword } if *value_keyword == keyword) {
                    return None;
                }
            }
            Kind::RequireZeroPx => {
                if !matches!(data, StyleValueData::Length { value, unit } if *unit == px_length_unit() && *value == 0.0)
                {
                    return None;
                }
            }
            Kind::RequireInitialValue => {
                if value.data != crate::css::style_compute::initial_value_data(descriptor.property_id).cast() {
                    return None;
                }
            }
        }
    }
    Some(pokes)
}

/// Applies decoded field pokes to a default-constructed scratch payload.
///
/// # Safety
/// `scratch` must be a payload of the group whose descriptors produced the
/// pokes, so every offset names a field of the right type.
unsafe fn apply_group_field_pokes(scratch: *mut c_void, pokes: &[GroupFieldPoke]) {
    use crate::css::style_value::StyleValueData;
    use GroupFieldPoke as Poke;

    unsafe {
        for poke in pokes {
            let base = scratch as *mut u8;
            match *poke {
                Poke::U8(offset, value) => *base.add(offset) = value,
                Poke::F32(offset, value) => *(base.add(offset) as *mut f32) = value,
                Poke::F64(offset, value) => *(base.add(offset) as *mut f64) = value,
                Poke::I32(offset, value) => *(base.add(offset) as *mut i32) = value,
                Poke::U64(offset, value) => *(base.add(offset) as *mut u64) = value,
                Poke::U32(offset, value) => *(base.add(offset) as *mut u32) = value,
                Poke::Data(offset, data) => {
                    // The slot's constructor default is null, so nothing is released.
                    let retained = crate::css::style_value::retain_style_value(data);
                    *(base.add(offset) as *mut *const StyleValueData) = retained;
                }
            }
        }
    }
}

/// Frees a scratch payload that was not published.
///
/// # Safety
/// `scratch` must be a payload of the group, allocated by
/// `allocate_payload` and not shared.
unsafe fn free_scratch_payload(group: StyleGroup, scratch: *mut c_void) {
    unsafe {
        destruct(group, scratch);
        let allocation = (scratch as *mut u8).sub(header_size(payload_align(group)));
        dealloc(allocation, allocation_layout(group));
    }
}

/// Builds a style group payload generically from its registered field
/// descriptors: decodes every descriptor's value (returning null for the C++
/// population path when any value cannot be decoded or a constraint fails),
/// default-constructs a scratch payload, pokes the simple fields, and shares
/// the parent or default payload when the result compares equal.
///
/// # Safety
/// `values` must hold one valid data entry per registered descriptor
/// of the group, in registration order; `parent_payload` must be a valid
/// payload of the group or null.
pub(crate) unsafe fn build_style_group(
    group_index: usize,
    values: &[GroupValueEntry],
    parent_payload: *const c_void,
) -> *const c_void {
    let build_or_reuse_group_payload = || {
        let descriptors = group_field_descriptors(group_index);
        if descriptors.len() != values.len() {
            return None;
        }
        let pokes = decode_group_field_pokes(descriptors, values, true)?;

        let group = style_group(group_index);

        // A group whose descriptors are all satisfied constraints would
        // scratch-build an exact copy of the default payload, so skip the
        // allocation and share directly.
        if pokes.is_empty() {
            let default_payload = default_group_payload(group_index);
            if !parent_payload.is_null() && unsafe { payloads_equal(group, parent_payload, default_payload) } {
                retain_group_payload(group_index, parent_payload);
                return Some(parent_payload);
            }
            return Some(default_payload);
        }

        let scratch = allocate_payload(group, 1);
        // SAFETY: The scratch payload was allocated for this group's layout,
        // and every poke offset comes from offsetof on the C++ side.
        unsafe {
            default_construct(group, scratch);
            apply_group_field_pokes(scratch, &pokes);
        }

        if !parent_payload.is_null() && unsafe { payloads_equal(group, scratch, parent_payload) } {
            unsafe { free_scratch_payload(group, scratch) };
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if unsafe { payloads_equal(group, scratch, default_payload) } {
            unsafe { free_scratch_payload(group, scratch) };
            return Some(default_payload);
        }
        Some(scratch as *const c_void)
    };
    build_or_reuse_group_payload().unwrap_or(std::ptr::null())
}

/// Shares one group's immortal default payload - or the parent payload when
/// it equals the default, keeping the identity - for a build whose inputs are
/// all initial values.
///
/// # Safety
/// `parent_payload` must be a valid payload of the group or null.
pub(crate) unsafe fn share_default_group_payload(group_index: usize, parent_payload: *const c_void) -> *const c_void {
    let group = style_group(group_index);
    let default_payload = default_group_payload(group_index);
    if !parent_payload.is_null() && unsafe { payloads_equal(group, parent_payload, default_payload) } {
        retain_group_payload(group_index, parent_payload);
        return parent_payload;
    }
    default_payload
}

/// Builds a Rust-native group whose descriptor-backed fields are completed by
/// a Rust closure before the normal parent/default sharing checks.
///
/// # Safety
/// `values` must hold one valid data entry per registered descriptor of the
/// group in registration order, and `parent_payload` must be a valid payload
/// of the group or null. The closure must cast the payload to the registered
/// Rust-native type only.
pub(crate) unsafe fn build_group_payload_with_rust_fill(
    group_index: usize,
    values: &[GroupValueEntry],
    fill: impl FnOnce(*mut c_void),
    parent_payload: *const c_void,
) -> *const c_void {
    let descriptors = group_field_descriptors(group_index);
    assert_eq!(descriptors.len(), values.len());
    let pokes = decode_group_field_pokes(descriptors, values, false)
        .expect("computed values decode for every non-constraint descriptor");

    let group = style_group(group_index);
    let scratch = allocate_payload(group, 1);
    // SAFETY: The scratch payload was allocated for this group's Rust-native
    // layout, and every poke offset comes from offsetof on the C++ mirror.
    unsafe {
        default_construct(group, scratch);
        apply_group_field_pokes(scratch, &pokes);
    }
    fill(scratch);

    if !parent_payload.is_null() && unsafe { payloads_equal(group, scratch, parent_payload) } {
        unsafe { free_scratch_payload(group, scratch) };
        retain_group_payload(group_index, parent_payload);
        return parent_payload;
    }
    let default_payload = default_group_payload(group_index);
    if unsafe { payloads_equal(group, scratch, default_payload) } {
        unsafe { free_scratch_payload(group, scratch) };
        return default_payload;
    }
    scratch as *const c_void
}

/// Builds an inherited box group payload from the five computed keyword
/// values, sharing instead of allocating whenever it can: the parent's
/// payload when every field matches it, or the immortal default payload when
/// every field holds its initial value. Every input is a canonical computed
/// keyword.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// The value pointers must be valid StyleValueData or null, and
/// `parent_payload` a valid inherited box payload or null.
pub(crate) unsafe fn build_inherited_box_group(
    group_index: usize,
    visibility: *const c_void,
    direction: *const c_void,
    writing_mode: *const c_void,
    content_visibility: *const c_void,
    image_rendering: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> u8 {
        match unsafe { (data as *const StyleValueData).as_ref() } {
            Some(StyleValueData::Keyword { keyword }) => map(*keyword).expect("computed keyword has a supported value"),
            _ => panic!("computed inherited box value is not a keyword"),
        }
    };
    let built = InheritedBoxValues {
        visibility: keyword_code(visibility, crate::css::style_compute::keyword_to_visibility),
        direction: keyword_code(direction, crate::css::style_compute::keyword_to_direction),
        writing_mode: keyword_code(writing_mode, crate::css::style_compute::keyword_to_writing_mode),
        content_visibility: keyword_code(
            content_visibility,
            crate::css::style_compute::keyword_to_content_visibility,
        ),
        image_rendering: keyword_code(image_rendering, crate::css::style_compute::keyword_to_image_rendering),
    };

    if !parent_payload.is_null() {
        // SAFETY: The caller guarantees a valid inherited box payload.
        if built == unsafe { *(parent_payload as *const InheritedBoxValues) } {
            retain_group_payload(group_index, parent_payload);
            return parent_payload;
        }
    }

    let default_payload = default_group_payload(group_index);
    // SAFETY: The default payload is a valid inherited box payload.
    if built == unsafe { *(default_payload as *const InheritedBoxValues) } {
        return default_payload;
    }

    let payload = allocate_payload(style_group(group_index), 1);
    // SAFETY: The payload was allocated for this group's layout.
    unsafe { *(payload as *mut InheritedBoxValues) = built };
    payload as *const c_void
}

impl ComputedSize {
    pub(crate) fn keyword(kind: ComputedSizeKind) -> Self {
        Self {
            kind,
            value: ComputedStyleValueHandle::empty(),
        }
    }

    fn retained(kind: ComputedSizeKind, data: *const crate::css::style_value::StyleValueData) -> Self {
        Self {
            kind,
            value: ComputedStyleValueHandle::retained(data),
        }
    }

    pub(crate) fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        match unsafe { data.as_ref() } {
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::AUTO => {
                Self::keyword(ComputedSizeKind::Auto)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::FIT_CONTENT => {
                Self::keyword(ComputedSizeKind::FitContent)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::MIN_CONTENT => {
                Self::keyword(ComputedSizeKind::MinContent)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::MAX_CONTENT => {
                Self::keyword(ComputedSizeKind::MaxContent)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::NONE => {
                Self::keyword(ComputedSizeKind::None)
            }
            Some(StyleValueData::Function { value, .. }) => {
                Self::retained(ComputedSizeKind::FitContent, value.pointer())
            }
            Some(StyleValueData::Calculated { .. }) => Self::retained(ComputedSizeKind::Calculated, data),
            Some(StyleValueData::Percentage { .. }) => Self::retained(ComputedSizeKind::Percentage, data),
            Some(StyleValueData::Length { .. }) => Self::retained(ComputedSizeKind::Length, data),
            // FIXME: Support `anchor-size(..)`.
            Some(StyleValueData::AnchorSize { .. }) => Self::keyword(ComputedSizeKind::None),
            _ => Self::keyword(ComputedSizeKind::Auto),
        }
    }
}

impl SizingValues {
    fn initial() -> Self {
        Self {
            width: ComputedSize::keyword(ComputedSizeKind::Auto),
            min_width: ComputedSize::keyword(ComputedSizeKind::Auto),
            max_width: ComputedSize::keyword(ComputedSizeKind::None),
            height: ComputedSize::keyword(ComputedSizeKind::Auto),
            min_height: ComputedSize::keyword(ComputedSizeKind::Auto),
            max_height: ComputedSize::keyword(ComputedSizeKind::None),
        }
    }
}

impl ComputedFlexBasis {
    fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let is_content = matches!(
            unsafe { data.cast::<StyleValueData>().as_ref() },
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::CONTENT
        );
        Self {
            is_content,
            size: if is_content {
                ComputedSize::keyword(ComputedSizeKind::Auto)
            } else {
                ComputedSize::from_data(data)
            },
        }
    }
}

impl ComputedGap {
    fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        let is_normal = matches!(
            unsafe { data.as_ref() },
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL
        );
        Self {
            is_normal,
            value: if is_normal {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::retained(data)
            },
        }
    }
}

impl AlignmentValues {
    fn initial() -> Self {
        use crate::css::css_enums::{
            align_content, align_items, align_self, flex_direction, flex_wrap, justify_content, justify_items,
            justify_self,
        };

        Self {
            webkit_box_orient: crate::css::css_enums::webkit_box_orient::HORIZONTAL,
            flex_direction: flex_direction::ROW,
            flex_wrap: flex_wrap::NOWRAP,
            flex_basis: ComputedFlexBasis {
                is_content: false,
                size: ComputedSize::keyword(ComputedSizeKind::Auto),
            },
            flex_grow: 0.0,
            flex_shrink: 1.0,
            order: 0,
            align_content: align_content::STRETCH,
            align_items: align_items::STRETCH,
            align_self: align_self::AUTO,
            justify_content: justify_content::FLEX_START,
            justify_items: justify_items::LEGACY,
            justify_self: justify_self::AUTO,
            column_gap: ComputedGap {
                is_normal: true,
                value: ComputedStyleValueHandle::empty(),
            },
            row_gap: ComputedGap {
                is_normal: true,
                value: ComputedStyleValueHandle::empty(),
            },
        }
    }
}

impl ComputedLengthPercentageOrAuto {
    fn auto() -> Self {
        Self {
            is_auto: true,
            value: ComputedStyleValueHandle::empty(),
        }
    }

    fn zero() -> Self {
        Self {
            is_auto: false,
            value: ComputedStyleValueHandle::length(0.0),
        }
    }

    pub(crate) fn from_data(data: *const c_void) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        let is_auto = matches!(
            unsafe { data.as_ref() },
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::AUTO
        );
        Self {
            is_auto,
            value: if is_auto {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::retained(data)
            },
        }
    }

    fn from_length_box_data(data: *const c_void, default_is_auto: bool) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let data = data.cast::<StyleValueData>();
        match unsafe { data.as_ref() } {
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::AUTO => Self::auto(),
            Some(
                StyleValueData::Length { .. } | StyleValueData::Percentage { .. } | StyleValueData::Calculated { .. },
            ) => Self {
                is_auto: false,
                value: ComputedStyleValueHandle::retained(data),
            },
            _ if default_is_auto => Self::auto(),
            _ => Self::zero(),
        }
    }
}

impl ComputedLengthBox {
    pub(crate) fn auto() -> Self {
        Self {
            top: ComputedLengthPercentageOrAuto::auto(),
            right: ComputedLengthPercentageOrAuto::auto(),
            bottom: ComputedLengthPercentageOrAuto::auto(),
            left: ComputedLengthPercentageOrAuto::auto(),
        }
    }

    pub(crate) fn zero() -> Self {
        let zero = ComputedStyleValueHandle::length(0.0);
        let side = || ComputedLengthPercentageOrAuto {
            is_auto: false,
            value: zero.clone(),
        };
        Self {
            top: side(),
            right: side(),
            bottom: side(),
            left: side(),
        }
    }

    pub(crate) fn from_data(
        top: *const c_void,
        right: *const c_void,
        bottom: *const c_void,
        left: *const c_void,
        default_is_auto: bool,
    ) -> Self {
        Self {
            top: ComputedLengthPercentageOrAuto::from_length_box_data(top, default_is_auto),
            right: ComputedLengthPercentageOrAuto::from_length_box_data(right, default_is_auto),
            bottom: ComputedLengthPercentageOrAuto::from_length_box_data(bottom, default_is_auto),
            left: ComputedLengthPercentageOrAuto::from_length_box_data(left, default_is_auto),
        }
    }
}

impl SurroundValues {
    fn initial() -> Self {
        Self {
            inset: ComputedLengthBox::auto(),
            top_anchor_inset: ComputedStyleValueHandle::empty(),
            right_anchor_inset: ComputedStyleValueHandle::empty(),
            bottom_anchor_inset: ComputedStyleValueHandle::empty(),
            left_anchor_inset: ComputedStyleValueHandle::empty(),
            top_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            right_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            bottom_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            left_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            position_anchor: ComputedStyleValueHandle::empty(),
            margin: ComputedLengthBox::zero(),
            padding: ComputedLengthBox::zero(),
        }
    }
}

impl ComputedAspectRatio {
    fn auto() -> Self {
        Self {
            use_natural_aspect_ratio_if_available: true,
            has_preferred_ratio: false,
            preferred_ratio_numerator: 0.0,
            preferred_ratio_denominator: 0.0,
            computed_use_natural_aspect_ratio_if_available: true,
            has_computed_ratio: false,
            computed_ratio_numerator: 0.0,
            computed_ratio_denominator: 0.0,
        }
    }
}

impl BoxValues {
    fn initial() -> Self {
        use crate::css::css_enums::{
            box_sizing, clear, float, overflow, positioning, resize, table_layout, text_overflow, unicode_bidi,
            vertical_align,
        };
        use crate::css::display::FfiDisplay;

        Self {
            display: FfiDisplay::inline(),
            display_before_box_type_transformation: FfiDisplay::inline(),
            float_: float::NONE,
            clear: clear::NONE,
            position: positioning::STATIC,
            overflow_x: overflow::VISIBLE,
            overflow_y: overflow::VISIBLE,
            box_sizing: box_sizing::CONTENT_BOX,
            resize: resize::NONE,
            text_overflow: text_overflow::CLIP,
            unicode_bidi: unicode_bidi::NORMAL,
            table_layout: table_layout::AUTO,
            grid_auto_flow_row: true,
            grid_auto_flow_dense: false,
            column_width: ComputedSize::keyword(ComputedSizeKind::Auto),
            column_count_has_value: false,
            column_count: 0,
            continue_: crate::css::css_enums::continue_value::AUTO,
            max_lines: 0,
            has_z_index: false,
            z_index: 0,
            vertical_align: ComputedVerticalAlign {
                is_keyword: true,
                keyword: vertical_align::BASELINE,
                value: ComputedStyleValueHandle::empty(),
            },
            aspect_ratio: ComputedAspectRatio::auto(),
            contain_intrinsic_width: ComputedContainIntrinsicSize::none(),
            contain_intrinsic_height: ComputedContainIntrinsicSize::none(),
            size_containment: false,
            inline_size_containment: false,
            layout_containment: false,
            style_containment: false,
            paint_containment: false,
            is_size_container: false,
            is_inline_size_container: false,
            is_scroll_state_container: false,
            container_name: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
        }
    }
}

impl SVGResetValues {
    fn initial() -> Self {
        use crate::css::css_enums::{keyword, vector_effect};

        const OPAQUE_BLACK_BGRA: u32 = 0xff00_0000;

        let zero = ComputedStyleValueHandle::length(0.0);
        Self {
            cx: zero.clone(),
            cy: zero.clone(),
            d: ComputedStyleValueHandle::keyword(keyword::NONE),
            r: zero.clone(),
            rx: ComputedLengthPercentageOrAuto {
                is_auto: true,
                value: ComputedStyleValueHandle::empty(),
            },
            ry: ComputedLengthPercentageOrAuto {
                is_auto: true,
                value: ComputedStyleValueHandle::empty(),
            },
            x: zero.clone(),
            y: zero,
            stop_color: OPAQUE_BLACK_BGRA,
            stop_opacity: 1.0,
            flood_color: OPAQUE_BLACK_BGRA,
            flood_opacity: 1.0,
            vector_effect: vector_effect::NONE,
        }
    }
}

impl TextResetValues {
    fn initial() -> Self {
        use crate::css::css_enums::{keyword, keyword_to_text_decoration_style};

        Self {
            text_decoration_lines: RetainedTextDecorationLineList::from_vec(Vec::new()),
            text_decoration_thickness_kind: 0,
            text_decoration_thickness: ComputedStyleValueHandle::empty(),
            text_decoration_style: keyword_to_text_decoration_style(keyword::SOLID)
                .expect("solid maps to TextDecorationStyle"),
            text_decoration_color: 0xff00_0000,
            white_space_trim_discard_before: false,
            white_space_trim_discard_after: false,
            white_space_trim_discard_inner: false,
        }
    }
}

impl TransformValues {
    fn initial() -> Self {
        Self {
            transformations: ComputedStyleValueHandle::empty(),
            resolved_transforms: RetainedComputedResolvedTransformList::from_vec(Vec::new()),
            // TransformBox::ViewBox, TransformStyle::Flat, and
            // BackfaceVisibility::Visible. The generated C++ enum values are
            // pinned by static assertions beside the payload layout checks.
            transform_box: 4,
            transform_origin_x: ComputedStyleValueHandle::percentage(50.0),
            transform_origin_y: ComputedStyleValueHandle::percentage(50.0),
            transform_origin_z: ComputedStyleValueHandle::length(0.0),
            transform_style: 0,
            backface_visibility: 0,
            rotate: ComputedStyleValueHandle::empty(),
            translate: ComputedStyleValueHandle::empty(),
            scale: ComputedStyleValueHandle::empty(),
            has_perspective: false,
            perspective_px: 0,
            perspective_origin_x: ComputedStyleValueHandle::percentage(50.0),
            perspective_origin_y: ComputedStyleValueHandle::percentage(50.0),
        }
    }
}

impl EffectsValues {
    fn initial_filter() -> ComputedFilter {
        ComputedFilter {
            filter_list: ComputedStyleValueHandle::empty(),
            operations: RetainedComputedFilterOperationList::from_vec(Vec::new()),
        }
    }

    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        let auto_edge = ComputedClipEdge {
            is_auto: true,
            value: 0.0,
            unit: crate::css::style_compute::px_length_unit(),
        };
        Self {
            opacity: 1.0,
            filter: Self::initial_filter(),
            backdrop_filter: Self::initial_filter(),
            // MixBlendMode::Normal and Isolation::Auto.
            mix_blend_mode: 0,
            isolation: 0,
            box_shadows: RetainedComputedShadowList::from_vec(Vec::new()),
            clip_is_rect: false,
            clip_edges: [auto_edge; 4],
            opacity_style_value: initial(property_id::OPACITY),
            filter_style_value: initial(property_id::FILTER),
            backdrop_filter_style_value: initial(property_id::BACKDROP_FILTER),
            mix_blend_mode_style_value: initial(property_id::MIX_BLEND_MODE),
            isolation_style_value: initial(property_id::ISOLATION),
            box_shadow_style_value: initial(property_id::BOX_SHADOW),
            clip_style_value: initial(property_id::CLIP),
        }
    }
}

impl AnchorValues {
    fn initial() -> Self {
        Self {
            anchor_names: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
            anchor_scope_all: false,
            anchor_scope_names: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
            // PositionAnchor::Type::Normal.
            position_anchor_type: 0,
            position_anchor_name: RetainedUtf16FlyString::none(),
            position_area: RetainedPositionAreaList::from_vec(Vec::new()),
            position_try_fallbacks: RetainedPositionTryFallbackList::from_vec(Vec::new()),
            has_position_try_order: false,
            position_try_order: 0,
            position_visibility_always: false,
            position_visibility_anchors_valid: false,
            position_visibility_anchors_visible: true,
            position_visibility_no_overflow: false,
        }
    }
}

impl InheritedUIValues {
    fn initial() -> Self {
        Self {
            caret_color: ComputedColorOrAuto {
                is_auto: true,
                computed_color: 0xff00_0000,
                used_color: 0xff00_0000,
            },
            accent_color: ComputedColorOrAuto {
                is_auto: true,
                computed_color: 0xff00_0000,
                used_color: 0xff00_0000,
            },
            cursor: RetainedComputedCursorList::from_vec(vec![ComputedCursor {
                is_cursor_value: false,
                cursor: ComputedStyleValueHandle::empty(),
                predefined: crate::css::css_enums::cursor_predefined::AUTO,
            }]),
            pointer_events: crate::css::css_enums::pointer_events::AUTO,
            scrollbar_color: crate::css::computed_value_types::ComputedScrollbarColor {
                thumb_color: 0,
                track_color: 0,
                is_auto: true,
            },
            color_scheme: 0,
            color_schemes: RetainedUtf16FlyStringList::from_retained_strings(Vec::new()),
            color_scheme_only: false,
        }
    }
}

impl InheritedSVGValues {
    fn initial_paint(kind: u8, color: u32) -> crate::css::computed_value_types::ComputedSvgPaint {
        crate::css::computed_value_types::ComputedSvgPaint {
            kind,
            url: ComputedStyleValueHandle::empty(),
            has_color: kind == crate::css::computed_value_types::SVG_PAINT_COLOR,
            color,
            color_is_currentcolor: false,
        }
    }

    fn initial() -> Self {
        use crate::css::css_enums;
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        let color_interpolation = |keyword| {
            css_enums::keyword_to_color_interpolation(keyword)
                .expect("an initial color-interpolation keyword maps to its enum")
        };
        Self {
            fill: Self::initial_paint(1, 0xff00_0000),
            stroke: Self::initial_paint(0, 0),
            fill_rule: 0,
            clip_rule: 0,
            fill_opacity: 1.0,
            stroke_opacity: 1.0,
            stroke_linecap: 0,
            stroke_linejoin: 0,
            stroke_dasharray: RetainedComputedSvgDashList::from_vec(Vec::new()),
            stroke_dashoffset: initial(property_id::STROKE_DASHOFFSET),
            stroke_miterlimit: 4.0,
            stroke_width: initial(property_id::STROKE_WIDTH),
            color_interpolation: color_interpolation(css_enums::keyword::SRGB),
            color_interpolation_filters: color_interpolation(css_enums::keyword::LINEARRGB),
            paint_order: [0, 1, 2],
            paint_order_serialization_length: 0,
            paint_order_is_normal: true,
            text_anchor: 0,
            has_dominant_baseline: false,
            dominant_baseline: 0,
            shape_rendering: 0,
        }
    }
}

impl InheritedTextValues {
    fn initial() -> Self {
        use crate::css::css_enums;
        use crate::css::css_pixels::CssPixels;

        let enum_value = |value: Option<u8>| value.expect("an initial inherited-text keyword maps to its enum");
        Self {
            text_align: enum_value(css_enums::keyword_to_text_align(css_enums::keyword::START)),
            text_justify: enum_value(css_enums::keyword_to_text_justify(css_enums::keyword::AUTO)),
            white_space_collapse: enum_value(css_enums::keyword_to_white_space_collapse(css_enums::keyword::COLLAPSE)),
            text_wrap_mode: enum_value(css_enums::keyword_to_text_wrap_mode(css_enums::keyword::WRAP)),
            word_break: enum_value(css_enums::keyword_to_word_break(css_enums::keyword::NORMAL)),
            tab_size_is_number: true,
            letter_spacing: CssPixels::default(),
            word_spacing: CssPixels::default(),
            tab_size_length: CssPixels::default(),
            tab_size_number: 8.0,
            text_indent: ComputedTextIndent {
                length_percentage: ComputedStyleValueHandle::length(0.0),
                each_line: false,
                hanging: false,
            },
            color: 0xff00_0000,
            color_style_value: ComputedStyleValueHandle::empty(),
            font_smoothing: css_enums::font_smoothing::AUTO,
            webkit_text_fill_color: 0xff00_0000,
            webkit_text_fill_color_is_current_color: true,
            text_shadow: RetainedComputedShadowList::from_vec(Vec::new()),
            text_transform: enum_value(css_enums::keyword_to_text_transform(css_enums::keyword::NONE)),
            text_wrap_style: enum_value(css_enums::keyword_to_text_wrap_style(css_enums::keyword::AUTO)),
            text_decoration_skip_ink: enum_value(css_enums::keyword_to_text_decoration_skip_ink(
                css_enums::keyword::AUTO,
            )),
            text_underline_position: ComputedTextUnderlinePosition {
                horizontal: 0,
                vertical: 0,
            },
            text_underline_offset: ComputedTextUnderlineOffset {
                used_value: CssPixels::from_integer(2),
                is_auto: true,
                value: ComputedStyleValueHandle::empty(),
            },
            overflow_wrap: enum_value(css_enums::keyword_to_overflow_wrap(css_enums::keyword::NORMAL)),
            block_ellipsis: ComputedStyleValueHandle::keyword(css_enums::keyword::NO_ELLIPSIS),
            word_spacing_style_value: ComputedStyleValueHandle::empty(),
            letter_spacing_style_value: ComputedStyleValueHandle::empty(),
            orphans: 2,
            widows: 2,
        }
    }
}

impl AnimationValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            animation_name: initial(property_id::ANIMATION_NAME),
            animation_composition: initial(property_id::ANIMATION_COMPOSITION),
            animation_delay: initial(property_id::ANIMATION_DELAY),
            animation_direction: initial(property_id::ANIMATION_DIRECTION),
            animation_duration: initial(property_id::ANIMATION_DURATION),
            animation_fill_mode: initial(property_id::ANIMATION_FILL_MODE),
            animation_iteration_count: initial(property_id::ANIMATION_ITERATION_COUNT),
            animation_play_state: initial(property_id::ANIMATION_PLAY_STATE),
            animation_timeline: initial(property_id::ANIMATION_TIMELINE),
            animation_timing_function: initial(property_id::ANIMATION_TIMING_FUNCTION),
            scroll_timeline_name: initial(property_id::SCROLL_TIMELINE_NAME),
            scroll_timeline_axis: initial(property_id::SCROLL_TIMELINE_AXIS),
            timeline_scope: initial(property_id::TIMELINE_SCOPE),
            view_timeline_name: initial(property_id::VIEW_TIMELINE_NAME),
            view_timeline_axis: initial(property_id::VIEW_TIMELINE_AXIS),
            view_timeline_inset: initial(property_id::VIEW_TIMELINE_INSET),
            transition_property: initial(property_id::TRANSITION_PROPERTY),
            transition_duration: initial(property_id::TRANSITION_DURATION),
            transition_timing_function: initial(property_id::TRANSITION_TIMING_FUNCTION),
            transition_delay: initial(property_id::TRANSITION_DELAY),
            transition_behavior: initial(property_id::TRANSITION_BEHAVIOR),
            transition_delay_and_duration_are_single_zero: true,
        }
    }
}

impl MaskValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            mask_image: initial(property_id::MASK_IMAGE),
            mask_type: initial(property_id::MASK_TYPE),
            clip_path: initial(property_id::CLIP_PATH),
            mask_mode: initial(property_id::MASK_MODE),
            mask_repeat: initial(property_id::MASK_REPEAT),
            mask_position: initial(property_id::MASK_POSITION),
            mask_clip: initial(property_id::MASK_CLIP),
            mask_origin: initial(property_id::MASK_ORIGIN),
            mask_size: initial(property_id::MASK_SIZE),
            mask_composite: initial(property_id::MASK_COMPOSITE),
        }
    }
}

impl BackgroundValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            background_color: 0,
            background_color_style_value: initial(property_id::BACKGROUND_COLOR),
            // BackgroundBox::BorderBox.
            background_color_clip: 0,
            background_image: initial(property_id::BACKGROUND_IMAGE),
            background_attachment: initial(property_id::BACKGROUND_ATTACHMENT),
            background_blend_mode: initial(property_id::BACKGROUND_BLEND_MODE),
            background_clip: initial(property_id::BACKGROUND_CLIP),
            background_origin: initial(property_id::BACKGROUND_ORIGIN),
            background_position_x: initial(property_id::BACKGROUND_POSITION_X),
            background_position_y: initial(property_id::BACKGROUND_POSITION_Y),
            background_repeat: initial(property_id::BACKGROUND_REPEAT),
            background_size: initial(property_id::BACKGROUND_SIZE),
        }
    }
}

impl BorderValues {
    fn initial() -> Self {
        use crate::css::computed_value_types::ComputedBorderSide;
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        let side = || ComputedBorderSide {
            color: 0,
            line_style: 0,
            width: CssPixels::default(),
        };
        Self {
            border_left: side(),
            border_top: side(),
            border_right: side(),
            border_bottom: side(),
            border_left_color_style_value: initial(property_id::BORDER_LEFT_COLOR),
            border_top_color_style_value: initial(property_id::BORDER_TOP_COLOR),
            border_right_color_style_value: initial(property_id::BORDER_RIGHT_COLOR),
            border_bottom_color_style_value: initial(property_id::BORDER_BOTTOM_COLOR),
            border_left_computed_width: CssPixels::default(),
            border_top_computed_width: CssPixels::default(),
            border_right_computed_width: CssPixels::default(),
            border_bottom_computed_width: CssPixels::default(),
            border_bottom_left_radius: initial(property_id::BORDER_BOTTOM_LEFT_RADIUS),
            border_bottom_right_radius: initial(property_id::BORDER_BOTTOM_RIGHT_RADIUS),
            border_top_left_radius: initial(property_id::BORDER_TOP_LEFT_RADIUS),
            border_top_right_radius: initial(property_id::BORDER_TOP_RIGHT_RADIUS),
            has_noninitial_border_radii: false,
            corner_bottom_left_shape: 1.0,
            corner_bottom_right_shape: 1.0,
            corner_top_left_shape: 1.0,
            corner_top_right_shape: 1.0,
            border_image_source: initial(property_id::BORDER_IMAGE_SOURCE),
            border_image_slice: initial(property_id::BORDER_IMAGE_SLICE),
            border_image_width: initial(property_id::BORDER_IMAGE_WIDTH),
            border_image_outset: initial(property_id::BORDER_IMAGE_OUTSET),
            border_image_repeat: initial(property_id::BORDER_IMAGE_REPEAT),
        }
    }
}

impl ContentValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            content: initial(property_id::CONTENT),
            counter_increment: initial(property_id::COUNTER_INCREMENT),
            counter_reset: initial(property_id::COUNTER_RESET),
            counter_set: initial(property_id::COUNTER_SET),
        }
    }

    fn generated_content_facts(&self) -> u8 {
        use crate::css::css_enums::keyword;
        use crate::layout::counters::LIST_ITEM_COUNTER_NAME;

        let mut facts = 0;
        for counters in [&self.counter_increment, &self.counter_reset, &self.counter_set] {
            let Some(StyleValueData::CounterDefinitions { counter_definitions }) = counters.data() else {
                continue;
            };
            let definitions = counter_definitions.as_slice();
            if !definitions.is_empty() {
                facts |= GENERATED_CONTENT_DEFINES_COUNTERS;
            }
            if definitions
                .iter()
                .any(|definition| definition.name().units() == LIST_ITEM_COUNTER_NAME)
            {
                facts |= GENERATED_CONTENT_DEFINES_LIST_ITEM_COUNTER;
            }
        }
        let Some(StyleValueData::Content { content, alt_text }) = self.content.data() else {
            return facts;
        };
        fn items(
            list: &crate::css::style_value::RetainedStyleValueData,
        ) -> &[crate::css::style_value::RetainedStyleValueData] {
            match list.optional_data() {
                Some(StyleValueData::ValueList { values, .. }) => values.as_slice(),
                _ => &[],
            }
        }
        for item in items(content) {
            if let StyleValueData::Keyword { keyword } = item.data()
                && matches!(
                    *keyword,
                    keyword::OPEN_QUOTE | keyword::CLOSE_QUOTE | keyword::NO_OPEN_QUOTE | keyword::NO_CLOSE_QUOTE
                )
            {
                facts |= GENERATED_CONTENT_HAS_QUOTES;
            }
        }
        if items(content).iter().chain(items(alt_text)).any(|item| {
            matches!(item.data(), StyleValueData::Counter { counter_name, .. } if counter_name.units() == LIST_ITEM_COUNTER_NAME)
        }) {
            facts |= GENERATED_CONTENT_SHOWS_LIST_ITEM_COUNTER;
        }
        facts
    }
}

/// What a content group's generated content involves, which the DOM asks to know which changes
/// can renumber or requote it: counter-increment, counter-reset or counter-set define a counter,
/// one of them defines list-item, the content or its alternative text shows list-item, and the
/// content has a quote.
pub const GENERATED_CONTENT_DEFINES_COUNTERS: u8 = 1 << 0;
pub const GENERATED_CONTENT_DEFINES_LIST_ITEM_COUNTER: u8 = 1 << 1;
pub const GENERATED_CONTENT_SHOWS_LIST_ITEM_COUNTER: u8 = 1 << 2;
pub const GENERATED_CONTENT_HAS_QUOTES: u8 = 1 << 3;

#[unsafe(no_mangle)]
pub extern "C" fn rust_content_group_generated_content_facts(content: &ContentValues) -> u8 {
    content.generated_content_facts()
}

impl InheritedListValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            list_style_type: initial(property_id::LIST_STYLE_TYPE),
            list_style_position: 1,
            list_style_image: initial(property_id::LIST_STYLE_IMAGE),
            quotes: initial(property_id::QUOTES),
        }
    }
}

impl ComputedOverflowClipMarginSide {
    fn initial() -> Self {
        Self {
            has_visual_box: false,
            visual_box: 0,
            offset: crate::css::css_pixels::CssPixels::from_raw(0),
        }
    }
}

impl ComputedOverflowClipMargin {
    fn initial() -> Self {
        Self {
            left: ComputedOverflowClipMarginSide::initial(),
            top: ComputedOverflowClipMarginSide::initial(),
            right: ComputedOverflowClipMarginSide::initial(),
            bottom: ComputedOverflowClipMarginSide::initial(),
        }
    }
}

impl MiscResetValues {
    fn initial() -> Self {
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            outline_offset_style_value: initial(property_id::OUTLINE_OFFSET),
            scroll_margin: ComputedLengthBox::zero(),
            scroll_padding: ComputedLengthBox::auto(),
            overflow_clip_margin: ComputedOverflowClipMargin::initial(),
            column_span: 0,
            break_before: crate::css::css_enums::break_between::AUTO,
            break_after: crate::css::css_enums::break_between::AUTO,
            break_inside: crate::css::css_enums::break_inside::AUTO,
            column_rule_style: crate::css::css_enums::line_style::NONE,
            box_decoration_break: crate::css::css_enums::box_decoration_break::SLICE,
            column_fill: crate::css::css_enums::column_fill::BALANCE,
            appearance: 0,
            computed_appearance: 7,
            outline_style: 1,
            object_fit: 0,
            column_height: ComputedSize::keyword(ComputedSizeKind::Auto),
            outline_color: 0xff000000,
            outline_width: CssPixels::from_integer(3),
            column_rule_color: 0xff000000,
            column_rule_width: CssPixels::from_integer(3),
            outline_offset: CssPixels::from_raw(0),
            user_select: 1,
            object_position_x: ComputedStyleValueHandle::percentage(50.0),
            object_position_y: ComputedStyleValueHandle::percentage(50.0),
            view_transition_name: initial(property_id::VIEW_TRANSITION_NAME),
            scroll_behavior: 0,
            scroll_snap_align_block: crate::css::css_enums::scroll_snap_align::NONE,
            scroll_snap_align_inline: crate::css::css_enums::scroll_snap_align::NONE,
            scroll_snap_stop: crate::css::css_enums::scroll_snap_stop::NORMAL,
            scroll_snap_axis: crate::css::css_enums::scroll_snap_axis::BOTH,
            scroll_snap_strictness: crate::css::css_enums::scroll_snap_strictness::NONE,
            scrollbar_gutter: 0,
            scrollbar_width: 0,
            shape_image_threshold: 0.0,
            shape_margin: initial(property_id::SHAPE_MARGIN),
            shape_outside: initial(property_id::SHAPE_OUTSIDE),
            will_change: initial(property_id::WILL_CHANGE),
        }
    }
}

impl FontValues {
    fn initial() -> Self {
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id;

        let initial =
            |property| ComputedStyleValueHandle::retained(crate::css::style_compute::initial_value_data(property));
        Self {
            font_size: CssPixels::from_integer(16),
            line_height_used: CssPixels::from_raw(0),
            font_variant_emoji: 0,
            font_ascent: 0.0,
            font_descent: 0.0,
            font_x_height: 0.0,
            font_zero_advance: 0.0,
            first_available_font: std::ptr::null(),
            font_cascade_list: libgfx_rust::font::FontCascadeListHandle::null(),
            frozen_font_list: libgfx_rust::font::FrozenFontListRef::null(),
            font_weight: 400.0,
            font_width: 100.0,
            math_shift: 0,
            math_style: 0,
            math_depth: 0,
            font_family: initial(property_id::FONT_FAMILY),
            font_style: initial(property_id::FONT_STYLE),
            font_optical_sizing: initial(property_id::FONT_OPTICAL_SIZING),
            font_feature_settings: initial(property_id::FONT_FEATURE_SETTINGS),
            font_kerning: initial(property_id::FONT_KERNING),
            font_language_override: initial(property_id::FONT_LANGUAGE_OVERRIDE),
            font_variant_alternates: initial(property_id::FONT_VARIANT_ALTERNATES),
            font_variant_caps: initial(property_id::FONT_VARIANT_CAPS),
            font_variant_east_asian: initial(property_id::FONT_VARIANT_EAST_ASIAN),
            font_variant_ligatures: initial(property_id::FONT_VARIANT_LIGATURES),
            font_variant_numeric: initial(property_id::FONT_VARIANT_NUMERIC),
            font_variant_position: initial(property_id::FONT_VARIANT_POSITION),
            font_variation_settings: initial(property_id::FONT_VARIATION_SETTINGS),
            text_rendering: initial(property_id::TEXT_RENDERING),
            line_height: initial(property_id::LINE_HEIGHT),
            math_shift_value: initial(property_id::MATH_SHIFT),
            math_style_value: initial(property_id::MATH_STYLE),
            math_depth_value: initial(property_id::MATH_DEPTH),
            font_size_value: initial(property_id::FONT_SIZE),
        }
    }
}

/// Builds the complete alignment group from its computed property values.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// The value pointers must identify valid StyleValueData, and
/// `parent_payload` must identify an alignment payload or be null.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn rust_build_alignment_group(
    group_index: usize,
    webkit_box_orient: *const c_void,
    flex_direction: *const c_void,
    flex_wrap: *const c_void,
    flex_basis: *const c_void,
    flex_grow: f64,
    flex_shrink: f64,
    order: i32,
    align_content: *const c_void,
    align_items: *const c_void,
    align_self: *const c_void,
    justify_content: *const c_void,
    justify_items: *const c_void,
    justify_self: *const c_void,
    column_gap: *const c_void,
    row_gap: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    let build_or_reuse_group_payload = || {
        let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match unsafe { data.cast::<StyleValueData>().as_ref() } {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let built = AlignmentValues {
            webkit_box_orient: keyword_code(webkit_box_orient, crate::css::css_enums::keyword_to_webkit_box_orient)?,
            flex_direction: keyword_code(flex_direction, crate::css::css_enums::keyword_to_flex_direction)?,
            flex_wrap: keyword_code(flex_wrap, crate::css::css_enums::keyword_to_flex_wrap)?,
            flex_basis: ComputedFlexBasis::from_data(flex_basis),
            flex_grow,
            flex_shrink,
            order,
            align_content: keyword_code(align_content, crate::css::css_enums::keyword_to_align_content)?,
            align_items: keyword_code(align_items, crate::css::css_enums::keyword_to_align_items)?,
            align_self: keyword_code(align_self, crate::css::css_enums::keyword_to_align_self)?,
            justify_content: keyword_code(justify_content, crate::css::css_enums::keyword_to_justify_content)?,
            justify_items: keyword_code(justify_items, crate::css::css_enums::keyword_to_justify_items)?,
            justify_self: keyword_code(justify_self, crate::css::css_enums::keyword_to_justify_self)?,
            column_gap: ComputedGap::from_data(column_gap),
            row_gap: ComputedGap::from_data(row_gap),
        };

        if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<AlignmentValues>() }) {
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*default_payload.cast::<AlignmentValues>() }) {
            return Some(default_payload);
        }

        let payload = allocate_payload(style_group(group_index), 1);
        unsafe { payload.cast::<AlignmentValues>().write(built) };
        Some(payload.cast_const())
    };
    build_or_reuse_group_payload().unwrap_or(std::ptr::null())
}

/// Interns a complete non-inherited SVG geometry and painting group.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// `parent_payload` must identify an SVG reset payload or be null.
pub(crate) unsafe fn build_svg_reset_group_payload(
    group_index: usize,
    built: SVGResetValues,
    parent_payload: *const c_void,
) -> *const c_void {
    abort_on_panic(|| {
        if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<SVGResetValues>() }) {
            retain_group_payload(group_index, parent_payload);
            return parent_payload;
        }
        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*default_payload.cast::<SVGResetValues>() }) {
            return default_payload;
        }

        let payload = allocate_payload(style_group(group_index), 1);
        unsafe { payload.cast::<SVGResetValues>().write(built) };
        payload.cast_const()
    })
}

/// Builds the complete text-decoration reset group from its lowered values.
///
/// # Safety
/// `text_decoration_lines` must address `text_decoration_line_count` valid
/// enum codes, a non-null thickness must identify valid StyleValueData, and
/// `parent_payload` must identify a text reset payload or be null.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn rust_build_text_reset_group(
    group_index: usize,
    text_decoration_lines: *const u8,
    text_decoration_line_count: usize,
    text_decoration_thickness_kind: u8,
    text_decoration_thickness: *const c_void,
    text_decoration_style: u8,
    text_decoration_color: u32,
    white_space_trim_discard_before: bool,
    white_space_trim_discard_after: bool,
    white_space_trim_discard_inner: bool,
    parent_payload: *const c_void,
) -> *const c_void {
    let lines = if text_decoration_line_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(text_decoration_lines, text_decoration_line_count) }
    };
    let built = TextResetValues {
        text_decoration_lines: RetainedTextDecorationLineList::from_vec(lines.to_vec()),
        text_decoration_thickness_kind,
        text_decoration_thickness: if text_decoration_thickness_kind == 2 {
            ComputedStyleValueHandle::retained(text_decoration_thickness.cast())
        } else {
            ComputedStyleValueHandle::empty()
        },
        text_decoration_style,
        text_decoration_color,
        white_space_trim_discard_before,
        white_space_trim_discard_after,
        white_space_trim_discard_inner,
    };

    if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<TextResetValues>() }) {
        retain_group_payload(group_index, parent_payload);
        return parent_payload;
    }
    let default_payload = default_group_payload(group_index);
    if built.eq(unsafe { &*default_payload.cast::<TextResetValues>() }) {
        return default_payload;
    }

    let payload = allocate_payload(style_group(group_index), 1);
    unsafe { payload.cast::<TextResetValues>().write(built) };
    payload.cast_const()
}

/// Builds the complete surround group from the physical inset, margin, and
/// padding properties. Anchor insets retain their original value separately
/// while exposing auto through the length-box facade, matching layout's
/// existing representation.
///
/// # Safety
/// Each value pointer must address valid StyleValueData, and `parent_payload`
/// must be a valid surround payload or null.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn rust_build_surround_group(
    group_index: usize,
    top: *const c_void,
    right: *const c_void,
    bottom: *const c_void,
    left: *const c_void,
    margin_top: *const c_void,
    margin_right: *const c_void,
    margin_bottom: *const c_void,
    margin_left: *const c_void,
    padding_top: *const c_void,
    padding_right: *const c_void,
    padding_bottom: *const c_void,
    padding_left: *const c_void,
    position_anchor: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    let build_or_reuse_group_payload = || {
        let anchor = |data: *const c_void| {
            let data = data.cast::<StyleValueData>();
            if matches!(unsafe { data.as_ref() }, Some(StyleValueData::Anchor { .. })) {
                ComputedStyleValueHandle::retained(data)
            } else {
                ComputedStyleValueHandle::empty()
            }
        };
        let mut built = SurroundValues {
            inset: ComputedLengthBox::from_data(top, right, bottom, left, true),
            top_anchor_inset: anchor(top),
            right_anchor_inset: anchor(right),
            bottom_anchor_inset: anchor(bottom),
            left_anchor_inset: anchor(left),
            top_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            right_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            bottom_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            left_anchor_inset_wrapper: ComputedStyleValueHandle::empty(),
            position_anchor: if position_anchor.is_null() {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::retained(position_anchor.cast())
            },
            margin: ComputedLengthBox::from_data(margin_top, margin_right, margin_bottom, margin_left, false),
            padding: ComputedLengthBox::from_data(padding_top, padding_right, padding_bottom, padding_left, false),
        };

        if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<SurroundValues>() }) {
            retain_group_payload(group_index, parent_payload);
            return Some(parent_payload);
        }
        let default_payload = default_group_payload(group_index);
        if built.eq(unsafe { &*default_payload.cast::<SurroundValues>() }) {
            return Some(default_payload);
        }

        // Only a fresh payload builds wrappers: equality excludes them, and a
        // shared parent payload already carries its own.
        let wrapper = |handle: &ComputedStyleValueHandle| {
            if handle.pointer.is_null() {
                return ComputedStyleValueHandle::empty();
            }
            // SAFETY: The handle retains anchor style value data.
            let calculated = unsafe { crate::css::calc::create_anchor_inset_calculated(handle.pointer.cast()) };
            ComputedStyleValueHandle {
                pointer: std::sync::Arc::into_raw(calculated).cast(),
            }
        };
        built.top_anchor_inset_wrapper = wrapper(&built.top_anchor_inset);
        built.right_anchor_inset_wrapper = wrapper(&built.right_anchor_inset);
        built.bottom_anchor_inset_wrapper = wrapper(&built.bottom_anchor_inset);
        built.left_anchor_inset_wrapper = wrapper(&built.left_anchor_inset);

        let payload = allocate_payload(style_group(group_index), 1);
        unsafe { payload.cast::<SurroundValues>().write(built) };
        Some(payload.cast_const())
    };
    build_or_reuse_group_payload().unwrap_or(std::ptr::null())
}

/// Builds the box group from a fully materialized payload value. C++ resolves
/// every field directly into the payload layout (box type transformation,
/// ratio degeneracy handling and the vertical-align representation live
/// there), and ownership of the value's retained references transfers here in
/// every outcome.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, shared payloads are retained, and default payloads are
/// intentionally leaked and never counted.
///
/// # Safety
/// `values` must point at a fully initialized box payload whose ownership
/// transfers to this call, and `parent_payload` must be a valid box payload
/// or null.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn rust_build_box_group(
    group_index: usize,
    values: *const BoxValues,
    parent_payload: *const c_void,
) -> *const c_void {
    let built = unsafe { values.read() };

    if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<BoxValues>() }) {
        retain_group_payload(group_index, parent_payload);
        return parent_payload;
    }
    let default_payload = default_group_payload(group_index);
    if built.eq(unsafe { &*default_payload.cast::<BoxValues>() }) {
        return default_payload;
    }

    let payload = allocate_payload(style_group(group_index), 1);
    unsafe { payload.cast::<BoxValues>().write(built) };
    payload.cast_const()
}

/// # Safety
/// `values` must point at a fully initialized grid payload whose ownership
/// transfers to this call, and `parent_payload` must be a valid grid payload
/// or null.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn rust_build_grid_group(
    group_index: usize,
    values: *const GridValues,
    parent_payload: *const c_void,
) -> *const c_void {
    let built = unsafe { values.read() };

    if !parent_payload.is_null() && built.eq(unsafe { &*parent_payload.cast::<GridValues>() }) {
        retain_group_payload(group_index, parent_payload);
        return parent_payload;
    }
    let default_payload = default_group_payload(group_index);
    if built.eq(unsafe { &*default_payload.cast::<GridValues>() }) {
        return default_payload;
    }

    let payload = allocate_payload(style_group(group_index), 1);
    unsafe { payload.cast::<GridValues>().write(built) };
    payload.cast_const()
}

pub(crate) fn copy_grid_placements(source: &GridValues, target: &mut GridValues) {
    let mut remapped = |placement: ComputedGridPlacement| {
        let mut remap_index = |index: u32| {
            if index == GRID_NO_INDEX {
                return GRID_NO_INDEX;
            }
            target.intern_name(&source.names.as_slice()[index as usize])
        };
        ComputedGridPlacement {
            name_index: remap_index(placement.name_index),
            implicit_start_name_index: remap_index(placement.implicit_start_name_index),
            implicit_end_name_index: remap_index(placement.implicit_end_name_index),
            ..placement
        }
    };
    let column_start = remapped(source.column_start);
    let column_end = remapped(source.column_end);
    let row_start = remapped(source.row_start);
    let row_end = remapped(source.row_end);
    target.column_start = column_start;
    target.column_end = column_end;
    target.row_start = row_start;
    target.row_end = row_end;
    target.grid_column_start_style_value = source.grid_column_start_style_value.clone();
    target.grid_column_end_style_value = source.grid_column_end_style_value.clone();
    target.grid_row_start_style_value = source.grid_row_start_style_value.clone();
    target.grid_row_end_style_value = source.grid_row_end_style_value.clone();
}

/// Builds the complete sizing group from its six computed values. Accepted
/// sizing functions are already constrained to fit-content() by parsing, so
/// the function payload can be consumed without inspecting or copying its
/// interned name.
///
/// # Safety
/// Each value pointer must address valid StyleValueData, and `parent_payload`
/// must be a valid sizing payload or null.
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn build_sizing_group(
    group_index: usize,
    width: *const c_void,
    min_width: *const c_void,
    max_width: *const c_void,
    height: *const c_void,
    min_height: *const c_void,
    max_height: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    let built = SizingValues {
        width: ComputedSize::from_data(width),
        min_width: ComputedSize::from_data(min_width),
        max_width: ComputedSize::from_data(max_width),
        height: ComputedSize::from_data(height),
        min_height: ComputedSize::from_data(min_height),
        max_height: ComputedSize::from_data(max_height),
    };

    if !parent_payload.is_null() && built.eq(unsafe { &*(parent_payload as *const SizingValues) }) {
        retain_group_payload(group_index, parent_payload);
        return parent_payload;
    }

    let default_payload = default_group_payload(group_index);
    if built.eq(unsafe { &*(default_payload as *const SizingValues) }) {
        return default_payload;
    }

    let payload = allocate_payload(style_group(group_index), 1);
    unsafe { (payload as *mut SizingValues).write(built) };
    payload
}

/// Builds an inherited table group payload from the computed values, with the
/// same sharing rules as the inherited box builder. Computed border-spacing
/// is a pair of absolute pixel lengths.
///
/// # Safety
/// The value pointers must be valid StyleValueData or null, and
/// `parent_payload` a valid inherited table payload or null.
pub(crate) unsafe fn build_inherited_table_group(
    group_index: usize,
    border_collapse: *const c_void,
    caption_side: *const c_void,
    empty_cells: *const c_void,
    border_spacing: *const c_void,
    parent_payload: *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    let keyword_code = |data: *const c_void, map: fn(u16) -> Option<u8>| -> u8 {
        match unsafe { (data as *const StyleValueData).as_ref() } {
            Some(StyleValueData::Keyword { keyword }) => map(*keyword).expect("computed keyword has a supported value"),
            _ => panic!("computed inherited table value is not a keyword"),
        }
    };
    let spacing_component = |data: &StyleValueData| -> i32 {
        match data {
            StyleValueData::Length { value, unit } if *unit == crate::css::style_compute::px_length_unit() => {
                crate::css::css_pixels::CssPixels::nearest_value_for(*value).raw_value()
            }
            _ => panic!("computed border-spacing component is not an absolute pixel length"),
        }
    };
    let components = match unsafe { (border_spacing as *const StyleValueData).as_ref() } {
        Some(StyleValueData::ValueList { values, .. }) if values.as_slice().len() == 2 => values.as_slice(),
        _ => panic!("computed border-spacing is not a pair"),
    };
    let built = InheritedTableValues {
        border_collapse: keyword_code(border_collapse, crate::css::style_compute::keyword_to_border_collapse),
        caption_side: keyword_code(caption_side, crate::css::style_compute::keyword_to_caption_side),
        empty_cells: keyword_code(empty_cells, crate::css::style_compute::keyword_to_empty_cells),
        border_spacing_horizontal: spacing_component(components[0].data()),
        border_spacing_vertical: spacing_component(components[1].data()),
    };

    if !parent_payload.is_null() {
        // SAFETY: The caller guarantees a valid inherited table payload.
        if built == unsafe { *(parent_payload as *const InheritedTableValues) } {
            retain_group_payload(group_index, parent_payload);
            return parent_payload;
        }
    }

    let default_payload = default_group_payload(group_index);
    // SAFETY: The default payload is a valid inherited table payload.
    if built == unsafe { *(default_payload as *const InheritedTableValues) } {
        return default_payload;
    }

    let payload = allocate_payload(style_group(group_index), 1);
    // SAFETY: The payload was allocated for this group's layout.
    unsafe { *(payload as *mut InheritedTableValues) = built };
    payload as *const c_void
}

/// Layout of the inherited table style value group.
///
/// The enum fields follow the opaque-byte convention; the border spacings are
/// raw CSSPixels fixed-point values.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InheritedTableValues {
    pub border_collapse: u8,
    pub caption_side: u8,
    pub empty_cells: u8,
    pub border_spacing_horizontal: i32,
    pub border_spacing_vertical: i32,
}

impl InheritedTableValues {
    fn initial() -> Self {
        use crate::css::css_enums::{border_collapse, caption_side, empty_cells};

        Self {
            border_collapse: border_collapse::SEPARATE,
            caption_side: caption_side::TOP,
            empty_cells: empty_cells::SHOW,
            border_spacing_horizontal: 0,
            border_spacing_vertical: 0,
        }
    }
}

impl InheritedBoxValues {
    fn initial() -> Self {
        use crate::css::css_enums::{content_visibility, direction, image_rendering, visibility, writing_mode};

        Self {
            visibility: visibility::VISIBLE,
            direction: direction::LTR,
            writing_mode: writing_mode::HORIZONTAL_TB,
            content_visibility: content_visibility::VISIBLE,
            image_rendering: image_rendering::AUTO,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn field_descriptors_belong_to_the_group_properties_json_declares() {
        for group in 0..group_index::COUNT {
            for descriptor in group_field_descriptors(group) {
                let declared = crate::css::property_metadata::property_style_group_index(descriptor.property_id);
                assert_eq!(declared.map(usize::from), Some(group));
            }
        }
    }

    #[test]
    fn payload_lifecycle() {
        // The contract a payload's content hash owes its equality: two payloads that compare
        // equal hash equal, whoever built them and wherever they live. That is what lets a
        // group's identity be its content instead of the address of the first payload to
        // carry it.
        for group_index in 0..group_index::COUNT {
            let default = default_group_payload(group_index);
            // SAFETY: The default payload is a live payload of its group.
            let clone = unsafe { rust_style_group_clone(group_index, default) };
            assert!(style_group_payloads_equal(group_index, default, clone));
            assert_eq!(
                style_group_payloads_hash(group_index, default),
                style_group_payloads_hash(group_index, clone),
                "a clone must hash like the payload it was copied from"
            );
            refcount_of(clone, payload_align(style_group(group_index))).store(0, Ordering::Relaxed);
            // SAFETY: The clone holds the last reference and is a payload of this group.
            unsafe { rust_style_group_free(group_index, clone) };
        }

        // A field the hash skipped would be a difference equality has to separate every time,
        // so every field equality reads is a field the hash reads too.
        let mut changed = InheritedTableValues::initial();
        changed.border_spacing_horizontal = 17;
        let changed_payload = (&raw const changed).cast::<c_void>();
        let table_default = default_group_payload(group_index::INHERITED_TABLE);
        assert!(!style_group_payloads_equal(
            group_index::INHERITED_TABLE,
            table_default,
            changed_payload
        ));
        assert_ne!(
            style_group_payloads_hash(group_index::INHERITED_TABLE, table_default),
            style_group_payloads_hash(group_index::INHERITED_TABLE, changed_payload)
        );
    }
}

pub(crate) fn copy_fieldset_content_alignment(source: &AlignmentValues, target: &mut AlignmentValues) {
    target.flex_direction = source.flex_direction;
    target.flex_wrap = source.flex_wrap;
    target.align_content = source.align_content;
    target.align_items = source.align_items;
    target.justify_content = source.justify_content;
    target.justify_items = source.justify_items;
    target.row_gap = source.row_gap.clone();
    target.column_gap = source.column_gap.clone();
}
