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
//! usize of the header.

use crate::css::style_value::{ContentPayload, CounterDefinitionsPayload, FunctionPayload};
use crate::css::style_value_arc::StyleValueArc;
use std::alloc::{Layout, alloc, dealloc};
use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::abort_on_panic;
pub use crate::css::computed_value_types::{
    AlignmentValues, AnchorValues, AnimationValues, BackgroundValues, BorderValues, BoxValues, ComputedAspectRatio,
    ComputedClipEdge, ComputedColorOrAuto, ComputedContainIntrinsicSize, ComputedCursor, ComputedFilter,
    ComputedFilterOperation, ComputedFlexBasis, ComputedGap, ComputedGridArea, ComputedGridPlacement,
    ComputedGridPlacementKind, ComputedGridTrackBreadth, ComputedGridTrackEntry, ComputedGridTrackEntryKind,
    ComputedGridTrackList, ComputedLengthBox, ComputedLengthPercentageOrAuto, ComputedOverflowClipMargin,
    ComputedOverflowClipMarginSide, ComputedPositionTryFallback, ComputedResolvedTransform, ComputedSize,
    ComputedSizeKind, ComputedStyleValueHandle, ComputedSvgDash, ComputedTextIndent, ComputedTextUnderlineOffset,
    ComputedTextUnderlinePosition, ComputedVerticalAlign, ContentValues, EffectsValues, FontValues, GRID_NO_INDEX,
    GridValues, InheritedBits, InheritedListTableValues, InheritedSVGValues, InheritedSetValues, InheritedTextValues,
    InheritedUIValues, MaskValues, MiscInteractionValues, MiscLayoutValues, ResetBits, RetainedComputedCursorList,
    RetainedComputedFilterOperationList, RetainedComputedResolvedTransformList, RetainedComputedShadowList,
    RetainedComputedSvgDashList, RetainedGridAreaList, RetainedGridNameIndexList, RetainedGridTrackEntryList,
    RetainedPositionAreaList, RetainedPositionTryFallbackList, RetainedTextDecorationLineList,
    RetainedWillChangePropertyList, SVGResetValues, SizingValues, StyleBits, SurroundValues, TextResetValues,
    TransformValues,
};
use crate::css::computed_value_views::InlineLengthPercentage;
use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};
use crate::css::style_value::retained_list;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use crate::css::table_group_builder::group_index;

impl ComputedStyleValueHandle {
    pub(crate) fn empty() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    /// Another reference to the value `handle` holds, empty for an empty handle.
    pub(crate) fn from_retained(handle: &RetainedStyleValueData) -> Self {
        Self {
            pointer: handle.clone().into_pointer().cast(),
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
            inline_form: self.inline_form,
            inline_value: self.inline_value,
            value: self.value.clone(),
        }
    }
}

// NB: The inline length or percentage is derived from the value, so it takes no part in comparisons.
impl PartialEq for ComputedSize {
    fn eq(&self, other: &Self) -> bool {
        self.kind == other.kind && self.value == other.value
    }
}

impl Clone for ComputedLengthPercentageOrAuto {
    fn clone(&self) -> Self {
        Self {
            is_auto: self.is_auto,
            inline_form: self.inline_form,
            inline_value: self.inline_value,
            value: self.value.clone(),
        }
    }
}

impl PartialEq for ComputedLengthPercentageOrAuto {
    fn eq(&self, other: &Self) -> bool {
        self.is_auto == other.is_auto && self.value == other.value
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
    };
    // A style group whose payload holds values that layout does not read, each followed by `=> paint_only`, or values
    // that layout reads only in part, each followed by `=>` and the function that compares that part.
    ($type:ty { $($field:ident $(=> $layout_comparison:ident)?),+ $(,)? }) => {
        impl_computed_payload_clone_and_eq!($type { $($field),+ });

        impl StyleGroupLayoutInputs for $type {
            fn layout_inputs_equal(&self, other: &Self) -> bool {
                true $(&& layout_input_equal!(self.$field, other.$field $(, $layout_comparison)?))+
            }
        }
    };
}

macro_rules! layout_input_equal {
    ($first:expr, $second:expr) => {
        $first == $second
    };
    ($first:expr, $second:expr, paint_only) => {
        true
    };
    ($first:expr, $second:expr, $layout_inputs_equal:ident) => {
        $layout_inputs_equal(&$first, &$second)
    };
}

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
retained_list!(RetainedTextDecorationLineList, u8);
retained_list!(RetainedComputedFilterOperationList, ComputedFilterOperation);
retained_list!(RetainedWillChangePropertyList, u16);
retained_list!(
    RetainedComputedShadowList,
    crate::css::computed_value_types::ComputedShadow
);
retained_list!(RetainedComputedCursorList, ComputedCursor);
retained_list!(RetainedComputedSvgDashList, ComputedSvgDash);
impl_computed_payload_clone_and_eq!(InheritedTextValues {
    text_justify,
    line_break,
    tab_size_is_number,
    letter_spacing,
    word_spacing,
    tab_size_length,
    tab_size_number,
    text_indent,
    color => paint_only,
    color_style_value => paint_only,
    font_smoothing,
    webkit_text_fill_color => paint_only,
    webkit_text_fill_color_is_current_color => paint_only,
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
impl_computed_payload_clone_and_eq!(BorderValues {
    border_left => border_side_layout_inputs_equal,
    border_top => border_side_layout_inputs_equal,
    border_right => border_side_layout_inputs_equal,
    border_bottom => border_side_layout_inputs_equal,
    border_left_color_style_value => paint_only,
    border_top_color_style_value => paint_only,
    border_right_color_style_value => paint_only,
    border_bottom_color_style_value => paint_only,
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
    border_shape,
    border_image_slice,
    border_image_width,
    border_image_outset,
    border_image_repeat,
});
impl_computed_payload_clone_and_eq!(MiscLayoutValues {
    outline_offset_style_value,
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
    native_appearance_disabled,
    outline_style,
    object_fit,
    column_height,
    outline_color => paint_only,
    outline_width,
    column_rule_color => paint_only,
    column_rule_width,
    outline_offset,
    object_position_x,
    object_position_y,
    shape_image_threshold,
    shape_margin,
    shape_outside,
    content_visibility,
});
impl_computed_payload_clone_and_eq!(MiscInteractionValues {
    scroll_margin,
    scroll_padding,
    user_select,
    view_transition_name,
    scroll_behavior,
    scroll_snap_align_block,
    scroll_snap_align_inline,
    scroll_snap_stop,
    scroll_snap_axis,
    scroll_snap_strictness,
    scrollbar_gutter,
    scrollbar_width,
    overscroll_behavior_x,
    overscroll_behavior_y,
    will_change,
    implicit_will_change => implicit_will_change_layout_inputs_equal,
});
impl Clone for FontValues {
    fn clone(&self) -> Self {
        Self {
            font_size: self.font_size,
            resolved_line_height_used: self.resolved_line_height_used,
            font_variant_emoji: self.font_variant_emoji,
            resolved_font_ascent: self.resolved_font_ascent,
            resolved_font_descent: self.resolved_font_descent,
            resolved_font_x_height: self.resolved_font_x_height,
            resolved_font_zero_advance: self.resolved_font_zero_advance,
            resolved_first_available_font: self.resolved_first_available_font,
            font_cascade_list: self.font_cascade_list.clone(),
            frozen_font_list: self.frozen_font_list.clone(),
            font_weight: self.font_weight,
            font_width: self.font_width,
            math_shift: self.math_shift,
            math_style: self.math_style,
            math_depth: self.math_depth,
            font_family: self.font_family.clone(),
            font_style: self.font_style.clone(),
            font_optical_sizing: self.font_optical_sizing.clone(),
            font_synthesis_weight: self.font_synthesis_weight.clone(),
            font_synthesis_style: self.font_synthesis_style.clone(),
            font_feature_settings: self.font_feature_settings.clone(),
            font_kerning: self.font_kerning.clone(),
            font_language_override: self.font_language_override.clone(),
            font_variant_alternates: self.font_variant_alternates.clone(),
            font_variant_caps: self.font_variant_caps.clone(),
            font_variant_east_asian: self.font_variant_east_asian.clone(),
            font_variant_ligatures: self.font_variant_ligatures.clone(),
            font_variant_numeric: self.font_variant_numeric.clone(),
            font_variant_position: self.font_variant_position.clone(),
            font_variation_settings: self.font_variation_settings.clone(),
            text_rendering: self.text_rendering.clone(),
            line_height: self.line_height.clone(),
            math_shift_value: self.math_shift_value.clone(),
            math_style_value: self.math_style_value.clone(),
            math_depth_value: self.math_depth_value.clone(),
            font_size_value: self.font_size_value.clone(),
        }
    }
}

impl PartialEq for FontValues {
    fn eq(&self, other: &Self) -> bool {
        // A cascade the font computer published answers its first available font itself, which a load filling one of
        // its faces in moves past what the cascade was resolved with. Only a cascade nobody published answers that.
        let first_available_fonts_equal = (!self.frozen_font_list.is_null() && !other.frozen_font_list.is_null())
            || (self.resolved_font_ascent == other.resolved_font_ascent
                && self.resolved_font_descent == other.resolved_font_descent
                && self.resolved_font_x_height == other.resolved_font_x_height
                && self.resolved_font_zero_advance == other.resolved_font_zero_advance
                && self.resolved_first_available_font == other.resolved_first_available_font);
        first_available_fonts_equal
            && self.font_size == other.font_size
            && self.line_height_used() == other.line_height_used()
            && self.font_variant_emoji == other.font_variant_emoji
            && self.font_cascade_list == other.font_cascade_list
            && self.frozen_font_list == other.frozen_font_list
            && self.font_weight == other.font_weight
            && self.font_width == other.font_width
            && self.math_shift == other.math_shift
            && self.math_style == other.math_style
            && self.math_depth == other.math_depth
            && self.font_family == other.font_family
            && self.font_style == other.font_style
            && self.font_optical_sizing == other.font_optical_sizing
            && self.font_synthesis_weight == other.font_synthesis_weight
            && self.font_synthesis_style == other.font_synthesis_style
            && self.font_feature_settings == other.font_feature_settings
            && self.font_kerning == other.font_kerning
            && self.font_language_override == other.font_language_override
            && self.font_variant_alternates == other.font_variant_alternates
            && self.font_variant_caps == other.font_variant_caps
            && self.font_variant_east_asian == other.font_variant_east_asian
            && self.font_variant_ligatures == other.font_variant_ligatures
            && self.font_variant_numeric == other.font_variant_numeric
            && self.font_variant_position == other.font_variant_position
            && self.font_variation_settings == other.font_variation_settings
            && self.text_rendering == other.text_rendering
            && self.line_height == other.line_height
            && self.math_shift_value == other.math_shift_value
            && self.math_style_value == other.math_style_value
            && self.math_depth_value == other.math_depth_value
            && self.font_size_value == other.font_size_value
    }
}
retained_list!(RetainedPositionAreaList, u8);
retained_list!(RetainedPositionTryFallbackList, ComputedPositionTryFallback);
impl_computed_payload_clone_and_eq!(EffectsValues {
    opacity => paint_only,
    filter,
    backdrop_filter,
    mix_blend_mode,
    isolation,
    box_shadows,
    clip_is_rect,
    clip_edges,
    opacity_style_value => paint_only,
    filter_style_value,
    backdrop_filter_style_value,
    mix_blend_mode_style_value,
    isolation_style_value,
    box_shadow_style_value,
    clip_style_value,
});
impl_computed_payload_clone_and_eq!(InheritedUIValues {
    caret_color => paint_only,
    accent_color => paint_only,
    cursor,
    forced_color_adjust,
    interpolate_size,
    scrollbar_color => paint_only,
    color_scheme,
    color_schemes,
    color_scheme_only,
    image_rendering,
});

retained_list!(RetainedComputedResolvedTransformList, ComputedResolvedTransform);

retained_list!(RetainedGridTrackEntryList, ComputedGridTrackEntry);
retained_list!(RetainedGridNameIndexList, u32);
retained_list!(RetainedGridAreaList, ComputedGridArea);

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
            size: ComputedSize::keyword(ComputedSizeKind::Auto),
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
            names: RetainedUtf16FlyStringList::from_vec(Vec::new()),
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
            fit_tolerance_style_value: ComputedStyleValueHandle::empty(),
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
        self.names = RetainedUtf16FlyStringList::from_vec(names);
        index
    }
}

/// Expands `$body` once per style group, with `$payload` naming the payload type of the group at
/// the index `$group`.
macro_rules! with_style_group_type {
    ($group:expr, $payload:ident => $body:expr) => {
        with_style_group_type!(@match $group, $payload, $body,
            INHERITED_LIST_TABLE: InheritedListTableValues,
            INHERITED_UI: InheritedUIValues,
            INHERITED_SVG: InheritedSVGValues,
            INHERITED_TEXT: InheritedTextValues,
            FONT: FontValues,
            ANIMATION: AnimationValues,
            SVG_RESET: SVGResetValues,
            GRID: GridValues,
            ANCHOR: AnchorValues,
            CONTENT: ContentValues,
            ALIGNMENT: AlignmentValues,
            MISC_LAYOUT: MiscLayoutValues,
            MISC_INTERACTION: MiscInteractionValues,
            EFFECTS: EffectsValues,
            MASK: MaskValues,
            TEXT_RESET: TextResetValues,
            TRANSFORM: TransformValues,
            BACKGROUND: BackgroundValues,
            BORDER: BorderValues,
            SIZING: SizingValues,
            SURROUND: SurroundValues,
            BOX: BoxValues,
        )
    };
    (@match $group:expr, $payload:ident, $body:expr, $($index:ident: $values:ident,)+) => {
        match $group {
            $(group_index::$index => {
                type $payload = $values;
                $body
            })+
            _ => panic!("no style group has this index"),
        }
    };
}

fn payload_size(group: usize) -> usize {
    with_style_group_type!(group, Payload => size_of::<Payload>())
}

fn payload_align(group: usize) -> usize {
    with_style_group_type!(group, Payload => align_of::<Payload>())
}

/// Views a type-erased payload as its group's payload type.
///
/// # Safety
/// `payload` must point to a live payload of type `T` that stays live and unmodified for `'a`.
unsafe fn payload_ref<'a, T>(payload: *const c_void) -> &'a T {
    unsafe { &*payload.cast::<T>() }
}

/// The payload must point to uninitialized storage laid out for the group's payload type.
unsafe fn default_construct(group: usize, payload: *mut c_void) {
    with_style_group_type!(group, Payload => unsafe { payload.cast::<Payload>().write(Payload::initial()) });
}

/// The payload must point to uninitialized storage laid out for the group's payload type, and the
/// source to a live payload of that type.
#[allow(
    clippy::clone_on_copy,
    reason = "some style groups are Copy, and the macro expands for every group"
)]
unsafe fn copy_construct(group: usize, payload: *mut c_void, source: *const c_void) {
    with_style_group_type!(group, Payload => unsafe {
        payload.cast::<Payload>().write(payload_ref::<Payload>(source).clone());
    });
}

/// The payload must be a live payload of the group's type that nothing uses afterwards.
unsafe fn destruct(group: usize, payload: *mut c_void) {
    with_style_group_type!(group, Payload => unsafe { std::ptr::drop_in_place(payload.cast::<Payload>()) });
}

/// Both payloads must be live payloads of the group's type.
unsafe fn payloads_equal(group: usize, a: *const c_void, b: *const c_void) -> bool {
    with_style_group_type!(group, Payload => unsafe { payload_ref::<Payload>(a) == payload_ref::<Payload>(b) })
}

/// The values of a style group's payload that layout reads. A box whose style changes only the other values keeps the
/// layout it cached, as only painting reads them.
trait StyleGroupLayoutInputs: PartialEq {
    fn layout_inputs_equal(&self, other: &Self) -> bool {
        self == other
    }
}

fn border_side_layout_inputs_equal(
    first: &crate::css::computed_value_types::ComputedBorderSide,
    second: &crate::css::computed_value_types::ComputedBorderSide,
) -> bool {
    let crate::css::computed_value_types::ComputedBorderSide {
        color: _,
        line_style,
        width,
    } = first;
    *line_style == second.line_style && *width == second.width
}

fn implicit_will_change_layout_inputs_equal(
    first: &RetainedWillChangePropertyList,
    second: &RetainedWillChangePropertyList,
) -> bool {
    use crate::css::property_metadata::property_id::{COLOR, OPACITY};
    let names_only_paint_properties = |hints: &RetainedWillChangePropertyList| {
        hints
            .as_slice()
            .iter()
            .all(|&property| matches!(property, COLOR | OPACITY))
    };
    (names_only_paint_properties(first) && names_only_paint_properties(second)) || first == second
}

macro_rules! impl_style_group_read_whole_by_layout {
    ($($type:ty),+ $(,)?) => {
        $(impl StyleGroupLayoutInputs for $type {})+
    };
}

impl_style_group_read_whole_by_layout!(
    InheritedListTableValues,
    InheritedSVGValues,
    FontValues,
    AnimationValues,
    SVGResetValues,
    GridValues,
    AnchorValues,
    ContentValues,
    TransformValues,
    AlignmentValues,
    SizingValues,
    SurroundValues,
    BoxValues,
);

macro_rules! impl_style_group_unread_by_layout {
    ($($type:ty),+ $(,)?) => {
        $(impl StyleGroupLayoutInputs for $type {
            fn layout_inputs_equal(&self, _: &Self) -> bool {
                true
            }
        })+
    };
}

// Only painting reads these groups. Resource observers for background images register in apply_style, whatever layout
// does. Hit testing also reads the mask. Text decorations resolve when the display list is built. Layout does not read
// white-space-trim yet, so an implementation of it must remove TextResetValues from this list. EffectsValues is not
// in this list, because filter and backdrop-filter make a containing block for fixed-position descendants.
impl_style_group_unread_by_layout!(MaskValues, TextResetValues, BackgroundValues);

/// Both payloads must be live payloads of the group's type.
unsafe fn payload_layout_inputs_equal(group: usize, a: *const c_void, b: *const c_void) -> bool {
    with_style_group_type!(group, Payload => {
        // SAFETY: The caller guarantees live payloads of the group.
        let (a, b) = unsafe { (payload_ref::<Payload>(a), payload_ref::<Payload>(b)) };
        a.layout_inputs_equal(b)
    })
}

/// Whether two records' group payloads hold the same values that layout reads (see [`StyleGroupLayoutInputs`]), so a
/// box whose style moves from one record to the other keeps the layout it cached. The geometry of an SVG box can also
/// depend on `color`, through `currentColor`.
pub(crate) fn style_payloads_layout_inputs_equal(
    a: crate::css::computed_value_views::ComputedValuesView<'_>,
    b: crate::css::computed_value_views::ComputedValuesView<'_>,
    is_svg_box: bool,
) -> bool {
    let (a, b) = (&a.record_groups().0, &b.record_groups().0);
    if a.style_bits() != b.style_bits() {
        return false;
    }
    if is_svg_box {
        // SAFETY: Each style holds a live payload of each group.
        let (first, second) = unsafe {
            (
                payload_ref::<InheritedTextValues>(a[group_index::INHERITED_TEXT].as_ptr()),
                payload_ref::<InheritedTextValues>(b[group_index::INHERITED_TEXT].as_ptr()),
            )
        };
        if first.color != second.color {
            return false;
        }
    }
    (0..group_index::COUNT).all(|group| {
        let (a, b) = (a[group].as_ptr(), b[group].as_ptr());
        // SAFETY: Each style holds a live payload of each group.
        a == b || unsafe { payload_layout_inputs_equal(group, a, b) }
    })
}

/// https://drafts.csswg.org/css-anchor-position-1/#fallback-sensitive
pub(crate) fn style_payloads_position_fallback_inputs_equal(
    a: crate::css::computed_value_views::ComputedValuesView<'_>,
    b: crate::css::computed_value_views::ComputedValuesView<'_>,
) -> bool {
    if a.position() != b.position() {
        return false;
    }
    let (a, b) = (&a.record_groups().0, &b.record_groups().0);
    /// # Safety
    /// `T` must be the payload type of the group at `group`.
    unsafe fn payloads_of_group<'a, T>(a: &'a StyleGroups, b: &'a StyleGroups, group: usize) -> (&'a T, &'a T) {
        // SAFETY: Each style holds a live payload of each group, of the type the caller names.
        unsafe { (payload_ref::<T>(a[group].as_ptr()), payload_ref::<T>(b[group].as_ptr())) }
    }
    // SAFETY: Each group is read as its own payload type.
    let (surround_a, surround_b) = unsafe { payloads_of_group::<SurroundValues>(a, b, group_index::SURROUND) };
    // SAFETY: As above.
    let (sizing_a, sizing_b) = unsafe { payloads_of_group::<SizingValues>(a, b, group_index::SIZING) };
    // SAFETY: As above.
    let (alignment_a, alignment_b) = unsafe { payloads_of_group::<AlignmentValues>(a, b, group_index::ALIGNMENT) };
    // SAFETY: As above.
    let (anchor_a, anchor_b) = unsafe { payloads_of_group::<AnchorValues>(a, b, group_index::ANCHOR) };
    surround_a.inset == surround_b.inset
        && surround_a.top_anchor_inset == surround_b.top_anchor_inset
        && surround_a.right_anchor_inset == surround_b.right_anchor_inset
        && surround_a.bottom_anchor_inset == surround_b.bottom_anchor_inset
        && surround_a.left_anchor_inset == surround_b.left_anchor_inset
        && surround_a.position_anchor == surround_b.position_anchor
        && surround_a.margin == surround_b.margin
        && sizing_a == sizing_b
        && alignment_a.align_self == alignment_b.align_self
        && alignment_a.align_self_is_last_baseline == alignment_b.align_self_is_last_baseline
        && alignment_a.align_self_overflow == alignment_b.align_self_overflow
        && alignment_a.justify_self == alignment_b.justify_self
        && alignment_a.justify_self_overflow == alignment_b.justify_self_overflow
        && anchor_a.position_anchor_type == anchor_b.position_anchor_type
        && anchor_a.position_anchor_name == anchor_b.position_anchor_name
        && anchor_a.position_area == anchor_b.position_area
        && anchor_a.position_try_fallbacks == anchor_b.position_try_fallbacks
        && anchor_a.has_position_try_order == anchor_b.has_position_try_order
        && anchor_a.position_try_order == anchor_b.position_try_order
}

/// Whether `a` and `b` hold equal payloads in each group `compares` answers true for.
pub(crate) fn style_groups_equal_in(
    a: crate::css::computed_value_views::ComputedValuesView<'_>,
    b: crate::css::computed_value_views::ComputedValuesView<'_>,
    compares: impl Fn(usize) -> bool,
) -> bool {
    let (a, b) = (&a.record_groups().0, &b.record_groups().0);
    // NB: The inherited bits and zoom are beside the inherited groups, compared with them.
    (!compares(INHERITED_SET_GROUPS[0]) || a.inherited_values() == b.inherited_values())
        && (0..group_index::COUNT).all(|group| {
            let (a, b) = (a[group].as_ptr(), b[group].as_ptr());
            // SAFETY: Each style holds a live payload of each group.
            !compares(group) || a == b || unsafe { style_group_payloads_equal(group, a, b) }
        })
}

/// Whether two payloads of the group at `group_index` hold the same values.
///
/// # Safety
/// `a` and `b` must be live payloads of the group at `group_index`.
pub(crate) unsafe fn style_group_payloads_equal(group_index: usize, a: *const c_void, b: *const c_void) -> bool {
    assert!(!a.is_null());
    assert!(!b.is_null());
    // SAFETY: The caller guarantees live payloads of the group.
    unsafe { payloads_equal(group_index, a, b) }
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

/// The `<image>` values a style group's payload holds in the properties whose images a layout node
/// loads and observes. Only the groups holding such properties override the defaults.
trait StyleGroupImageValues {
    fn holds_image_values(&self) -> bool {
        false
    }

    /// Whether `self` and `new` hold different images, by value.
    fn image_values_differ(&self, _new: &Self) -> bool {
        false
    }
}

fn image_handles_differ(
    old: &ComputedStyleValueHandle,
    new: &ComputedStyleValueHandle,
    holds_image: fn(&ComputedStyleValueHandle) -> bool,
) -> bool {
    // NB: A computation may give an <image> a value of its own each time, which loads the same
    //     resources as an equal one.
    old.pointer != new.pointer && (holds_image(old) || holds_image(new)) && old.data() != new.data()
}

fn handle_holds_image(handle: &ComputedStyleValueHandle) -> bool {
    handle.data().is_some_and(StyleValueData::is_image)
}

impl StyleGroupImageValues for BackgroundValues {
    fn holds_image_values(&self) -> bool {
        layer_values_hold_image(&self.background_image)
    }

    fn image_values_differ(&self, new: &Self) -> bool {
        image_handles_differ(&self.background_image, &new.background_image, layer_values_hold_image)
    }
}

impl StyleGroupImageValues for MaskValues {
    fn holds_image_values(&self) -> bool {
        layer_values_hold_image(&self.mask_image)
    }

    fn image_values_differ(&self, new: &Self) -> bool {
        image_handles_differ(&self.mask_image, &new.mask_image, layer_values_hold_image)
    }
}

impl StyleGroupImageValues for BorderValues {
    fn holds_image_values(&self) -> bool {
        handle_holds_image(&self.border_image_source)
    }

    fn image_values_differ(&self, new: &Self) -> bool {
        image_handles_differ(&self.border_image_source, &new.border_image_source, handle_holds_image)
    }
}

impl StyleGroupImageValues for InheritedListTableValues {
    fn holds_image_values(&self) -> bool {
        handle_holds_image(&self.list_style_image)
    }

    fn image_values_differ(&self, new: &Self) -> bool {
        image_handles_differ(&self.list_style_image, &new.list_style_image, handle_holds_image)
    }
}

impl StyleGroupImageValues for InheritedUIValues {
    fn holds_image_values(&self) -> bool {
        self.cursor.as_slice().iter().any(|cursor| cursor.is_cursor_value)
    }

    fn image_values_differ(&self, new: &Self) -> bool {
        (self.cursor.pointer != new.cursor.pointer || self.cursor.length != new.cursor.length)
            && (self.holds_image_values() || new.holds_image_values())
    }
}

macro_rules! impl_style_group_without_image_values {
    ($($type:ty),+ $(,)?) => {
        $(impl StyleGroupImageValues for $type {})+
    };
}

impl_style_group_without_image_values!(
    SizingValues,
    AlignmentValues,
    SVGResetValues,
    SurroundValues,
    BoxValues,
    GridValues,
    TextResetValues,
    TransformValues,
    EffectsValues,
    AnchorValues,
    InheritedSVGValues,
    InheritedTextValues,
    AnimationValues,
    ContentValues,
    MiscLayoutValues,
    MiscInteractionValues,
    FontValues,
);

/// The payload must be a live payload of the group's type.
unsafe fn payload_holds_image_values(group: usize, payload: *const c_void) -> bool {
    with_style_group_type!(group, Payload => unsafe { payload_ref::<Payload>(payload) }.holds_image_values())
}

/// Whether any of a record's group payloads holds an `<image>` in a property whose images a layout
/// node loads and observes: the background and mask layers, the cursor, the border-image source and
/// the list-style image. Nearly every style holds none, and a record answers that with one flag
/// instead of materializing its layers to find out.
///
/// # Safety
/// Every non-null payload must be a live payload of the group at its index.
pub(crate) unsafe fn style_group_payloads_hold_image_values(payloads: &[*const c_void]) -> bool {
    payloads.iter().enumerate().any(|(group_index, &payload)| {
        // SAFETY: The caller guarantees live payloads of each group.
        !payload.is_null() && unsafe { payload_holds_image_values(group_index, payload) }
    })
}

/// Whether the group payloads `old` and `new`, of two records, hold different `<image>` values in the properties whose
/// images a layout node loads and observes, by value. Where they hold the same ones, a box that moves from one
/// record to the other keeps the images it loads and the facts it paints them with.
///
/// # Safety
/// Every non-null payload must be a live payload of the group at its index.
pub(crate) unsafe fn style_group_payloads_move_image_values(old: &[*const c_void], new: &[*const c_void]) -> bool {
    old.iter()
        .zip(new)
        .enumerate()
        .any(|(group_index, (&old_payload, &new_payload))| {
            if old_payload == new_payload {
                return false;
            }
            if old_payload.is_null() || new_payload.is_null() {
                // SAFETY: As in `style_group_payloads_hold_image_values`.
                return [old_payload, new_payload]
                    .into_iter()
                    .any(|payload| !payload.is_null() && unsafe { payload_holds_image_values(group_index, payload) });
            }
            // SAFETY: As in `style_group_payloads_hold_image_values`.
            unsafe { payload_image_values_differ(group_index, old_payload, new_payload) }
        })
}

/// Whether `old` and `new`, two live payloads of `group`, hold different images, by identity.
unsafe fn payload_image_values_differ(group: usize, old: *const c_void, new: *const c_void) -> bool {
    with_style_group_type!(group, Payload => unsafe {
        payload_ref::<Payload>(old).image_values_differ(payload_ref::<Payload>(new))
    })
}

/// Whether `payload`, a live payload of the group at `group_index`, holds the group's default
/// values.
///
/// # Safety
/// `payload` must be a live payload of the group.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_group_holds_default_values(group_index: usize, payload: *const c_void) -> bool {
    // SAFETY: The caller passes a live payload of the group.
    unsafe { GroupPayloadRef::new(group_index, payload) }.is_some_and(GroupPayloadRef::holds_the_defaults)
}

/// The bytes of the allocation of `payload`, a payload of the group at `group_index`. What the
/// payload holds beyond its own size is not counted.
pub(crate) fn group_payload_allocation_bytes(group_index: usize, payload: *const c_void) -> usize {
    if payload.is_null() {
        return 0;
    }
    allocation_layout(group_index).size()
}

/// Retains one reference to a payload, mirroring StyleStructRef::ref().
pub(crate) fn retain_group_payload(group_index: usize, payload: *const c_void) {
    refcount_of(payload, payload_align(group_index)).fetch_add(1, Ordering::Relaxed);
}

fn header_size(align: usize) -> usize {
    align.max(size_of::<usize>())
}

/// Where a group's computed values start in its allocation: after its payload.
fn values_offset(group: usize) -> usize {
    (header_size(payload_align(group)) + payload_size(group)).next_multiple_of(align_of::<RetainedStyleValueData>())
}

/// How many computed values the allocation of `group` holds.
fn value_count(group: usize) -> usize {
    usize::from(GROUP_VALUE_LAYOUT.counts[group])
}

fn allocation_layout(group: usize) -> Layout {
    let payload_align = payload_align(group);
    let align = payload_align
        .max(align_of::<usize>())
        .max(align_of::<RetainedStyleValueData>());
    Layout::from_size_align(
        values_offset(group) + value_count(group) * size_of::<RetainedStyleValueData>(),
        align,
    )
    .expect("style group layout overflow")
}

/// The computed values the allocation of `payload`, a payload of `group`, holds after it.
///
/// # Safety
/// `payload` must be a live payload of the group, allocated by `allocate_payload`.
unsafe fn values_of<'a>(group: usize, payload: *const c_void) -> &'a [RetainedStyleValueData] {
    // SAFETY: The allocation holds `value_count` initialized values at `values_offset`.
    unsafe {
        let start = (payload as *const u8)
            .sub(header_size(payload_align(group)))
            .add(values_offset(group))
            .cast::<RetainedStyleValueData>();
        ak::slice_from_raw_parts(start, value_count(group))
    }
}

/// The computed values of a payload nothing else references yet, to write.
///
/// # Safety
/// `payload` must be a live payload of the group, allocated by `allocate_payload`, that only the
/// caller references.
unsafe fn values_of_mut<'a>(group: usize, payload: *mut c_void) -> &'a mut [RetainedStyleValueData] {
    // SAFETY: As in `values_of`, and the caller holds the only reference.
    unsafe {
        let start = (payload as *mut u8)
            .sub(header_size(payload_align(group)))
            .add(values_offset(group))
            .cast::<RetainedStyleValueData>();
        ak::slice_from_raw_parts_mut(start, value_count(group))
    }
}

/// Destroys the payload and the values of an allocation nothing references, and frees it.
///
/// # Safety
/// `payload` must be a payload of the group, allocated by `allocate_payload`, with no remaining
/// references.
unsafe fn destroy_allocation(group: usize, payload: *mut c_void) {
    unsafe {
        std::ptr::drop_in_place(values_of_mut(group, payload));
        destruct(group, payload);
        let allocation = (payload as *mut u8).sub(header_size(payload_align(group)));
        dealloc(allocation, allocation_layout(group));
    }
}

fn refcount_of(payload: *const c_void, align: usize) -> &'static AtomicUsize {
    // SAFETY: Every payload pointer handed out by this module is preceded by
    // its header, whose first usize is the reference count.
    unsafe {
        let header = (payload as *const u8).sub(header_size(align)) as *const AtomicUsize;
        &*header
    }
}

fn allocate_payload(group: usize, initial_refcount: usize) -> *mut c_void {
    // SAFETY: The layout is never zero-sized (the header is at least a usize).
    unsafe {
        let allocation = alloc(allocation_layout(group));
        if allocation.is_null() {
            std::process::abort();
        }
        let header = allocation as *mut AtomicUsize;
        (*header).store(initial_refcount, Ordering::Relaxed);
        let values = allocation.add(values_offset(group)).cast::<RetainedStyleValueData>();
        for index in 0..value_count(group) {
            values.add(index).write(RetainedStyleValueData::none());
        }
        allocation.add(header_size(payload_align(group))) as *mut c_void
    }
}

/// A style group's payload type, which knows the index of its group.
pub(crate) trait StyleGroupPayload: 'static {
    const GROUP_INDEX: usize;
}

macro_rules! impl_style_group_payload {
    ($($type:ty => $index:ident),+ $(,)?) => {
        $(impl StyleGroupPayload for $type {
            const GROUP_INDEX: usize = group_index::$index;
        })+
    };
}

impl_style_group_payload!(
    InheritedListTableValues => INHERITED_LIST_TABLE,
    InheritedUIValues => INHERITED_UI,
    InheritedSVGValues => INHERITED_SVG,
    InheritedTextValues => INHERITED_TEXT,
    FontValues => FONT,
    AnimationValues => ANIMATION,
    SVGResetValues => SVG_RESET,
    GridValues => GRID,
    AnchorValues => ANCHOR,
    EffectsValues => EFFECTS,
    MaskValues => MASK,
    TextResetValues => TEXT_RESET,
    ContentValues => CONTENT,
    TransformValues => TRANSFORM,
    BackgroundValues => BACKGROUND,
    BorderValues => BORDER,
    AlignmentValues => ALIGNMENT,
    MiscLayoutValues => MISC_LAYOUT,
    MiscInteractionValues => MISC_INTERACTION,
    SizingValues => SIZING,
    SurroundValues => SURROUND,
    BoxValues => BOX,
);

/// Verifies that `T` is the payload type of `group_index`, so typed payload views of the group's
/// default and freshly allocated payloads are sound.
fn assert_group_payload_type<T: StyleGroupPayload>(group_index: usize) {
    assert_eq!(group_index, T::GROUP_INDEX, "style group payload type mismatch");
}

/// The index of a computed-value style group, in the order both sides use (`group_index`,
/// LIBWEB_ENUMERATE_COMPUTED_VALUE_STYLE_GROUPS).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
#[repr(transparent)]
pub(crate) struct StyleGroupIndex(u8);

impl StyleGroupIndex {
    pub(crate) const fn new(index: usize) -> Self {
        assert!(index < group_index::COUNT);
        Self(index as u8)
    }

    pub(crate) const fn usize(self) -> usize {
        self.0 as usize
    }

    /// Every group's index, in group order.
    pub(crate) fn all() -> impl Iterator<Item = Self> {
        (0..group_index::COUNT).map(Self::new)
    }
}

/// A pointer to a live style group, which a [`StyleGroups`] holds a reference to.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub(crate) struct GroupPointer(std::ptr::NonNull<c_void>);

impl GroupPointer {
    pub(crate) fn as_ptr(self) -> *const c_void {
        self.0.as_ptr().cast_const()
    }

    /// The group, as its payload type `T`, borrowed.
    pub(crate) fn cast<T>(self) -> crate::css::host_shared::HostShared<T> {
        crate::css::host_shared::HostShared::new(self.as_ptr().cast())
    }
}

/// Where a style keeps one of its groups: in its root, or in one of the group sets it may share
/// with other styles.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum GroupPlace {
    Root(u8),
    Inherited(u8),
    Rare(u8),
    Visual(u8),
}

/// The groups a style keeps in its root, which elements styled apart differ in most often.
pub(crate) const ROOT_GROUPS: [usize; 3] = [group_index::SIZING, group_index::SURROUND, group_index::BOX];

/// The groups of the properties that inherit, which a child writing none of them holds as its
/// parent's set.
pub(crate) const INHERITED_SET_GROUPS: [usize; 5] = [
    group_index::INHERITED_LIST_TABLE,
    group_index::INHERITED_UI,
    group_index::INHERITED_SVG,
    group_index::INHERITED_TEXT,
    group_index::FONT,
];

/// The groups of the properties few elements write.
pub(crate) const RARE_SET_GROUPS: [usize; 8] = [
    group_index::ANIMATION,
    group_index::SVG_RESET,
    group_index::GRID,
    group_index::ANCHOR,
    group_index::CONTENT,
    group_index::ALIGNMENT,
    group_index::MISC_LAYOUT,
    group_index::MISC_INTERACTION,
];

/// The groups of the properties that paint a box.
pub(crate) const VISUAL_SET_GROUPS: [usize; 6] = [
    group_index::EFFECTS,
    group_index::MASK,
    group_index::TEXT_RESET,
    group_index::TRANSFORM,
    group_index::BACKGROUND,
    group_index::BORDER,
];

// NB: Each place is a run of group indices, so that the host finds a group's place from its index
//     alone: the inherited set, then the rare set, the visual set and the root.
const _: () = {
    let runs: [&[usize]; 4] = [
        &INHERITED_SET_GROUPS,
        &RARE_SET_GROUPS,
        &VISUAL_SET_GROUPS,
        &ROOT_GROUPS,
    ];
    let mut next = 0;
    let mut run = 0;
    while run < runs.len() {
        let mut slot = 0;
        while slot < runs[run].len() {
            assert!(runs[run][slot] == next);
            next += 1;
            slot += 1;
        }
        run += 1;
    }
    assert!(next == group_index::COUNT);
    assert!(crate::css::computed_value_types::STYLE_GROUP_RARE_SET_START == RARE_SET_GROUPS[0]);
    assert!(crate::css::computed_value_types::STYLE_GROUP_VISUAL_SET_START == VISUAL_SET_GROUPS[0]);
    assert!(crate::css::computed_value_types::STYLE_GROUP_ROOT_START == ROOT_GROUPS[0]);
};

/// The place of each group, by group index.
pub(crate) const GROUP_PLACES: [GroupPlace; group_index::COUNT] = {
    let mut places = [GroupPlace::Root(u8::MAX); group_index::COUNT];
    let mut slot = 0;
    while slot < ROOT_GROUPS.len() {
        places[ROOT_GROUPS[slot]] = GroupPlace::Root(slot as u8);
        slot += 1;
    }
    let mut slot = 0;
    while slot < INHERITED_SET_GROUPS.len() {
        places[INHERITED_SET_GROUPS[slot]] = GroupPlace::Inherited(slot as u8);
        slot += 1;
    }
    let mut slot = 0;
    while slot < RARE_SET_GROUPS.len() {
        places[RARE_SET_GROUPS[slot]] = GroupPlace::Rare(slot as u8);
        slot += 1;
    }
    let mut slot = 0;
    while slot < VISUAL_SET_GROUPS.len() {
        places[VISUAL_SET_GROUPS[slot]] = GroupPlace::Visual(slot as u8);
        slot += 1;
    }
    let mut group = 0;
    while group < group_index::COUNT {
        assert!(
            !matches!(places[group], GroupPlace::Root(u8::MAX)),
            "every group has a place"
        );
        group += 1;
    }
    places
};

macro_rules! group_sets {
    ($($(#[$meta:meta])* $set:ident => $groups:ident, $(#[$beside_meta:meta])* beside: $beside:ty,)+) => {$(
        $(#[$meta])*
        #[repr(C)]
        pub(crate) struct $set {
            // NB: First, so that the host reads the set's address as the address of its groups.
            groups: [GroupPointer; $groups.len()],
            $(#[$beside_meta])*
            beside: $beside,
        }

        impl $set {
            /// Takes over one reference to each group of the set, out of a style's groups in group order.
            fn adopt(pointers: &[GroupPointer; group_index::COUNT]) -> Self {
                Self {
                    groups: std::array::from_fn(|slot| pointers[$groups[slot]]),
                    beside: Default::default(),
                }
            }

            /// Whether `pointers` holds this set's groups.
            fn holds(&self, pointers: &[GroupPointer; group_index::COUNT]) -> bool {
                self.groups.iter().zip($groups).all(|(pointer, group)| *pointer == pointers[group])
            }
        }

        impl Clone for $set {
            fn clone(&self) -> Self {
                for (pointer, group) in self.groups.iter().zip($groups) {
                    retain_group_payload(group, pointer.as_ptr());
                }
                Self {
                    groups: self.groups,
                    beside: self.beside.clone(),
                }
            }
        }

        impl Drop for $set {
            fn drop(&mut self) {
                for (pointer, group) in self.groups.iter().zip($groups) {
                    release_group_payload(group, pointer.as_ptr());
                }
            }
        }
    )+};
}

/// What a style's inherited set holds beside its groups.
#[derive(Clone)]
#[repr(C)]
pub(crate) struct InheritedSetBeside {
    /// NB: First, so that the host reads them right after the set's groups.
    values: InheritedSetValues,
    /// The custom properties the element holds, which its children holding the same share.
    custom_properties: Option<std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>,
}

impl Default for InheritedSetBeside {
    fn default() -> Self {
        Self {
            values: InheritedSetValues::INITIAL,
            custom_properties: None,
        }
    }
}

group_sets! {
    /// The inherited groups of a style, which its children writing none of them hold too.
    InheritedSet => INHERITED_SET_GROUPS,
    /// The inherited longhands held as bits, the effective zoom and the custom properties, which
    /// the children of an element holding the same share.
    beside: InheritedSetBeside,
    /// The groups of a style few elements write.
    RareSet => RARE_SET_GROUPS,
    /// What the computation of a record holding the set recorded beside its values, and the
    /// element's own custom declarations.
    beside: crate::css::style::pipeline::RareSetFacts,
    /// The groups of a style painting a box, and its other reset groups.
    VisualSet => VISUAL_SET_GROUPS,
    beside: (),
}

/// One counted reference to a group set, which styles holding the same groups there share.
#[repr(transparent)]
pub(crate) struct SharedGroupSet<T>(std::ptr::NonNull<T>);

impl<T> SharedGroupSet<T> {
    fn new(set: T) -> Self {
        // SAFETY: `Arc::into_raw` never answers null.
        Self(unsafe { std::ptr::NonNull::new_unchecked(std::sync::Arc::into_raw(std::sync::Arc::new(set)).cast_mut()) })
    }

    fn get(&self) -> &T {
        // SAFETY: The reference keeps the set alive.
        unsafe { self.0.as_ref() }
    }

    fn ptr_eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }

    /// The set, to write, copied first where another style shares it.
    fn make_mut(&mut self) -> &mut T
    where
        T: Clone,
    {
        // SAFETY: The pointer came from `Arc::into_raw`, and this handle owns one reference.
        let mut arc = unsafe { std::sync::Arc::from_raw(self.0.as_ptr().cast_const()) };
        std::sync::Arc::make_mut(&mut arc);
        // SAFETY: As above.
        self.0 = unsafe { std::ptr::NonNull::new_unchecked(std::sync::Arc::into_raw(arc).cast_mut()) };
        // SAFETY: `make_mut` left this handle the set's only reference.
        unsafe { self.0.as_mut() }
    }
}

impl<T> Clone for SharedGroupSet<T> {
    fn clone(&self) -> Self {
        // SAFETY: The pointer came from `Arc::into_raw`, and this handle keeps it alive.
        unsafe { std::sync::Arc::increment_strong_count(self.0.as_ptr().cast_const()) };
        Self(self.0)
    }
}

impl<T> Drop for SharedGroupSet<T> {
    fn drop(&mut self) {
        // SAFETY: The pointer came from `Arc::into_raw`, and this handle owns one reference.
        unsafe { std::sync::Arc::decrement_strong_count(self.0.as_ptr().cast_const()) };
    }
}

/// One counted reference to the rare set of a style, which the next style recording the same rare
/// facts and holding the same rare groups shares.
#[derive(Clone)]
#[repr(transparent)]
pub(crate) struct RareSetRef(SharedGroupSet<RareSet>);

impl RareSetRef {
    /// The rare facts the set holds beside its groups.
    pub(crate) fn facts(&self) -> &std::sync::Arc<crate::css::style::pipeline::RareStyleFacts> {
        self.0
            .get()
            .beside
            .facts
            .as_ref()
            .expect("a kept rare set holds its facts")
    }

    /// The custom declarations the set holds beside its groups.
    pub(crate) fn custom_declarations(&self) -> Option<&crate::css::style::pipeline::CustomDeclarations> {
        self.0.get().beside.custom_declarations.as_ref()
    }

    pub(crate) fn ptr_eq(&self, other: &Self) -> bool {
        self.0.ptr_eq(&other.0)
    }
}

/// One counted reference to each group of an element's style: the groups elements styled apart
/// differ in most often in the root, and the others in sets the style shares with the styles that
/// hold the same groups there, such as its parent's inherited groups.
#[repr(C)]
pub(crate) struct StyleGroups {
    root: [GroupPointer; ROOT_GROUPS.len()],
    inherited: SharedGroupSet<InheritedSet>,
    rare: SharedGroupSet<RareSet>,
    visual: SharedGroupSet<VisualSet>,
    /// The properties that do not inherit held as bits, after the set pointers.
    reset: ResetBits,
    /// The element's color, resolved, which `currentcolor` in its groups resolves to.
    current_color: u32,
}

// NB: The host reads groups through this layout (`CSS::style_record_group`): the root's groups,
//     then a pointer to the first group of each set.
const _: () = assert!(size_of::<StyleGroups>() == 56);
const _: () = assert!(std::mem::offset_of!(StyleGroups, reset) == 48);
const _: () = assert!(std::mem::offset_of!(StyleGroups, current_color) == 52);
const _: () = assert!(std::mem::offset_of!(StyleGroups, root) == 0);
const _: () = assert!(std::mem::offset_of!(StyleGroups, inherited) == 24);
const _: () = assert!(std::mem::offset_of!(StyleGroups, rare) == 32);
const _: () = assert!(std::mem::offset_of!(StyleGroups, visual) == 40);

// SAFETY: A group is immutable once published, and its reference count is atomic, so the
//         references may move to and be read from any thread, as `HeldGroup`'s groups are. A set
//         is immutable while shared and its count is an `Arc`'s.
unsafe impl Send for StyleGroups {}
unsafe impl Sync for StyleGroups {}

/// The groups of a style's group pointers that hold no reference of their own: each one of a set
/// another style holds, which the style is to share, as an element sharing its parent's inherited
/// set takes those groups without counting a reference to each.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub(crate) struct BorrowedGroups(u32);

impl BorrowedGroups {
    pub(crate) const NONE: Self = Self(0);

    /// Adds `group` where it is a group of a set, answering whether it is.
    pub(crate) fn note_if_of_a_set(&mut self, group: usize) -> bool {
        const SET_GROUPS: u32 = {
            let mut groups = (1_u32 << group_index::COUNT) - 1;
            let mut slot = 0;
            while slot < ROOT_GROUPS.len() {
                groups &= !(1 << ROOT_GROUPS[slot]);
                slot += 1;
            }
            groups
        };
        let is_of_a_set = SET_GROUPS & (1 << group) != 0;
        if is_of_a_set {
            self.0 |= 1 << group;
        }
        is_of_a_set
    }

    pub(crate) fn contains(self, group: usize) -> bool {
        self.0 & (1 << group) != 0
    }
}

/// The groups one computation of a style made, holding a reference to each but the borrowed ones
/// until a style adopts them: a computation whose result is thrown away, as one computed again,
/// releases them as it drops.
pub(crate) struct ComputedGroups {
    pointers: [*const c_void; group_index::COUNT],
    bits: StyleBits,
    current_color: u32,
    borrowed: BorrowedGroups,
}

impl ComputedGroups {
    /// Takes over one reference to each of `pointers` but the `borrowed` ones, with `inherited`
    /// beside the inherited groups.
    ///
    /// # Safety
    /// Each of `pointers` must be a live group of its index, with a reference the caller hands over
    /// unless `borrowed` names it, and a borrowed one must stay live while these groups do.
    pub(crate) unsafe fn new(
        pointers: [*const c_void; group_index::COUNT],
        bits: StyleBits,
        current_color: u32,
        borrowed: BorrowedGroups,
    ) -> Self {
        Self {
            pointers,
            bits,
            current_color,
            borrowed,
        }
    }

    /// What the groups hold as bits beside them.
    pub(crate) fn style_bits(&self) -> StyleBits {
        self.bits
    }

    /// The groups, one live group of each index.
    pub(crate) fn pointers(&self) -> &[*const c_void; group_index::COUNT] {
        &self.pointers
    }

    /// The computed value of `property` these groups hold.
    pub(crate) fn value(&self, property: u16) -> &RetainedStyleValueData {
        // SAFETY: These groups hold a reference to each of their groups, or borrow it from a set
        //         that outlives them.
        unsafe { value_in_groups_of(|group| self.pointers[group], &self.bits, property) }
    }

    /// The groups as a style's, sharing the sets of `shares` that hold the same groups.
    pub(crate) fn adopt_sharing(self, shares: &[Option<&StyleGroups>]) -> StyleGroups {
        let this = std::mem::ManuallyDrop::new(self);
        // SAFETY: The groups hold the references `new` took over, which the style takes.
        unsafe { StyleGroups::adopt_sharing(this.pointers, this.bits, this.borrowed, shares) }
            .with_current_color(this.current_color)
    }
}

impl Drop for ComputedGroups {
    fn drop(&mut self) {
        for (group, &pointer) in self.pointers.iter().enumerate() {
            if !self.borrowed.contains(group) {
                release_group_payload(group, pointer);
            }
        }
    }
}

impl StyleGroups {
    /// Takes over one reference to each of `pointers` but the `borrowed` ones, with `bits` beside
    /// them, sharing the sets of `shares` that hold the same.
    ///
    /// # Safety
    /// Each of `pointers` must be a live group of its index, with a reference the caller hands over
    /// unless `borrowed` names it, and a borrowed one must stay live for this call.
    pub(crate) unsafe fn adopt_sharing(
        pointers: [*const c_void; group_index::COUNT],
        bits: StyleBits,
        borrowed: BorrowedGroups,
        shares: &[Option<&StyleGroups>],
    ) -> Self {
        assert!(
            ROOT_GROUPS.iter().all(|&group| !borrowed.contains(group)),
            "a root group is never borrowed"
        );
        let pointers = pointers.map(|pointer| {
            GroupPointer(std::ptr::NonNull::new(pointer.cast_mut()).expect("a style group is never null"))
        });
        fn set<T: Clone>(
            pointers: &[GroupPointer; group_index::COUNT],
            borrowed: BorrowedGroups,
            shares: &[Option<&StyleGroups>],
            of: impl Fn(&StyleGroups) -> &SharedGroupSet<T>,
            holds: impl Fn(&T, &[GroupPointer; group_index::COUNT]) -> bool,
            adopt: impl Fn(&[GroupPointer; group_index::COUNT]) -> T,
            groups: &[usize],
        ) -> SharedGroupSet<T> {
            match shares
                .iter()
                .flatten()
                .map(|share| of(share))
                .find(|set| holds(set.get(), pointers))
            {
                Some(shared) => {
                    // The shared set holds references of its own.
                    for &group in groups.iter().filter(|&&group| !borrowed.contains(group)) {
                        release_group_payload(group, pointers[group].as_ptr());
                    }
                    shared.clone()
                }
                None => {
                    // A set of its own counts a reference to each of its groups.
                    for &group in groups.iter().filter(|&&group| borrowed.contains(group)) {
                        retain_group_payload(group, pointers[group].as_ptr());
                    }
                    SharedGroupSet::new(adopt(pointers))
                }
            }
        }
        Self {
            root: std::array::from_fn(|slot| pointers[ROOT_GROUPS[slot]]),
            inherited: set(
                &pointers,
                borrowed,
                shares,
                |groups| &groups.inherited,
                |set, pointers| set.holds(pointers) && set.beside.values == bits.inherited,
                |pointers| {
                    let mut set = InheritedSet::adopt(pointers);
                    set.beside.values = bits.inherited;
                    set
                },
                &INHERITED_SET_GROUPS,
            ),
            rare: set(
                &pointers,
                borrowed,
                shares,
                |groups| &groups.rare,
                RareSet::holds,
                RareSet::adopt,
                &RARE_SET_GROUPS,
            ),
            visual: set(
                &pointers,
                borrowed,
                shares,
                |groups| &groups.visual,
                VisualSet::holds,
                VisualSet::adopt,
                &VISUAL_SET_GROUPS,
            ),
            reset: bits.reset,
            current_color: 0,
        }
    }

    /// Takes over one reference to each of `pointers`, with `inherited` beside the inherited
    /// groups.
    ///
    /// # Safety
    /// Each of `pointers` must be a live group of its index, with a reference the caller hands over.
    pub(crate) unsafe fn adopt(pointers: [*const c_void; group_index::COUNT], bits: StyleBits) -> Self {
        // SAFETY: As the caller guarantees.
        unsafe { Self::adopt_sharing(pointers, bits, BorrowedGroups::NONE, &[]) }
    }

    /// Takes one more reference to each of `pointers`, with `inherited` beside the inherited
    /// groups.
    ///
    /// # Safety
    /// `pointers` must hold one live group of each index, in group order.
    pub(crate) unsafe fn retained(pointers: &[*const c_void], bits: StyleBits) -> Self {
        // SAFETY: As the caller guarantees.
        unsafe { Self::retained_sharing(pointers, bits, &[]) }
    }

    /// Takes one more reference to each of `pointers`, with `inherited` beside the inherited
    /// groups, sharing the sets of `shares` that hold the same.
    ///
    /// # Safety
    /// `pointers` must hold one live group of each index, in group order.
    pub(crate) unsafe fn retained_sharing(
        pointers: &[*const c_void],
        bits: StyleBits,
        shares: &[Option<&StyleGroups>],
    ) -> Self {
        let pointers: &[*const c_void; group_index::COUNT] =
            pointers.try_into().expect("an element's style has every group");
        for (group, pointer) in pointers.iter().enumerate() {
            retain_group_payload(group, *pointer);
        }
        // SAFETY: Each pointer is live, with the reference just taken.
        unsafe { Self::adopt_sharing(*pointers, bits, BorrowedGroups::NONE, shares) }
    }

    /// The address and allocation size of each group set the style holds, which a memory report
    /// counts once however many styles share it.
    pub(crate) fn set_allocations(&self) -> [(*const c_void, usize); 3] {
        fn allocation<T>(set: &SharedGroupSet<T>) -> (*const c_void, usize) {
            (
                set.0.as_ptr().cast_const().cast(),
                crate::heap_size::ARC_HEADER_BYTES + size_of::<T>(),
            )
        }
        [
            allocation(&self.inherited),
            allocation(&self.rare),
            allocation(&self.visual),
        ]
    }

    /// The computed value of `property` these groups hold.
    pub(crate) fn value(&self, property: u16) -> &RetainedStyleValueData {
        // SAFETY: The style holds a reference to each of its groups.
        unsafe { value_in_groups_of(|group| self[group].as_ptr(), &self.style_bits(), property) }
    }

    /// The groups, in group order.
    pub(crate) fn pointers(&self) -> [*const c_void; group_index::COUNT] {
        let mut pointers = [std::ptr::null(); group_index::COUNT];
        for (&group, pointer) in ROOT_GROUPS.iter().zip(&self.root) {
            pointers[group] = pointer.as_ptr();
        }
        for (&group, pointer) in INHERITED_SET_GROUPS.iter().zip(&self.inherited.get().groups) {
            pointers[group] = pointer.as_ptr();
        }
        for (&group, pointer) in RARE_SET_GROUPS.iter().zip(&self.rare.get().groups) {
            pointers[group] = pointer.as_ptr();
        }
        for (&group, pointer) in VISUAL_SET_GROUPS.iter().zip(&self.visual.get().groups) {
            pointers[group] = pointer.as_ptr();
        }
        pointers
    }

    /// The custom properties the element holds.
    pub(crate) fn custom_properties(
        &self,
    ) -> Option<&std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>> {
        self.inherited.get().beside.custom_properties.as_ref()
    }

    /// The inherited longhands these groups hold as bits, and the effective zoom.
    #[inline]
    pub(crate) fn inherited_values(&self) -> InheritedSetValues {
        self.inherited.get().beside.values
    }

    /// The inherited longhands these groups hold as bits.
    #[inline]
    pub(crate) fn inherited_bits(&self) -> InheritedBits {
        self.inherited.get().beside.values.bits
    }

    /// The properties that do not inherit these groups hold as bits.
    #[inline]
    pub(crate) fn reset_bits(&self) -> ResetBits {
        self.reset
    }

    /// What these groups hold as bits beside them.
    #[inline]
    pub(crate) fn style_bits(&self) -> StyleBits {
        StyleBits {
            inherited: self.inherited_values(),
            reset: self.reset,
        }
    }

    /// These groups, with `bits` beside them: in a copy of the inherited set where it holds other
    /// inherited bits.
    pub(crate) fn with_style_bits(mut self, bits: StyleBits) -> Self {
        self.set_inherited_values(bits.inherited);
        self.reset = bits.reset;
        self
    }

    /// Puts `reset` in place of the reset bits.
    pub(crate) fn set_reset_bits(&mut self, reset: ResetBits) {
        self.reset = reset;
    }

    /// The element's color, resolved, which `currentcolor` in its groups resolves to.
    #[inline]
    pub(crate) fn current_color(&self) -> u32 {
        self.current_color
    }

    /// These groups, with `current_color` as the element's resolved color.
    pub(crate) fn with_current_color(mut self, current_color: u32) -> Self {
        self.current_color = current_color;
        self
    }

    /// Puts `values` beside the inherited groups, in a copy of the inherited set where it holds
    /// others.
    pub(crate) fn set_inherited_values(&mut self, values: InheritedSetValues) {
        if self.inherited.get().beside.values != values {
            self.inherited.make_mut().beside.values = values;
        }
    }

    /// These groups, holding `store` as the element's custom properties: in the inherited set they
    /// share where it holds `store` already, otherwise in a copy of it.
    pub(crate) fn with_custom_properties(
        mut self,
        store: Option<&std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>>,
    ) -> Self {
        let holds_store = match (&self.inherited.get().beside.custom_properties, store) {
            (None, None) => true,
            (Some(held), Some(store)) => std::sync::Arc::ptr_eq(held, store),
            _ => false,
        };
        if !holds_store {
            self.inherited.make_mut().beside.custom_properties = store.cloned();
        }
        self
    }

    /// What the computation of the record holding these groups recorded beside its values.
    pub(crate) fn rare_facts(&self) -> Option<&std::sync::Arc<crate::css::style::pipeline::RareStyleFacts>> {
        self.rare.get().beside.facts.as_ref()
    }

    /// The custom declarations the cascade of the element holding these groups decided.
    pub(crate) fn custom_declarations(&self) -> Option<&crate::css::style::pipeline::CustomDeclarations> {
        self.rare.get().beside.custom_declarations.as_ref()
    }

    /// The rare set of these groups.
    pub(crate) fn rare_set(&self) -> &RareSetRef {
        // SAFETY: `RareSetRef` is a transparent wrapper of the set's handle.
        unsafe { &*std::ptr::from_ref(&self.rare).cast::<RareSetRef>() }
    }

    /// These groups, holding `facts` and `custom_declarations` beside their rare groups: in the
    /// one of `shares` that holds all three, where one does, otherwise in their own set, copied
    /// first where it is shared.
    pub(crate) fn with_rare_facts(
        mut self,
        facts: std::sync::Arc<crate::css::style::pipeline::RareStyleFacts>,
        custom_declarations: Option<crate::css::style::pipeline::CustomDeclarations>,
        shares: &[Option<&RareSetRef>],
    ) -> Self {
        let beside = crate::css::style::pipeline::RareSetFacts {
            facts: Some(facts),
            custom_declarations,
        };
        let holds_facts = |set: &RareSet| set.beside.is(&beside);
        if holds_facts(self.rare.get()) {
            return self;
        }
        if let Some(shared) = shares
            .iter()
            .flatten()
            .map(|share| &share.0)
            .find(|set| set.get().groups == self.rare.get().groups && holds_facts(set.get()))
        {
            self.rare = shared.clone();
            return self;
        }
        self.rare.make_mut().beside = beside;
        self
    }

    /// Whether `other` holds this style's inherited set itself, and so its inherited groups.
    pub(crate) fn shares_inherited_groups_with(&self, other: &StyleGroups) -> bool {
        self.inherited.ptr_eq(&other.inherited)
    }

    /// Whether `other` holds this style's root groups and its other sets themselves, and so its
    /// groups of the properties that do not inherit.
    pub(crate) fn shares_reset_groups_with(&self, other: &StyleGroups) -> bool {
        self.root == other.root && self.rare.ptr_eq(&other.rare) && self.visual.ptr_eq(&other.visual)
    }

    /// The groups `other` holds the very same payload of, one bit per group: a set both hold
    /// itself holds the same payloads, so only the groups of sets they hold apart are compared.
    pub(crate) fn shared_group_mask(&self, other: &StyleGroups) -> u32 {
        fn set_mask(groups: &[usize], mine: &[GroupPointer], theirs: &[GroupPointer], same_set: bool) -> u32 {
            groups
                .iter()
                .zip(mine.iter().zip(theirs))
                .filter(|(_, (mine, theirs))| same_set || mine == theirs)
                .fold(0, |mask, (&group, _)| mask | 1 << group)
        }
        let (inherited, rare, visual) = (
            self.inherited.ptr_eq(&other.inherited),
            self.rare.ptr_eq(&other.rare),
            self.visual.ptr_eq(&other.visual),
        );
        set_mask(&ROOT_GROUPS, &self.root, &other.root, false)
            | set_mask(
                &INHERITED_SET_GROUPS,
                &self.inherited.get().groups,
                &other.inherited.get().groups,
                inherited,
            )
            | set_mask(
                &RARE_SET_GROUPS,
                &self.rare.get().groups,
                &other.rare.get().groups,
                rare,
            )
            | set_mask(
                &VISUAL_SET_GROUPS,
                &self.visual.get().groups,
                &other.visual.get().groups,
                visual,
            )
    }

    /// Whether `other` holds equal payloads of every group, and the same inherited bits and
    /// zoom: the very same ones where it holds a set itself, and else each group's very same
    /// payload or an equal one.
    pub(crate) fn holds_equal_payloads_as(&self, other: &StyleGroups) -> bool {
        fn equal(groups: &[usize], mine: &[GroupPointer], theirs: &[GroupPointer]) -> bool {
            groups
                .iter()
                .zip(mine.iter().zip(theirs))
                .all(|(&group, (mine, theirs))| {
                    // SAFETY: Both styles hold a reference to each of their groups.
                    mine == theirs || unsafe { style_group_payloads_equal(group, mine.as_ptr(), theirs.as_ptr()) }
                })
        }
        self.reset == other.reset
            && equal(&ROOT_GROUPS, &self.root, &other.root)
            && (self.inherited.ptr_eq(&other.inherited)
                || (self.inherited_values() == other.inherited_values()
                    && equal(
                        &INHERITED_SET_GROUPS,
                        &self.inherited.get().groups,
                        &other.inherited.get().groups,
                    )))
            && (self.rare.ptr_eq(&other.rare)
                || equal(&RARE_SET_GROUPS, &self.rare.get().groups, &other.rare.get().groups))
            && (self.visual.ptr_eq(&other.visual)
                || equal(
                    &VISUAL_SET_GROUPS,
                    &self.visual.get().groups,
                    &other.visual.get().groups,
                ))
    }

    /// Whether `other` holds the same groups.
    pub(crate) fn holds_the_same_groups_as(&self, other: &StyleGroups) -> bool {
        self.root == other.root
            && self.reset == other.reset
            && (self.inherited.ptr_eq(&other.inherited)
                || (self.inherited.get().groups == other.inherited.get().groups
                    && self.inherited_values() == other.inherited_values()))
            && (self.visual.ptr_eq(&other.visual) || self.visual.get().groups == other.visual.get().groups)
            && (self.rare.ptr_eq(&other.rare) || self.rare.get().groups == other.rare.get().groups)
    }

    pub(crate) fn group(&self, index: StyleGroupIndex) -> GroupPayloadRef<'_> {
        GroupPayloadRef {
            group_index: index.usize(),
            pointer: self[index.usize()].0,
            payload: std::marker::PhantomData,
        }
    }

    fn slot_mut(&mut self, group: usize) -> &mut GroupPointer {
        match GROUP_PLACES[group] {
            GroupPlace::Root(slot) => &mut self.root[slot as usize],
            GroupPlace::Inherited(slot) => &mut self.inherited.make_mut().groups[slot as usize],
            GroupPlace::Visual(slot) => &mut self.visual.make_mut().groups[slot as usize],
            GroupPlace::Rare(slot) => &mut self.rare.make_mut().groups[slot as usize],
        }
    }

    /// Puts `group` in place of the group at its index, answering the one it held.
    pub(crate) fn replace(&mut self, group: HeldGroup) -> HeldGroup {
        let index = group.group;
        let new = GroupPointer(std::ptr::NonNull::new(group.payload.cast_mut()).expect("a style group is never null"));
        std::mem::forget(group);
        if self[index] == new {
            // A set another style shares stays shared where its group does not change.
            return HeldGroup {
                group: index,
                payload: new.as_ptr(),
            };
        }
        let pointer = std::mem::replace(self.slot_mut(index), new);
        HeldGroup {
            group: index,
            payload: pointer.as_ptr(),
        }
    }

    /// Puts `other`'s group at `index` in place of this one's.
    pub(crate) fn share_group(&mut self, other: &StyleGroups, index: StyleGroupIndex) {
        drop(self.replace(other.group(index).held()));
    }

    /// Puts a copy of the group of `T`, written by `write`, in place of it.
    ///
    /// # Safety
    /// The group at `index` must be a `T`.
    pub(crate) unsafe fn write_group<T>(&mut self, index: usize, write: impl FnOnce(&mut T)) {
        // SAFETY: The style holds a reference to its group, and the copy is uniquely owned.
        let copy = unsafe { clone_group_payload(index, self[index].as_ptr()) };
        // SAFETY: As the caller guarantees, the copy is a `T`.
        write(unsafe { &mut *copy.cast::<T>() });
        drop(self.replace(HeldGroup {
            group: index,
            payload: copy.cast_const(),
        }));
    }

    /// The groups, in group order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = GroupPointer> + '_ {
        (0..group_index::COUNT).map(|group| self[group])
    }
}

impl std::ops::Index<usize> for StyleGroups {
    type Output = GroupPointer;

    #[inline]
    fn index(&self, group: usize) -> &GroupPointer {
        match GROUP_PLACES[group] {
            GroupPlace::Root(slot) => &self.root[slot as usize],
            GroupPlace::Inherited(slot) => &self.inherited.get().groups[slot as usize],
            GroupPlace::Visual(slot) => &self.visual.get().groups[slot as usize],
            GroupPlace::Rare(slot) => &self.rare.get().groups[slot as usize],
        }
    }
}

impl Clone for StyleGroups {
    fn clone(&self) -> Self {
        for (pointer, group) in self.root.iter().zip(ROOT_GROUPS) {
            retain_group_payload(group, pointer.as_ptr());
        }
        Self {
            root: self.root,
            inherited: self.inherited.clone(),
            rare: self.rare.clone(),
            visual: self.visual.clone(),
            reset: self.reset,
            current_color: self.current_color,
        }
    }
}

impl Drop for StyleGroups {
    fn drop(&mut self) {
        for (pointer, group) in self.root.iter().zip(ROOT_GROUPS) {
            release_group_payload(group, pointer.as_ptr());
        }
    }
}

impl std::fmt::Debug for StyleGroups {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_list().entries(self.pointers()).finish()
    }
}

/// A live payload of the style group at `group_index`, borrowed for `'a`.
#[derive(Clone, Copy)]
pub(crate) struct GroupPayloadRef<'a> {
    group_index: usize,
    pointer: std::ptr::NonNull<c_void>,
    payload: std::marker::PhantomData<&'a c_void>,
}

impl<'a> GroupPayloadRef<'a> {
    /// The payload `pointer` names, or `None` for a null pointer.
    ///
    /// # Safety
    /// A non-null `pointer` must be a payload of the group at `group_index` that stays live and
    /// unmodified for `'a`.
    pub(crate) unsafe fn new(group_index: usize, pointer: *const c_void) -> Option<Self> {
        assert!(group_index < group_index::COUNT);
        Some(Self {
            group_index,
            pointer: std::ptr::NonNull::new(pointer.cast_mut())?,
            payload: std::marker::PhantomData,
        })
    }

    /// The computed values the payload's group holds.
    pub(crate) fn group_values(self) -> &'a [RetainedStyleValueData] {
        // SAFETY: The payload is live for 'a.
        unsafe { values_of(self.group_index, self.as_ptr()) }
    }

    pub(crate) fn as_ptr(self) -> *const c_void {
        self.pointer.as_ptr().cast_const()
    }

    /// The payload as its group's type, which must be `T`.
    fn values<T: StyleGroupPayload>(self) -> &'a T {
        assert_group_payload_type::<T>(self.group_index);
        // SAFETY: The payload is live for 'a and its group's type is `T`.
        unsafe { payload_ref::<T>(self.as_ptr()) }
    }

    /// Whether the payload holds the group's initial values, as a default-constructed payload does.
    pub(crate) fn holds_the_defaults(self) -> bool {
        self.equals(ScratchPayload::new_default(self.group_index).as_ref())
    }

    /// Whether `other`, a payload of the same group, holds the same values.
    fn equals(self, other: GroupPayloadRef<'_>) -> bool {
        assert_eq!(self.group_index, other.group_index);
        // SAFETY: Both payloads are live payloads of the group.
        unsafe { payloads_equal(self.group_index, self.as_ptr(), other.as_ptr()) }
    }

    /// The group, with one more reference to it.
    pub(crate) fn held(self) -> HeldGroup {
        HeldGroup {
            group: self.group_index,
            payload: self.retained(),
        }
    }

    /// The payload, with one more reference for the caller.
    fn retained(self) -> *const c_void {
        retain_group_payload(self.group_index, self.as_ptr());
        self.as_ptr()
    }
}

/// A freshly allocated payload of one group, which holds the payload's one reference and frees it unless published.
struct ScratchPayload {
    group_index: usize,
    pointer: *mut c_void,
}

impl ScratchPayload {
    fn new_default(group_index: usize) -> Self {
        let pointer = allocate_payload(group_index, 1);
        // SAFETY: The payload was just allocated with the layout of the group's type.
        unsafe { default_construct(group_index, pointer) };
        Self { group_index, pointer }
    }

    fn apply_pokes(&mut self, pokes: &[GroupFieldPoke<'_>]) {
        // SAFETY: The pokes come from the group's registered descriptors, whose offsets name fields of the right
        //         type in the group's payload, and nothing else references the scratch payload yet.
        unsafe { apply_group_field_pokes(self.pointer, pokes) };
    }

    /// The payload as its group's type, which must be `T`.
    fn values_mut<T: StyleGroupPayload>(&mut self) -> &mut T {
        assert_group_payload_type::<T>(self.group_index);
        // SAFETY: The scratch payload is a live payload of type `T` that only this guard references.
        unsafe { &mut *self.pointer.cast::<T>() }
    }

    fn as_ref(&self) -> GroupPayloadRef<'_> {
        // SAFETY: The scratch payload is live while the guard is, and nothing modifies it while borrowed.
        unsafe { GroupPayloadRef::new(self.group_index, self.pointer) }.expect("allocation never returns null")
    }

    /// Hands the payload and its one reference to the caller.
    fn publish(self) -> *const c_void {
        let pointer = self.pointer.cast_const();
        std::mem::forget(self);
        pointer
    }
}

impl Drop for ScratchPayload {
    fn drop(&mut self) {
        // SAFETY: The scratch payload was never published, so this guard holds its only reference.
        unsafe { free_scratch_payload(self.group_index, self.pointer) };
    }
}

/// The payload a build of the group at `group_index` can share instead of publishing its own: the parent's payload,
/// retained for the caller, or the group's default payload, whichever `matches` first.
fn share_matching_group_payload(
    group_index: usize,
    shares: GroupShares<'_>,
    matches: impl Fn(GroupPayloadRef<'_>) -> bool,
) -> Option<*const c_void> {
    if let Some(parent) = shares.parent {
        assert_eq!(parent.group_index, group_index);
        if matches(parent) {
            return Some(parent.retained());
        }
    }
    assert_eq!(shares.default.group_index, group_index);
    matches(shares.default).then(|| shares.default.retained())
}

/// The payloads a group being built may share instead of holding its own: the parent's payload of
/// the group, if any, and else the process's default payload.
#[derive(Clone, Copy)]
pub(crate) struct GroupShares<'a> {
    pub(crate) parent: Option<GroupPayloadRef<'a>>,
    pub(crate) default: GroupPayloadRef<'a>,
}

impl<'a> GroupShares<'a> {
    pub(crate) fn new(defaults: &'a DefaultGroups, group: usize, parent: Option<GroupPayloadRef<'a>>) -> Self {
        Self {
            parent,
            default: defaults.groups().group(StyleGroupIndex::new(group)),
        }
    }

    /// The default payload, with one more reference to it.
    pub(crate) fn default_retained(self) -> *const c_void {
        self.default.retained()
    }
}

/// The payload `built` can share instead of being published.
fn share_equal_group_payload(built: GroupPayloadRef<'_>, parent: GroupShares<'_>) -> Option<*const c_void> {
    share_matching_group_payload(built.group_index, parent, |candidate| built.equals(candidate))
}

/// The payload a built group value can share instead of allocating its own.
fn share_group_payload<T: StyleGroupPayload + PartialEq>(
    group_index: usize,
    built: &T,
    parent: GroupShares<'_>,
) -> Option<*const c_void> {
    assert_group_payload_type::<T>(group_index);
    share_matching_group_payload(group_index, parent, |candidate| built == candidate.values::<T>())
}

/// The payload a built group value shares, or else a fresh payload holding it. Either carries one reference for the
/// caller.
fn share_or_publish_group_payload<T: StyleGroupPayload + PartialEq>(
    group_index: usize,
    built: T,
    parent: GroupShares<'_>,
) -> *const c_void {
    share_group_payload(group_index, &built, parent).unwrap_or_else(|| publish_group_payload(group_index, built))
}

/// The payload of a group holding the values `source` holds but for what `patch` changes: a
/// shared payload where one equals it, or else a fresh one. Either carries one reference for the
/// caller.
///
/// # Safety
/// `source` must be a live payload of the group at `group_index`, whose type is `T`.
pub(crate) unsafe fn patch_group_payload<T: StyleGroupPayload + PartialEq + Clone>(
    group_index: usize,
    source: *const c_void,
    patch: impl FnOnce(&mut T),
    parent: GroupShares<'_>,
) -> *const c_void {
    assert_group_payload_type::<T>(group_index);
    // SAFETY: The caller passes a live payload of the group, whose type was just asserted.
    let mut built = unsafe { payload_ref::<T>(source) }.clone();
    patch(&mut built);
    share_or_publish_group_payload(group_index, built, parent)
}

/// Moves a built group value into a fresh payload that carries one reference for the caller.
fn publish_group_payload<T: StyleGroupPayload>(group_index: usize, built: T) -> *const c_void {
    assert_group_payload_type::<T>(group_index);
    let payload = allocate_payload(group_index, 1);
    // SAFETY: The payload was just allocated with the layout of the group's type, which is `T`.
    unsafe { payload.cast::<T>().write(built) };
    payload.cast_const()
}

/// Allocates a new payload for `group_index` with a reference count of one,
/// copy-constructed from `source`, a live payload of the registered group type.
pub(crate) unsafe fn clone_group_payload(group_index: usize, source: *const c_void) -> *mut c_void {
    let payload = allocate_payload(group_index, 1);
    unsafe {
        copy_construct(group_index, payload, source);
        values_of_mut(group_index, payload).clone_from_slice(values_of(group_index, source));
    }
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
        debug_assert!(refcount_of(payload, payload_align(group_index)).load(Ordering::Relaxed) == 0);
        destroy_allocation(group_index, payload);
    };
}

pub(crate) fn release_group_payload(group_index: usize, payload: *const c_void) {
    let refcount = refcount_of(payload, payload_align(group_index));
    if refcount.fetch_sub(1, Ordering::AcqRel) == 1 {
        // SAFETY: The count reached zero, so this reference was the last one.
        unsafe { destroy_allocation(group_index, payload.cast_mut()) }
    }
}

/// The computed values the group of `payload`, a payload of the group at `group_index`, holds, in
/// the order of the group's longhands.
///
/// # Safety
/// `payload` must be a live payload of the group that stays live for `'a`.
pub(crate) unsafe fn group_values<'a>(group_index: usize, payload: *const c_void) -> &'a [RetainedStyleValueData] {
    let (offset, count) = GROUP_VALUE_SPANS[group_index];
    // SAFETY: The allocation holds `count` initialized values `offset` bytes from the payload.
    unsafe { ak::slice_from_raw_parts(payload.byte_add(offset).cast::<RetainedStyleValueData>(), count) }
}

/// Per group, how far from a payload its values start and how many it holds.
const GROUP_VALUE_SPANS: [(usize, usize); group_index::COUNT] = {
    let mut spans = [(0, 0); group_index::COUNT];
    let mut slot = 0;
    while slot < group_index::COUNT {
        let (size, align) = with_style_group_type!(slot, Payload => (size_of::<Payload>(), align_of::<Payload>()));
        let header = if align > size_of::<usize>() {
            align
        } else {
            size_of::<usize>()
        };
        let offset = (header + size).next_multiple_of(align_of::<RetainedStyleValueData>()) - header;
        spans[slot] = (offset, GROUP_VALUE_LAYOUT.counts[slot] as usize);
        slot += 1;
    }
    spans
};

/// The bytes of the computed values the group of `payload`, a payload of the group at `group_index`,
/// holds, in its values and in its payload handles, as `sizer` has not counted them yet.
///
/// # Safety
/// `payload` must be a live payload of the group.
pub(crate) unsafe fn group_held_values_heap_size(
    group_index: usize,
    payload: *const c_void,
    sizer: &mut crate::heap_size::HeapSizer,
) -> usize {
    use crate::heap_size::HeapSize;
    // SAFETY: As the caller guarantees.
    let values = unsafe { group_values(group_index, payload) };
    GroupValueLayout::handles_of(group_index).fold(values.heap_size(sizer), |bytes, (_, place)| {
        // SAFETY: As the caller guarantees.
        bytes + unsafe { place.value_in(payload, values) }.heap_size(sizer)
    })
}

/// The computed value of `property` in the groups `group_of` answers by group index, with
/// `inherited` beside the inherited groups.
///
/// # Safety
/// The groups `group_of` answers must be live payloads of the group at their index, which stay
/// live for `'a`.
unsafe fn value_in_groups_of<'a>(
    group_of: impl Fn(usize) -> *const c_void + Copy,
    bits: &StyleBits,
    property: u16,
) -> &'a RetainedStyleValueData {
    // SAFETY: As the caller guarantees.
    let property = GROUP_VALUE_LAYOUT.resolved(property, |property| unsafe {
        value_in_groups_of(group_of, bits, property)
    });
    let place = GROUP_VALUE_LAYOUT.place_of(property);
    if let Some(field) = place.bit_field() {
        return bits.value(field);
    }
    // SAFETY: As the caller guarantees.
    unsafe {
        let payload = group_of(place.group());
        place.value_in(payload, group_values(place.group(), payload))
    }
}

/// The computed value at `place` of `group`, a payload of the place's group.
pub(crate) fn value_at(group: GroupPayloadRef<'_>, place: LonghandPlace) -> &RetainedStyleValueData {
    // SAFETY: The payload is live for as long as `group` is borrowed and holds its group's values.
    unsafe { place.value_in(group.as_ptr(), group.group_values()) }
}

/// Whether the groups `first` and `second`, payloads of the group at `group_index`, hold the same
/// computed values, each by identity or else by value, in their values and their payload handles.
///
/// # Safety
/// Both must be live payloads of the group.
pub(crate) unsafe fn hold_the_same_values(group_index: usize, first: *const c_void, second: *const c_void) -> bool {
    let (first_values, second_values) =
        unsafe { (group_values(group_index, first), group_values(group_index, second)) };
    same_group_values(first_values, second_values)
        && GroupValueLayout::handles_of(group_index).all(|(_, place)| {
            // SAFETY: As the caller guarantees.
            let (first, second) = unsafe {
                (
                    place.value_in(first, first_values),
                    place.value_in(second, second_values),
                )
            };
            first.pointer() == second.pointer() || first == second
        })
}

/// Whether `first` and `second` hold the same computed values, each by identity or else by value.
pub(crate) fn same_group_values(first: &[RetainedStyleValueData], second: &[RetainedStyleValueData]) -> bool {
    first.len() == second.len()
        && first
            .iter()
            .zip(second)
            .all(|(first, second)| first.pointer() == second.pointer() || first == second)
}

/// Whether the groups `first` and `second`, payloads of the group at `group_index`, hold the same
/// payload and the same computed values.
///
/// # Safety
/// Both must be live payloads of the group.
pub(crate) unsafe fn groups_hold_the_same(group_index: usize, first: *const c_void, second: *const c_void) -> bool {
    first == second
        || unsafe {
            payloads_equal(group_index, first, second)
                && same_group_values(values_of(group_index, first), values_of(group_index, second))
        }
}

/// The group holding the payload of `payload` and the computed values of `values`, a seed group's,
/// with `writes` over them, the writes of the group in place order. It takes over the caller's
/// reference to `payload` and carries one for the caller: `payload` itself where it holds those
/// values already, else the seed where nothing is written over it and it holds the same group,
/// else `payload` with the values written into it where only the caller references it, else a copy
/// of it holding them.
///
/// # Safety
/// `payload` and `seed` must be live payloads of the group at `group_index`, and `values` the values
/// of `seed`.
unsafe fn group_with_values(
    group_index: usize,
    payload: *const c_void,
    seed: *const c_void,
    values: &[RetainedStyleValueData],
    writes: &[ValueWrite],
) -> *const c_void {
    assert_eq!(values.len(), value_count(group_index));
    let written = || written_values(values, writes);
    // SAFETY: The caller passes live payloads of the group.
    unsafe {
        if values_of(group_index, payload)
            .iter()
            .zip(written())
            .all(|(held, value)| held.pointer() == value.pointer() || held == value)
        {
            return payload;
        }
        if writes.is_empty()
            && seed != payload
            && payloads_equal(group_index, seed, payload)
            && values_of(group_index, seed)
                .iter()
                .zip(written())
                .all(|(held, value)| held.pointer() == value.pointer())
        {
            retain_group_payload(group_index, seed);
            release_group_payload(group_index, payload);
            return seed;
        }
        let write_into = |target: *mut c_void| {
            for (slot, value) in values_of_mut(group_index, target).iter_mut().zip(written()) {
                *slot = value.clone();
            }
        };
        if refcount_of(payload, payload_align(group_index)).load(Ordering::Acquire) == 1 {
            write_into(payload.cast_mut());
            return payload;
        }
        let copy = allocate_payload(group_index, 1);
        copy_construct(group_index, copy, payload);
        write_into(copy);
        release_group_payload(group_index, payload);
        copy.cast_const()
    }
}

/// Clamps a border box size by the min and max sizes of a sizing group that resolve against the given basis, as
/// resizing an element by its resizer does.
#[unsafe(no_mangle)]
pub extern "C" fn rust_sizing_values_clamp_size(
    values: &SizingValues,
    basis_width: f64,
    basis_height: f64,
    width: &mut f64,
    height: &mut f64,
) {
    use crate::css::css_pixels::CssPixels;
    let clamp = |size: f64, basis: f64, minimum: &ComputedSize, maximum: &ComputedSize| {
        let basis = CssPixels::nearest_value_for(basis);
        let mut size = size;
        if minimum.is_length_percentage() {
            size = size.max(minimum.to_px(basis).to_double());
        }
        if maximum.is_length_percentage() {
            size = size.min(maximum.to_px(basis).to_double());
        }
        size
    };
    *width = clamp(*width, basis_width, &values.min_width, &values.max_width);
    *height = clamp(*height, basis_height, &values.min_height, &values.max_height);
}

/// The used line height of a font group, as [`FontValues::line_height_used`] reads it.
#[unsafe(no_mangle)]
pub extern "C" fn rust_font_values_line_height_used_raw(values: &FontValues) -> i32 {
    values.line_height_used().raw_value()
}

/// The pixels of a computed size given as an absolute length, written to `pixels`. Returns false for any other size.
#[unsafe(no_mangle)]
pub extern "C" fn rust_computed_size_absolute_length_px(size: &ComputedSize, pixels: &mut f64) -> bool {
    let Some(StyleValueData::Length { value, unit }) = size.value.style_value() else {
        return false;
    };
    if size.kind != ComputedSizeKind::Length {
        return false;
    }
    let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
    if !ratio.is_finite() {
        return false;
    }
    *pixels = value * ratio;
    true
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
    /// A color stored as a [`crate::css::computed_value_types::StyleColor`]: plain `currentcolor`
    /// stays for the reader to resolve, any other color is resolved by the gather loop.
    HeldColor,
    /// A color like `HeldColor`, except that this keyword leaves the initial value standing, for
    /// fields like outline-color whose auto keyword is not a resolvable color.
    HeldColorOrKeyword(u16),
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
            TEXT_DECORATION_COLOR @ text_decoration_color => HeldColor,
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
        group_index::MISC_LAYOUT => fields! { MiscLayoutValues:
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
            OUTLINE_COLOR @ outline_color => HeldColorOrKeyword(k::AUTO),
            OUTLINE_OFFSET => RequireZeroPx,
            OUTLINE_WIDTH @ outline_width => CssPixelsNonNegative,
            COLUMN_RULE_COLOR @ column_rule_color => HeldColor,
            COLUMN_RULE_WIDTH @ column_rule_width => CssPixelsNonNegative,
            OBJECT_POSITION => RequireInitialValue,
            SHAPE_IMAGE_THRESHOLD @ shape_image_threshold => ResolvedF64,
            SHAPE_MARGIN => RequireZeroPx,
            SHAPE_OUTSIDE => RequireKeyword(k::NONE),
            CONTENT_VISIBILITY @ content_visibility => EnumKeyword(enums::keyword_to_content_visibility),
        },
        group_index::MISC_INTERACTION => fields! { MiscInteractionValues:
            SCROLL_MARGIN_TOP => RequireZeroPx,
            SCROLL_MARGIN_RIGHT => RequireZeroPx,
            SCROLL_MARGIN_BOTTOM => RequireZeroPx,
            SCROLL_MARGIN_LEFT => RequireZeroPx,
            SCROLL_PADDING_TOP => RequireKeyword(k::AUTO),
            SCROLL_PADDING_RIGHT => RequireKeyword(k::AUTO),
            SCROLL_PADDING_BOTTOM => RequireKeyword(k::AUTO),
            SCROLL_PADDING_LEFT => RequireKeyword(k::AUTO),
            USER_SELECT @ user_select => EnumKeyword(enums::keyword_to_user_select),
            VIEW_TRANSITION_CLASS => RequireKeyword(k::NONE),
            VIEW_TRANSITION_NAME => RequireKeyword(k::NONE),
            TOUCH_ACTION => RequireInitialValue,
            SCROLL_BEHAVIOR @ scroll_behavior => EnumKeyword(enums::keyword_to_scroll_behavior),
            SCROLL_SNAP_ALIGN => RequireInitialValue,
            SCROLL_SNAP_STOP @ scroll_snap_stop => EnumKeyword(enums::keyword_to_scroll_snap_stop),
            SCROLL_SNAP_TYPE => RequireInitialValue,
            SCROLLBAR_GUTTER => RequireInitialValue,
            SCROLLBAR_WIDTH @ scrollbar_width => EnumKeyword(enums::keyword_to_scrollbar_width),
            OVERSCROLL_BEHAVIOR_X @ overscroll_behavior_x => EnumKeyword(enums::keyword_to_overscroll_behavior),
            OVERSCROLL_BEHAVIOR_Y @ overscroll_behavior_y => EnumKeyword(enums::keyword_to_overscroll_behavior),
            WILL_CHANGE => RequireKeyword(k::AUTO),
        },
        group_index::INHERITED_TEXT => fields! { InheritedTextValues:
            _WEBKIT_FONT_SMOOTHING @ font_smoothing => EnumKeyword(enums::keyword_to_font_smoothing),
            COLOR @ color => Color,
            COLOR @ color_style_value => RetainedData,
            _WEBKIT_TEXT_FILL_COLOR @ webkit_text_fill_color => Color,
            _WEBKIT_TEXT_FILL_COLOR @ webkit_text_fill_color_is_current_color => KeywordEqualsBool(k::CURRENTCOLOR),
            TEXT_SHADOW => RequireKeyword(k::NONE),
            TEXT_JUSTIFY @ text_justify => EnumKeyword(enums::keyword_to_text_justify),
            TEXT_TRANSFORM @ text_transform => EnumKeyword(enums::keyword_to_text_transform),
            TEXT_WRAP_STYLE @ text_wrap_style => EnumKeyword(enums::keyword_to_text_wrap_style),
            TEXT_DECORATION_SKIP_INK @ text_decoration_skip_ink => EnumKeyword(enums::keyword_to_text_decoration_skip_ink),
            TEXT_UNDERLINE_POSITION => RequireKeyword(k::AUTO),
            TEXT_UNDERLINE_OFFSET => RequireKeyword(k::AUTO),
            TEXT_INDENT => RequireInitialValue,
            TAB_SIZE => RequireInitialValue,
            LINE_BREAK @ line_break => EnumKeyword(enums::keyword_to_line_break),
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
            CARET_COLOR => RequireKeyword(k::AUTO),
            ACCENT_COLOR @ accent_color.color => HeldColor,
            ACCENT_COLOR => RequireKeyword(k::AUTO),
            CURSOR => RequireKeyword(k::AUTO),
            FORCED_COLOR_ADJUST @ forced_color_adjust => EnumKeyword(enums::keyword_to_forced_color_adjust),
            INTERPOLATE_SIZE @ interpolate_size => EnumKeyword(enums::keyword_to_interpolate_size),
            SCROLLBAR_COLOR => RequireKeyword(k::AUTO),
            COLOR_SCHEME @ color_scheme => ResolvedU8,
            COLOR_SCHEME => RequireInitialValue,
            IMAGE_RENDERING @ image_rendering => EnumKeyword(enums::keyword_to_image_rendering),
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
            OFFSET_PATH => RequireKeyword(k::NONE),
            OFFSET_DISTANCE => RequireZeroPx,
            OFFSET_POSITION => RequireKeyword(k::NORMAL),
            OFFSET_ANCHOR => RequireKeyword(k::AUTO),
            OFFSET_ROTATE => RequireKeyword(k::AUTO),
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
            ANIMATION_RANGE_START => RequireInitialValue,
            ANIMATION_RANGE_END => RequireInitialValue,
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
        group_index::INHERITED_LIST_TABLE => fields! { InheritedListTableValues:
            LIST_STYLE_TYPE => RequireInitialValue,
            LIST_STYLE_IMAGE => RequireKeyword(k::NONE),
            QUOTES => RequireKeyword(k::AUTO),
            BORDER_SPACING => RequireInitialValue,
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
            BORDER_LEFT_COLOR @ border_left.color => HeldColor,
            BORDER_LEFT_COLOR => RequireInitialValue,
            BORDER_LEFT_STYLE => RequireKeyword(k::NONE),
            BORDER_LEFT_WIDTH @ border_left_computed_width => CssPixelsNonNegative,
            BORDER_TOP_COLOR @ border_top.color => HeldColor,
            BORDER_TOP_COLOR => RequireInitialValue,
            BORDER_TOP_STYLE => RequireKeyword(k::NONE),
            BORDER_TOP_WIDTH @ border_top_computed_width => CssPixelsNonNegative,
            BORDER_RIGHT_COLOR @ border_right.color => HeldColor,
            BORDER_RIGHT_COLOR => RequireInitialValue,
            BORDER_RIGHT_STYLE => RequireKeyword(k::NONE),
            BORDER_RIGHT_WIDTH @ border_right_computed_width => CssPixelsNonNegative,
            BORDER_BOTTOM_COLOR @ border_bottom.color => HeldColor,
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
            BORDER_SHAPE => RequireKeyword(k::NONE),
            BORDER_IMAGE_OUTSET => RequireInitialValue,
            BORDER_IMAGE_REPEAT => RequireInitialValue,
            BORDER_IMAGE_SLICE => RequireInitialValue,
            BORDER_IMAGE_WIDTH => RequireInitialValue,
        },
        group_index::BACKGROUND => fields! { BackgroundValues:
            BACKGROUND_COLOR @ background_color => HeldColor,
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

/// How many longhands there are, logical aliases included.
const LONGHAND_COUNT: usize = (crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
    - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
    + 1) as usize;

const fn longhand_index_of(property: u16) -> usize {
    (property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID) as usize
}

const fn is_logical_alias(property: u16) -> bool {
    use crate::css::property_metadata::{FIRST_SHORTHAND_PROPERTY_ID, PROPERTY_IS_LOGICAL_ALIAS};
    PROPERTY_IS_LOGICAL_ALIAS[(property - FIRST_SHORTHAND_PROPERTY_ID) as usize]
}

/// Whether `property` is a logical alias that maps to a physical longhand in every flow, which it
/// then computes as.
const fn maps_in_every_flow(property: u16) -> bool {
    use crate::css::style_compute::{DIRECTION_COUNT, WRITING_MODE_COUNT};
    if !is_logical_alias(property) {
        return false;
    }
    let mut writing_mode = 0;
    while writing_mode < WRITING_MODE_COUNT {
        let mut direction = 0;
        while direction < DIRECTION_COUNT {
            if physical_longhand(property, writing_mode, direction) == property {
                return false;
            }
            direction += 1;
        }
        writing_mode += 1;
    }
    true
}

/// As `map_logical_alias_to_physical`, for a longhand.
const fn physical_longhand(property: u16, writing_mode: usize, direction: usize) -> u16 {
    use crate::css::property_metadata::LOGICAL_ALIAS_TO_PHYSICAL;
    use crate::css::style_compute::{DIRECTION_COUNT, WRITING_MODE_COUNT};
    match LOGICAL_ALIAS_TO_PHYSICAL
        [(longhand_index_of(property) * WRITING_MODE_COUNT + writing_mode) * DIRECTION_COUNT + direction]
    {
        0 => property,
        physical => physical,
    }
}

/// The physical longhands `transition-property: all` names, per writing mode and direction, in the
/// order the recursive expansion of `all` lists them, logical aliases mapped: a table the compiler
/// computes from the property metadata.
struct AllTransitionLonghands {
    longhands: [[u16; ALL_TRANSITION_LONGHAND_COUNT];
        crate::css::style_compute::WRITING_MODE_COUNT * crate::css::style_compute::DIRECTION_COUNT],
}

/// How many longhands the recursive expansion of `all` lists, a logical pair's longhands each once.
const ALL_TRANSITION_LONGHAND_COUNT: usize =
    all_transition_longhand_count(crate::css::property_metadata::property_id::ALL);

const fn all_transition_longhand_count(property: u16) -> usize {
    use crate::css::property_metadata::{
        FIRST_SHORTHAND_PROPERTY_ID, LAST_SHORTHAND_PROPERTY_ID, SHORTHAND_EXPANSIONS, property_id,
    };
    if property >= FIRST_SHORTHAND_PROPERTY_ID && property <= LAST_SHORTHAND_PROPERTY_ID {
        let longhands = SHORTHAND_EXPANSIONS[(property - FIRST_SHORTHAND_PROPERTY_ID) as usize];
        let mut count = 0;
        let mut index = 0;
        while index < longhands.len() {
            count += all_transition_longhand_count(longhands[index]);
            index += 1;
        }
        return count;
    }
    (property != property_id::CUSTOM) as usize
}

impl AllTransitionLonghands {
    const fn compute() -> Self {
        use crate::css::style_compute::{DIRECTION_COUNT, WRITING_MODE_COUNT};
        let mut longhands = [[0; ALL_TRANSITION_LONGHAND_COUNT]; WRITING_MODE_COUNT * DIRECTION_COUNT];
        let mut writing_mode = 0;
        while writing_mode < WRITING_MODE_COUNT {
            let mut direction = 0;
            while direction < DIRECTION_COUNT {
                let row = &mut longhands[writing_mode * DIRECTION_COUNT + direction];
                let written = Self::expand(
                    crate::css::property_metadata::property_id::ALL,
                    writing_mode,
                    direction,
                    row,
                    0,
                );
                assert!(written == ALL_TRANSITION_LONGHAND_COUNT);
                direction += 1;
            }
            writing_mode += 1;
        }
        Self { longhands }
    }

    /// Writes the physical longhands `property` names from `at` on, answering where it stopped.
    const fn expand(
        property: u16,
        writing_mode: usize,
        direction: usize,
        row: &mut [u16; ALL_TRANSITION_LONGHAND_COUNT],
        at: usize,
    ) -> usize {
        use crate::css::property_metadata::{
            FIRST_SHORTHAND_PROPERTY_ID, LAST_SHORTHAND_PROPERTY_ID, SHORTHAND_EXPANSIONS, property_id,
        };
        if property >= FIRST_SHORTHAND_PROPERTY_ID && property <= LAST_SHORTHAND_PROPERTY_ID {
            let longhands = SHORTHAND_EXPANSIONS[(property - FIRST_SHORTHAND_PROPERTY_ID) as usize];
            let mut at = at;
            let mut index = 0;
            while index < longhands.len() {
                at = Self::expand(longhands[index], writing_mode, direction, row, at);
                index += 1;
            }
            return at;
        }
        if property == property_id::CUSTOM {
            return at;
        }
        row[at] = physical_longhand(property, writing_mode, direction);
        at + 1
    }
}

const ALL_TRANSITION_LONGHANDS: AllTransitionLonghands = AllTransitionLonghands::compute();

/// The physical longhands `transition-property: all` names under `writing_mode` and `direction`.
pub(crate) fn all_transition_longhands(writing_mode: u8, direction: u8) -> &'static [u16] {
    const TABLE: &AllTransitionLonghands = &ALL_TRANSITION_LONGHANDS;
    &TABLE.longhands[usize::from(writing_mode) * crate::css::style_compute::DIRECTION_COUNT + usize::from(direction)]
}

/// The computed style groups each longhand reaches, a table the compiler computes from the property metadata.
pub(crate) struct StyleGroupMasks {
    /// Per longhand, the groups a change of its specified winner may change.
    masks: [u32; LONGHAND_COUNT],
    /// Per longhand, the group that owns its output.
    output_masks: [u32; LONGHAND_COUNT],
    /// The inherited longhands no group holds, which a child reads from its parent's table alone, in the first
    /// `ungrouped_inherited_count` entries.
    ungrouped_inherited: [u16; LONGHAND_COUNT],
    ungrouped_inherited_count: usize,
}

/// The style groups each longhand reaches, computed as the engine is compiled.
const STYLE_GROUP_MASKS: StyleGroupMasks = StyleGroupMasks::compute();

impl StyleGroupMasks {
    pub(crate) fn get() -> &'static Self {
        &STYLE_GROUP_MASKS
    }

    const fn compute() -> Self {
        use crate::css::property_metadata::property_id::{
            CONTENT_VISIBILITY, DIRECTION, POSITION_ANCHOR, WRITING_MODE,
        };
        use crate::css::property_metadata::{
            FIRST_INHERITED_PROPERTY_ID, FIRST_LONGHAND_PROPERTY_ID, LAST_INHERITED_PROPERTY_ID,
            PROPERTY_STYLE_GROUP_INDICES,
        };
        use crate::css::style_compute::{DIRECTION_COUNT, WRITING_MODE_COUNT};

        const fn group_mask(property: u16) -> u32 {
            match PROPERTY_STYLE_GROUP_INDICES[(property - FIRST_LONGHAND_PROPERTY_ID) as usize] {
                u8::MAX => 0,
                group => 1 << group,
            }
        }
        const fn physical(property: u16, writing_mode: usize, direction: usize) -> u16 {
            physical_longhand(property, writing_mode, direction)
        }
        const fn index(property: u16) -> usize {
            longhand_index_of(property)
        }

        let mut output_masks = [0; LONGHAND_COUNT];
        let mut longhand = 0;
        while longhand < LONGHAND_COUNT {
            let property = FIRST_LONGHAND_PROPERTY_ID + longhand as u16;
            if !is_logical_alias(property) {
                output_masks[longhand] = group_mask(property);
            }
            longhand += 1;
        }
        let mut masks = output_masks;

        // Direction and writing-mode select the physical winners for every logical property group.
        // Those aliases own no payload, so they reach the groups of every physical property they
        // map to.
        let mut logical_mapping_mask = 0;
        let mut longhand = 0;
        while longhand < LONGHAND_COUNT {
            let property = FIRST_LONGHAND_PROPERTY_ID + longhand as u16;
            if is_logical_alias(property) {
                let mut alias_mask = 0;
                let mut writing_mode = 0;
                while writing_mode < WRITING_MODE_COUNT {
                    let mut direction = 0;
                    while direction < DIRECTION_COUNT {
                        alias_mask |= group_mask(physical(property, writing_mode, direction));
                        direction += 1;
                    }
                    writing_mode += 1;
                }
                masks[longhand] |= alias_mask;
                logical_mapping_mask |= alias_mask;
            }
            longhand += 1;
        }
        masks[index(DIRECTION)] |= logical_mapping_mask;
        masks[index(WRITING_MODE)] |= logical_mapping_mask;
        // NB: The box group holds the used containment flags implied by hidden content visibility.
        masks[index(CONTENT_VISIBILITY)] |= 1 << crate::css::table_group_builder::group_index::BOX;
        // NB: The surround group holds whether the element names a default anchor, which layout reads beside the
        //     anchor group's name.
        masks[index(POSITION_ANCHOR)] |= 1 << crate::css::table_group_builder::group_index::SURROUND;

        let mut ungrouped_inherited = [0; LONGHAND_COUNT];
        let mut ungrouped_inherited_count = 0;
        let mut longhand = 0;
        while longhand < LONGHAND_COUNT {
            let property = FIRST_LONGHAND_PROPERTY_ID + longhand as u16;
            if property >= FIRST_INHERITED_PROPERTY_ID
                && property <= LAST_INHERITED_PROPERTY_ID
                && !is_logical_alias(property)
                && (masks[longhand] == 0 || StyleBitField::of(property).is_some())
            {
                ungrouped_inherited[ungrouped_inherited_count] = property;
                ungrouped_inherited_count += 1;
            }
            longhand += 1;
        }
        Self {
            masks,
            output_masks,
            ungrouped_inherited,
            ungrouped_inherited_count,
        }
    }

    /// The inherited longhands no group holds, which a child reads from its parent's table alone.
    pub(crate) fn ungrouped_inherited(&self) -> &[u16] {
        &self.ungrouped_inherited[..self.ungrouped_inherited_count]
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

/// The most computed values one group holds, so that a read names each of a group's values by one bit
/// and its payload handles by another.
pub(crate) const MAX_GROUP_VALUES: usize = 63;

/// The bit of a group's reads that names the values its payload handles hold.
const READS_PAYLOAD_HANDLES: u64 = 1 << MAX_GROUP_VALUES;

/// A longhand property, by its id.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub(crate) struct LonghandID(u16);

impl LonghandID {
    pub(crate) const fn new(property: u16) -> Self {
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
        assert!(property >= FIRST_LONGHAND_PROPERTY_ID && property <= LAST_LONGHAND_PROPERTY_ID);
        Self(property)
    }

    pub(crate) const fn property(self) -> u16 {
        self.0
    }

    const fn index(self) -> usize {
        (self.0 - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID) as usize
    }
}

/// The words of bits a style holds beside its groups.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum BitWord {
    /// The inherited bits beside the inherited set's groups.
    Inherited,
    /// The reset bits in the style's root.
    Reset,
}

/// Declares the fields of the bits a style holds beside its groups: each field's word, its
/// longhand, the CSS enum its codes are of, the code of its initial value, the mapping of keywords
/// to codes, and the shift and width the C++ readers read it at.
macro_rules! style_bit_fields {
    ($($word:ident $field:ident: $property:ident, $enumeration:ident::$initial:ident, $code_of:ident, $shift:ident, $width:ident;)+) => {
        /// One field of the bits a style holds beside its groups.
        #[derive(Clone, Copy, PartialEq, Eq, Debug)]
        pub(crate) enum StyleBitField {
            $($field,)+
        }

        impl StyleBitField {
            const ALL: &[Self] = &[$(Self::$field,)+];

            /// The word of bits holding the field.
            const fn word(self) -> BitWord {
                match self {
                    $(Self::$field => BitWord::$word,)+
                }
            }

            /// The field holding `property`'s value, where the bits hold it.
            pub(crate) const fn of(property: u16) -> Option<Self> {
                match property {
                    $(crate::css::property_metadata::property_id::$property => Some(Self::$field),)+
                    _ => None,
                }
            }

            /// The longhand the field holds.
            const fn property(self) -> u16 {
                match self {
                    $(Self::$field => crate::css::property_metadata::property_id::$property,)+
                }
            }

            const fn shift(self) -> u32 {
                match self {
                    $(Self::$field => crate::css::computed_value_types::$shift,)+
                }
            }

            const fn width(self) -> u32 {
                match self {
                    $(Self::$field => crate::css::computed_value_types::$width,)+
                }
            }

            /// The code of the field's initial value.
            const fn initial(self) -> u8 {
                match self {
                    $(Self::$field => crate::css::css_enums::$enumeration::$initial,)+
                }
            }

            /// The keyword of each code the field holds, by code.
            const fn keywords(self) -> &'static [u16] {
                match self {
                    $(Self::$field => &crate::css::css_enums::$enumeration::KEYWORDS,)+
                }
            }

            /// The code of `keyword` in the field's enum.
            fn code_of(self, keyword: u16) -> Option<u8> {
                match self {
                    $(Self::$field => crate::css::css_enums::$code_of(keyword),)+
                }
            }
        }
    };
}

style_bit_fields! {
    Inherited WritingMode: WRITING_MODE, writing_mode::HORIZONTAL_TB, keyword_to_writing_mode,
        INHERITED_BITS_WRITING_MODE_SHIFT, INHERITED_BITS_WRITING_MODE_WIDTH;
    Inherited Direction: DIRECTION, direction::LTR, keyword_to_direction,
        INHERITED_BITS_DIRECTION_SHIFT, INHERITED_BITS_DIRECTION_WIDTH;
    Inherited Visibility: VISIBILITY, visibility::VISIBLE, keyword_to_visibility,
        INHERITED_BITS_VISIBILITY_SHIFT, INHERITED_BITS_VISIBILITY_WIDTH;
    Inherited TextOrientation: TEXT_ORIENTATION, text_orientation::MIXED, keyword_to_text_orientation,
        INHERITED_BITS_TEXT_ORIENTATION_SHIFT, INHERITED_BITS_TEXT_ORIENTATION_WIDTH;
    Inherited PointerEvents: POINTER_EVENTS, pointer_events::AUTO, keyword_to_pointer_events,
        INHERITED_BITS_POINTER_EVENTS_SHIFT, INHERITED_BITS_POINTER_EVENTS_WIDTH;
    Inherited ListStylePosition: LIST_STYLE_POSITION, list_style_position::OUTSIDE, keyword_to_list_style_position,
        INHERITED_BITS_LIST_STYLE_POSITION_SHIFT, INHERITED_BITS_LIST_STYLE_POSITION_WIDTH;
    Inherited BorderCollapse: BORDER_COLLAPSE, border_collapse::SEPARATE, keyword_to_border_collapse,
        INHERITED_BITS_BORDER_COLLAPSE_SHIFT, INHERITED_BITS_BORDER_COLLAPSE_WIDTH;
    Inherited CaptionSide: CAPTION_SIDE, caption_side::TOP, keyword_to_caption_side,
        INHERITED_BITS_CAPTION_SIDE_SHIFT, INHERITED_BITS_CAPTION_SIDE_WIDTH;
    Inherited EmptyCells: EMPTY_CELLS, empty_cells::SHOW, keyword_to_empty_cells,
        INHERITED_BITS_EMPTY_CELLS_SHIFT, INHERITED_BITS_EMPTY_CELLS_WIDTH;
    Inherited TextAlign: TEXT_ALIGN, text_align::START, keyword_to_text_align,
        INHERITED_BITS_TEXT_ALIGN_SHIFT, INHERITED_BITS_TEXT_ALIGN_WIDTH;
    Inherited WhiteSpaceCollapse: WHITE_SPACE_COLLAPSE, white_space_collapse::COLLAPSE, keyword_to_white_space_collapse,
        INHERITED_BITS_WHITE_SPACE_COLLAPSE_SHIFT, INHERITED_BITS_WHITE_SPACE_COLLAPSE_WIDTH;
    Inherited TextWrapMode: TEXT_WRAP_MODE, text_wrap_mode::WRAP, keyword_to_text_wrap_mode,
        INHERITED_BITS_TEXT_WRAP_MODE_SHIFT, INHERITED_BITS_TEXT_WRAP_MODE_WIDTH;
    Inherited WordBreak: WORD_BREAK, word_break::NORMAL, keyword_to_word_break,
        INHERITED_BITS_WORD_BREAK_SHIFT, INHERITED_BITS_WORD_BREAK_WIDTH;
    Reset Position: POSITION, positioning::STATIC, keyword_to_positioning,
        RESET_BITS_POSITION_SHIFT, RESET_BITS_POSITION_WIDTH;
    Reset Float: FLOAT, float::NONE, keyword_to_float,
        RESET_BITS_FLOAT_SHIFT, RESET_BITS_FLOAT_WIDTH;
    Reset Clear: CLEAR, clear::NONE, keyword_to_clear,
        RESET_BITS_CLEAR_SHIFT, RESET_BITS_CLEAR_WIDTH;
    Reset OverflowX: OVERFLOW_X, overflow::VISIBLE, keyword_to_overflow,
        RESET_BITS_OVERFLOW_X_SHIFT, RESET_BITS_OVERFLOW_X_WIDTH;
    Reset OverflowY: OVERFLOW_Y, overflow::VISIBLE, keyword_to_overflow,
        RESET_BITS_OVERFLOW_Y_SHIFT, RESET_BITS_OVERFLOW_Y_WIDTH;
    Reset BoxSizing: BOX_SIZING, box_sizing::CONTENT_BOX, keyword_to_box_sizing,
        RESET_BITS_BOX_SIZING_SHIFT, RESET_BITS_BOX_SIZING_WIDTH;
    Reset UnicodeBidi: UNICODE_BIDI, unicode_bidi::NORMAL, keyword_to_unicode_bidi,
        RESET_BITS_UNICODE_BIDI_SHIFT, RESET_BITS_UNICODE_BIDI_WIDTH;
    Reset TableLayout: TABLE_LAYOUT, table_layout::AUTO, keyword_to_table_layout,
        RESET_BITS_TABLE_LAYOUT_SHIFT, RESET_BITS_TABLE_LAYOUT_WIDTH;
}

impl StyleBitField {
    const fn from_index(index: u8) -> Self {
        Self::ALL[index as usize]
    }
}

// NB: Each field's codes fit its width, and the fields of a word do not overlap.
const _: () = {
    let mut field = 0;
    let mut taken = [0_u32; 2];
    while field < StyleBitField::ALL.len() {
        let bits = StyleBitField::ALL[field];
        assert!(bits.keywords().len() <= 1 << bits.width());
        let mask = ((1 << bits.width()) - 1) << bits.shift();
        let word = bits.word() as usize;
        assert!(taken[word] & mask == 0);
        taken[word] |= mask;
        field += 1;
    }
};

/// The codes of one word of bits a style holds beside its groups, by field.
macro_rules! impl_bit_word {
    ($bits:ident, $word:ident) => {
        impl crate::css::computed_value_types::$bits {
            /// The bits of the initial values.
            pub(crate) const INITIAL: Self = {
                let mut bits = Self(0);
                let mut field = 0;
                while field < StyleBitField::ALL.len() {
                    let field_at = StyleBitField::ALL[field];
                    if matches!(field_at.word(), BitWord::$word) {
                        bits = bits.with(field_at, field_at.initial());
                    }
                    field += 1;
                }
                bits
            };

            /// The code `field`, a field of these bits, holds.
            pub(crate) const fn get(self, field: StyleBitField) -> u8 {
                assert!(matches!(field.word(), BitWord::$word));
                ((self.0 >> field.shift()) & ((1 << field.width()) - 1)) as u8
            }

            /// These bits, with `code` in `field`, a field of them.
            pub(crate) const fn with(self, field: StyleBitField, code: u8) -> Self {
                assert!(matches!(field.word(), BitWord::$word));
                let mask = ((1 << field.width()) - 1) << field.shift();
                Self((self.0 & !mask) | ((code as u32) << field.shift() & mask))
            }
        }
    };
}

impl_bit_word!(InheritedBits, Inherited);
impl_bit_word!(ResetBits, Reset);

impl crate::css::computed_value_types::InheritedBits {
    pub(crate) fn writing_mode(self) -> u8 {
        self.get(StyleBitField::WritingMode)
    }

    pub(crate) fn direction(self) -> u8 {
        self.get(StyleBitField::Direction)
    }
}

impl crate::css::computed_value_types::InheritedSetValues {
    /// The values of the initial style.
    pub(crate) const INITIAL: Self = Self {
        bits: crate::css::computed_value_types::InheritedBits::INITIAL,
        effective_zoom_bits: 0x3f80_0000,
    };

    /// https://drafts.csswg.org/css-viewport/#effective-zoom
    pub(crate) fn effective_zoom(&self) -> f32 {
        f32::from_bits(self.effective_zoom_bits)
    }
}

impl crate::css::computed_value_types::StyleBits {
    /// The bits of the initial style.
    pub(crate) const INITIAL: Self = Self {
        inherited: InheritedSetValues::INITIAL,
        reset: crate::css::computed_value_types::ResetBits::INITIAL,
    };

    /// The code `field` holds.
    pub(crate) const fn get(&self, field: StyleBitField) -> u8 {
        match field.word() {
            BitWord::Inherited => self.inherited.bits.get(field),
            BitWord::Reset => self.reset.get(field),
        }
    }

    /// These bits, with `code` in `field`.
    pub(crate) const fn with(mut self, field: StyleBitField, code: u8) -> Self {
        match field.word() {
            BitWord::Inherited => self.inherited.bits = self.inherited.bits.with(field, code),
            BitWord::Reset => self.reset = self.reset.with(field, code),
        }
        self
    }

    /// The computed value `field` holds: its keyword, as the shared keyword values hold it.
    pub(crate) fn value(&self, field: StyleBitField) -> &'static RetainedStyleValueData {
        RetainedStyleValueData::shared_keyword(field.keywords()[usize::from(self.get(field))])
    }

    /// These bits, with the code of `value`, a computed keyword of `field`'s enum, in `field`.
    pub(crate) fn with_value(self, field: StyleBitField, value: &RetainedStyleValueData) -> Self {
        match value.optional_data() {
            Some(StyleValueData::Keyword { keyword }) => self.with(
                field,
                field.code_of(*keyword).expect("a computed keyword of the field's enum"),
            ),
            _ => panic!("a computed value held as bits is a keyword"),
        }
    }

    /// The bits of a style whose longhands `value_of` answers, with `effective_zoom`.
    pub(crate) fn of_values<'v>(value_of: impl Fn(u16) -> &'v RetainedStyleValueData, effective_zoom: f32) -> Self {
        let bits = StyleBitField::ALL.iter().fold(Self::INITIAL, |bits, &field| {
            let value = value_of(field.property());
            match value.optional_data() {
                Some(_) => bits.with_value(field, value),
                None => bits,
            }
        });
        Self {
            inherited: InheritedSetValues {
                effective_zoom_bits: effective_zoom.to_bits(),
                ..bits.inherited
            },
            ..bits
        }
    }
}

/// Where one longhand's computed value lives: in the allocation of the computed-value group
/// holding it, at its index among the group's values, in a handle of the group's payload that
/// holds the computed value itself, or in the inherited bits beside the style's inherited groups.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct LonghandPlace {
    group: u8,
    index: u8,
    /// One past the offset of the payload field holding the value, or zero where the group's values
    /// hold it.
    handle: u16,
}

const _: () = assert!(size_of::<LonghandPlace>() == 4);

impl LonghandPlace {
    /// The place of a longhand holding no value of its own.
    const NONE: Self = Self {
        group: u8::MAX,
        index: 0,
        handle: 0,
    };

    /// Whether a build reading `reads` reads the value at this place, a place of a group.
    #[inline]
    fn is_read_by(self, reads: &GroupReads) -> bool {
        let bit = match self.handle {
            0 => 1 << self.index,
            _ => READS_PAYLOAD_HANDLES,
        };
        reads[usize::from(self.group)] & bit != 0
    }

    /// Where this place comes among the places of the groups: by group, then by index, with the
    /// payload handles of a group after its values.
    fn order(self) -> u32 {
        (u32::from(self.group) << 24) | (u32::from(self.index) << 16) | u32::from(self.handle)
    }

    /// The offset of the payload handle holding the value, where one does.
    pub(crate) fn handle_offset(self) -> Option<usize> {
        (self.handle != 0).then(|| usize::from(self.handle) - 1)
    }

    /// The value at this place of `payload`, a live payload of the group whose values are
    /// `values`.
    ///
    /// # Safety
    /// `payload` must be a live payload of the place's group, holding `values`, live for `'a`.
    unsafe fn value_in(self, payload: *const c_void, values: &[RetainedStyleValueData]) -> &RetainedStyleValueData {
        let Some(offset) = self.handle_offset() else {
            return &values[self.index()];
        };
        if self.index != u8::MAX
            && let Some(StyleValueData::ValueList { values, .. }) = values[TYPED_VALUES_SLOT].optional_data()
        {
            return &values.as_slice()[self.index()];
        }
        // SAFETY: As the caller guarantees.
        unsafe { self.value_in_payload(payload, offset) }
    }

    /// The value the payload holds at this place, a place of a handle or of a typed field at `offset`.
    ///
    /// # Safety
    /// `payload` must be a live payload of the place's group, live for `'a`.
    unsafe fn value_in_payload<'a>(self, payload: *const c_void, offset: usize) -> &'a RetainedStyleValueData {
        // SAFETY: As the caller guarantees: the field at the offset is of the type the group says,
        //         and a handle is a single retained pointer laid out as a `RetainedStyleValueData` is.
        unsafe {
            let field = payload.byte_add(offset);
            if self.index == u8::MAX {
                return &*field.cast::<RetainedStyleValueData>();
            }
            match usize::from(self.group) {
                group_index::SIZING => (*field.cast::<ComputedSize>()).computed_value(),
                _ => (*field.cast::<ComputedLengthPercentageOrAuto>()).computed_value(),
            }
        }
    }

    /// Puts `value` in the handle or typed field at this place of `payload`, which only the caller
    /// references.
    ///
    /// # Safety
    /// `payload` must be a live payload of the place's group.
    unsafe fn set_in_payload(self, payload: *mut c_void, value: &RetainedStyleValueData) {
        let offset = self.handle_offset().expect("a payload field place");
        // SAFETY: As in `value_in_payload`.
        unsafe {
            let field = payload.byte_add(offset);
            if self.index == u8::MAX {
                *field.cast::<RetainedStyleValueData>() = value.clone();
                return;
            }
            match usize::from(self.group) {
                group_index::SIZING => *field.cast::<ComputedSize>() = ComputedSize::from_data(value),
                _ => {
                    // NB: Insets come first among the group's longhands.
                    *field.cast::<ComputedLengthPercentageOrAuto>() =
                        ComputedLengthPercentageOrAuto::from_length_box_data(value, self.index < 4);
                }
            }
        }
    }

    /// The place of a longhand the inherited bits hold, in `field`.
    const fn bits(field: StyleBitField) -> Self {
        Self {
            group: Self::BITS_GROUP,
            index: field as u8,
            handle: 0,
        }
    }

    /// The `group` of a place in the inherited bits.
    const BITS_GROUP: u8 = u8::MAX - 1;

    /// The field of the inherited bits holding the value, where those hold it.
    pub(crate) fn bit_field(self) -> Option<StyleBitField> {
        (self.group == Self::BITS_GROUP).then(|| StyleBitField::from_index(self.index))
    }

    /// The group holding the value, which a place in the inherited bits has none of.
    pub(crate) fn group(self) -> usize {
        assert!(self.group != Self::BITS_GROUP, "the inherited bits are in no group");
        usize::from(self.group)
    }

    pub(crate) fn index(self) -> usize {
        usize::from(self.index)
    }
}

/// The values a group's build reads, of each group, one bit per value index.
pub(crate) type GroupReads = [u64; group_index::COUNT];

/// The slot of the values of a group whose longhands typed fields hold, for the computed values of an
/// element some of whose values its typed fields do not tell.
const TYPED_VALUES_SLOT: usize = 0;

/// The groups whose longhands are all held by typed fields of their payload, which hold each value,
/// or leave a keyword to tell by their kind.
const GROUPS_OF_TYPED_LONGHANDS: u32 = (1 << group_index::SIZING) | (1 << group_index::SURROUND);

/// The longhands of those groups, by the offset of the typed field and the position of the longhand
/// among its group's.
const PAYLOAD_TYPED_LONGHANDS: [(u16, usize, usize, u8); 18] = {
    use crate::css::property_metadata::property_id as p;
    use std::mem::offset_of;
    let sizing = group_index::SIZING;
    let surround = group_index::SURROUND;
    let inset = offset_of!(SurroundValues, inset);
    let margin = offset_of!(SurroundValues, margin);
    let padding = offset_of!(SurroundValues, padding);
    let top = offset_of!(ComputedLengthBox, top);
    let right = offset_of!(ComputedLengthBox, right);
    let bottom = offset_of!(ComputedLengthBox, bottom);
    let left = offset_of!(ComputedLengthBox, left);
    [
        (p::WIDTH, sizing, offset_of!(SizingValues, width), 0),
        (p::MIN_WIDTH, sizing, offset_of!(SizingValues, min_width), 1),
        (p::MAX_WIDTH, sizing, offset_of!(SizingValues, max_width), 2),
        (p::HEIGHT, sizing, offset_of!(SizingValues, height), 3),
        (p::MIN_HEIGHT, sizing, offset_of!(SizingValues, min_height), 4),
        (p::MAX_HEIGHT, sizing, offset_of!(SizingValues, max_height), 5),
        (p::TOP, surround, inset + top, 0),
        (p::RIGHT, surround, inset + right, 1),
        (p::BOTTOM, surround, inset + bottom, 2),
        (p::LEFT, surround, inset + left, 3),
        (p::MARGIN_TOP, surround, margin + top, 4),
        (p::MARGIN_RIGHT, surround, margin + right, 5),
        (p::MARGIN_BOTTOM, surround, margin + bottom, 6),
        (p::MARGIN_LEFT, surround, margin + left, 7),
        (p::PADDING_TOP, surround, padding + top, 8),
        (p::PADDING_RIGHT, surround, padding + right, 9),
        (p::PADDING_BOTTOM, surround, padding + bottom, 10),
        (p::PADDING_LEFT, surround, padding + left, 11),
    ]
};

/// The longhands whose computed value a handle of their group's payload holds as it is, by the
/// handle's offset: the group's values keep no second copy of them.
const PAYLOAD_HANDLE_LONGHANDS: [(u16, usize, usize); 53] = {
    use crate::css::computed_value_types::{BackgroundValues, EffectsValues, FontValues, MaskValues, SVGResetValues};
    use crate::css::property_metadata::property_id as p;
    use std::mem::offset_of;
    let font = group_index::FONT;
    let background = group_index::BACKGROUND;
    let mask = group_index::MASK;
    let effects = group_index::EFFECTS;
    let svg_reset = group_index::SVG_RESET;
    [
        (
            p::BACKGROUND_IMAGE,
            background,
            offset_of!(BackgroundValues, background_image),
        ),
        (
            p::BACKGROUND_ATTACHMENT,
            background,
            offset_of!(BackgroundValues, background_attachment),
        ),
        (
            p::BACKGROUND_BLEND_MODE,
            background,
            offset_of!(BackgroundValues, background_blend_mode),
        ),
        (
            p::BACKGROUND_CLIP,
            background,
            offset_of!(BackgroundValues, background_clip),
        ),
        (
            p::BACKGROUND_ORIGIN,
            background,
            offset_of!(BackgroundValues, background_origin),
        ),
        (
            p::BACKGROUND_POSITION_X,
            background,
            offset_of!(BackgroundValues, background_position_x),
        ),
        (
            p::BACKGROUND_POSITION_Y,
            background,
            offset_of!(BackgroundValues, background_position_y),
        ),
        (
            p::BACKGROUND_REPEAT,
            background,
            offset_of!(BackgroundValues, background_repeat),
        ),
        (
            p::BACKGROUND_SIZE,
            background,
            offset_of!(BackgroundValues, background_size),
        ),
        (p::MASK_IMAGE, mask, offset_of!(MaskValues, mask_image)),
        (p::MASK_TYPE, mask, offset_of!(MaskValues, mask_type)),
        (p::CLIP_PATH, mask, offset_of!(MaskValues, clip_path)),
        (p::MASK_MODE, mask, offset_of!(MaskValues, mask_mode)),
        (p::MASK_REPEAT, mask, offset_of!(MaskValues, mask_repeat)),
        (p::MASK_POSITION, mask, offset_of!(MaskValues, mask_position)),
        (p::MASK_CLIP, mask, offset_of!(MaskValues, mask_clip)),
        (p::MASK_ORIGIN, mask, offset_of!(MaskValues, mask_origin)),
        (p::MASK_SIZE, mask, offset_of!(MaskValues, mask_size)),
        (p::MASK_COMPOSITE, mask, offset_of!(MaskValues, mask_composite)),
        (p::OPACITY, effects, offset_of!(EffectsValues, opacity_style_value)),
        (p::FILTER, effects, offset_of!(EffectsValues, filter_style_value)),
        (
            p::BACKDROP_FILTER,
            effects,
            offset_of!(EffectsValues, backdrop_filter_style_value),
        ),
        (
            p::MIX_BLEND_MODE,
            effects,
            offset_of!(EffectsValues, mix_blend_mode_style_value),
        ),
        (p::ISOLATION, effects, offset_of!(EffectsValues, isolation_style_value)),
        (
            p::BOX_SHADOW,
            effects,
            offset_of!(EffectsValues, box_shadow_style_value),
        ),
        (p::CLIP, effects, offset_of!(EffectsValues, clip_style_value)),
        (p::CX, svg_reset, offset_of!(SVGResetValues, cx)),
        (p::CY, svg_reset, offset_of!(SVGResetValues, cy)),
        (p::D, svg_reset, offset_of!(SVGResetValues, d)),
        (p::R, svg_reset, offset_of!(SVGResetValues, r)),
        (p::X, svg_reset, offset_of!(SVGResetValues, x)),
        (p::Y, svg_reset, offset_of!(SVGResetValues, y)),
        (p::FONT_FAMILY, font, offset_of!(FontValues, font_family)),
        (p::FONT_STYLE, font, offset_of!(FontValues, font_style)),
        (
            p::FONT_OPTICAL_SIZING,
            font,
            offset_of!(FontValues, font_optical_sizing),
        ),
        (
            p::FONT_SYNTHESIS_WEIGHT,
            font,
            offset_of!(FontValues, font_synthesis_weight),
        ),
        (
            p::FONT_SYNTHESIS_STYLE,
            font,
            offset_of!(FontValues, font_synthesis_style),
        ),
        (
            p::FONT_FEATURE_SETTINGS,
            font,
            offset_of!(FontValues, font_feature_settings),
        ),
        (p::FONT_KERNING, font, offset_of!(FontValues, font_kerning)),
        (
            p::FONT_LANGUAGE_OVERRIDE,
            font,
            offset_of!(FontValues, font_language_override),
        ),
        (
            p::FONT_VARIANT_ALTERNATES,
            font,
            offset_of!(FontValues, font_variant_alternates),
        ),
        (p::FONT_VARIANT_CAPS, font, offset_of!(FontValues, font_variant_caps)),
        (
            p::FONT_VARIANT_EAST_ASIAN,
            font,
            offset_of!(FontValues, font_variant_east_asian),
        ),
        (
            p::FONT_VARIANT_LIGATURES,
            font,
            offset_of!(FontValues, font_variant_ligatures),
        ),
        (
            p::FONT_VARIANT_NUMERIC,
            font,
            offset_of!(FontValues, font_variant_numeric),
        ),
        (
            p::FONT_VARIANT_POSITION,
            font,
            offset_of!(FontValues, font_variant_position),
        ),
        (
            p::FONT_VARIATION_SETTINGS,
            font,
            offset_of!(FontValues, font_variation_settings),
        ),
        (p::TEXT_RENDERING, font, offset_of!(FontValues, text_rendering)),
        (p::LINE_HEIGHT, font, offset_of!(FontValues, line_height)),
        (p::MATH_SHIFT, font, offset_of!(FontValues, math_shift_value)),
        (p::MATH_STYLE, font, offset_of!(FontValues, math_style_value)),
        (p::MATH_DEPTH, font, offset_of!(FontValues, math_depth_value)),
        (p::FONT_SIZE, font, offset_of!(FontValues, font_size_value)),
    ]
};

const _: () = assert!(
    size_of::<crate::css::computed_value_types::ComputedStyleValueHandle>() == size_of::<RetainedStyleValueData>()
);

/// Where each longhand's computed value lives among an element's groups, and which of them each
/// group's build reads, computed as the engine is compiled.
///
/// A longhand's value lives in the group owning its output. A logical alias's lives in the group
/// of the physical longhand it maps to in a horizontal, left-to-right flow, and the value of a
/// longhand no group owns lives in the font group where it inherits, as the font longhands do, or
/// in the box group where it does not. A group's values then inherit or not as its payload does.
pub(crate) struct GroupValueLayout {
    places: [LonghandPlace; LONGHAND_COUNT],
    counts: [u8; group_index::COUNT],
    /// Per group, the values its build reads: those of the longhands whose changes may change it,
    /// but for logical aliases, and all of its own. The longhands whose changes feed no group are
    /// read by the font group's build.
    reads: [GroupReads; group_index::COUNT],
    /// Per group, the groups its build reads values of, one bit per group.
    read_groups: [u32; group_index::COUNT],
    /// Per group, the values its payload is looked for by: those it reads, but for a background
    /// color, which the cache patches into the payload it finds.
    key_reads: [GroupReads; group_index::COUNT],
}

const GROUP_VALUE_LAYOUT: GroupValueLayout = GroupValueLayout::compute();

/// Per group, the longhands whose values it holds, one bit per longhand from the first: all but those
/// in the inherited bits and the logical aliases, which map elsewhere.
const GROUP_LONGHANDS: [[u64; crate::css::property_metadata::LONGHAND_WORD_COUNT]; group_index::COUNT] = {
    let mut groups = [[0; crate::css::property_metadata::LONGHAND_WORD_COUNT]; group_index::COUNT];
    let mut longhand = 0;
    while longhand < LONGHAND_COUNT {
        let property = crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID + longhand as u16;
        let group = GROUP_VALUE_LAYOUT.places[longhand].group;
        if !is_logical_alias(property) && group != LonghandPlace::NONE.group && group != LonghandPlace::BITS_GROUP {
            groups[group as usize][longhand / 64] |= 1 << (longhand % 64);
        }
        longhand += 1;
    }
    groups
};

impl GroupValueLayout {
    pub(crate) fn get() -> &'static Self {
        &GROUP_VALUE_LAYOUT
    }

    /// The longhands whose values each group holds, one bit per longhand from the first.
    pub(crate) fn group_longhands()
    -> &'static [[u64; crate::css::property_metadata::LONGHAND_WORD_COUNT]; group_index::COUNT] {
        &GROUP_LONGHANDS
    }

    const fn compute() -> Self {
        use crate::css::property_metadata::{
            FIRST_INHERITED_PROPERTY_ID, FIRST_LONGHAND_PROPERTY_ID, LAST_INHERITED_PROPERTY_ID,
        };
        const fn home(property: u16) -> usize {
            let masks = &STYLE_GROUP_MASKS;
            let output = masks.output_masks[longhand_index_of(property)];
            if output != 0 {
                return output.trailing_zeros() as usize;
            }
            if is_logical_alias(property) {
                let physical = masks.output_masks[longhand_index_of(physical_longhand(property, 0, 0))];
                if physical != 0 {
                    return physical.trailing_zeros() as usize;
                }
            }
            match property >= FIRST_INHERITED_PROPERTY_ID && property <= LAST_INHERITED_PROPERTY_ID {
                true => group_index::FONT,
                false => group_index::BOX,
            }
        }

        let mut places = [LonghandPlace {
            group: 0,
            index: 0,
            handle: 0,
        }; LONGHAND_COUNT];
        let mut counts = [0_u8; group_index::COUNT];
        counts[group_index::SIZING] = 1;
        counts[group_index::SURROUND] = 1;
        let mut longhand = 0;
        while longhand < LONGHAND_COUNT {
            let property = FIRST_LONGHAND_PROPERTY_ID + longhand as u16;
            if maps_in_every_flow(property) {
                places[longhand] = LonghandPlace::NONE;
                longhand += 1;
                continue;
            }
            if let Some(field) = StyleBitField::of(property) {
                places[longhand] = LonghandPlace::bits(field);
                longhand += 1;
                continue;
            }
            let group = home(property);
            let mut handle = 0;
            while handle < PAYLOAD_HANDLE_LONGHANDS.len() {
                let (handle_property, handle_group, offset) = PAYLOAD_HANDLE_LONGHANDS[handle];
                if handle_property == property {
                    assert!(handle_group == group, "a payload handle holds a longhand of its group");
                    assert!(offset < u16::MAX as usize);
                    break;
                }
                handle += 1;
            }
            if handle < PAYLOAD_HANDLE_LONGHANDS.len() {
                places[longhand] = LonghandPlace {
                    group: group as u8,
                    index: u8::MAX,
                    handle: PAYLOAD_HANDLE_LONGHANDS[handle].2 as u16 + 1,
                };
                longhand += 1;
                continue;
            }
            let mut typed = 0;
            while typed < PAYLOAD_TYPED_LONGHANDS.len() {
                if PAYLOAD_TYPED_LONGHANDS[typed].0 == property {
                    break;
                }
                typed += 1;
            }
            if typed < PAYLOAD_TYPED_LONGHANDS.len() {
                let (_, typed_group, offset, position) = PAYLOAD_TYPED_LONGHANDS[typed];
                assert!(typed_group == group, "a typed field holds a longhand of its group");
                places[longhand] = LonghandPlace {
                    group: group as u8,
                    index: position,
                    handle: offset as u16 + 1,
                };
                longhand += 1;
                continue;
            }
            let index = counts[group] as usize;
            assert!(
                index < MAX_GROUP_VALUES,
                "a group holds no more values than a read names"
            );
            places[longhand] = LonghandPlace {
                group: group as u8,
                index: index as u8,
                handle: 0,
            };
            counts[group] += 1;
            longhand += 1;
        }

        let mut reads = [[0_u64; group_index::COUNT]; group_index::COUNT];
        let mut longhand = 0;
        while longhand < LONGHAND_COUNT {
            let property = FIRST_LONGHAND_PROPERTY_ID + longhand as u16;
            let place = places[longhand];
            // NB: No group's build reads the inherited bits: they are no group's values.
            if place.group == LonghandPlace::NONE.group || place.group == LonghandPlace::BITS_GROUP {
                longhand += 1;
                continue;
            }
            // NB: A build reading a value a payload handle holds reads all the group's handles.
            if place.handle != 0 {
                longhand += 1;
                continue;
            }
            let read_by = match STYLE_GROUP_MASKS.masks[longhand] {
                _ if is_logical_alias(property) => 0,
                0 => 1 << group_index::FONT,
                groups => groups,
            } | (1 << place.group);
            let mut group = 0;
            while group < group_index::COUNT {
                if read_by & (1 << group) != 0 {
                    reads[group][place.group as usize] |= 1 << place.index;
                }
                group += 1;
            }
            longhand += 1;
        }
        let mut typed = 0;
        while typed < PAYLOAD_TYPED_LONGHANDS.len() {
            let (property, group, _, _) = PAYLOAD_TYPED_LONGHANDS[typed];
            let read_by = STYLE_GROUP_MASKS.masks[longhand_index_of(property)] | (1 << group);
            let mut reader = 0;
            while reader < group_index::COUNT {
                if read_by & (1 << reader) != 0 {
                    reads[reader][group] |= READS_PAYLOAD_HANDLES;
                }
                reader += 1;
            }
            typed += 1;
        }
        let mut handle = 0;
        while handle < PAYLOAD_HANDLE_LONGHANDS.len() {
            let (property, group, _) = PAYLOAD_HANDLE_LONGHANDS[handle];
            let read_by = match STYLE_GROUP_MASKS.masks[longhand_index_of(property)] {
                0 => 1 << group_index::FONT,
                groups => groups,
            } | (1 << group);
            let mut reader = 0;
            while reader < group_index::COUNT {
                if read_by & (1 << reader) != 0 {
                    reads[reader][group] |= READS_PAYLOAD_HANDLES;
                }
                reader += 1;
            }
            handle += 1;
        }
        let mut read_groups = [0_u32; group_index::COUNT];
        let mut reader = 0;
        while reader < group_index::COUNT {
            let mut group = 0;
            while group < group_index::COUNT {
                if reads[reader][group] != 0 {
                    read_groups[reader] |= 1 << group;
                }
                group += 1;
            }
            reader += 1;
        }
        let mut key_reads = reads;
        let background_color = places[longhand_index_of(crate::css::property_metadata::property_id::BACKGROUND_COLOR)];
        key_reads[group_index::BACKGROUND][background_color.group as usize] &= !(1 << background_color.index);
        Self {
            places,
            counts,
            reads,
            read_groups,
            key_reads,
        }
    }

    /// The longhands that inherit by default.
    pub(crate) fn inherited_longhands() -> impl Iterator<Item = u16> {
        use crate::css::property_metadata::{FIRST_INHERITED_PROPERTY_ID, LAST_INHERITED_PROPERTY_ID};
        (FIRST_INHERITED_PROPERTY_ID..=LAST_INHERITED_PROPERTY_ID).filter(|&property| !is_logical_alias(property))
    }

    /// Where the computed value of `longhand` lives. A logical alias mapping to a physical
    /// longhand in every flow holds no value of its own: see [`Self::resolved`].
    pub(crate) fn place(&self, longhand: LonghandID) -> LonghandPlace {
        self.places[longhand.index()]
    }

    /// Whether `longhand` holds a value of its own, which a logical alias mapping to a physical
    /// longhand in every flow does not.
    pub(crate) fn holds_value(&self, longhand: LonghandID) -> bool {
        self.places[longhand.index()] != LonghandPlace::NONE
    }

    /// The longhand whose value is the computed value of `property`, among values `value_of`
    /// answers: `property` itself, or for a logical alias holding no value of its own, the
    /// physical longhand it maps to in the flow of the writing mode and direction those values
    /// hold. https://drafts.csswg.org/css-logical/#box
    /// NB: A logical alias computes as the physical longhand it maps to, so its value is that
    ///     longhand's, held once.
    pub(crate) fn resolved<'v>(&self, property: u16, value_of: impl Fn(u16) -> &'v RetainedStyleValueData) -> u16 {
        use crate::css::property_metadata::property_id;
        if self.holds_value(LonghandID::new(property)) {
            return property;
        }
        let (writing_mode, direction) = match (
            value_of(property_id::WRITING_MODE).optional_data(),
            value_of(property_id::DIRECTION).optional_data(),
        ) {
            (Some(_), Some(_)) => crate::css::style_compute::writing_mode_and_direction_of(
                Some(value_of(property_id::WRITING_MODE)),
                Some(value_of(property_id::DIRECTION)),
            ),
            _ => (0, 0),
        };
        crate::css::style_compute::map_logical_alias_to_physical(property, writing_mode, direction)
    }

    fn place_of(&self, property: u16) -> LonghandPlace {
        self.place(LonghandID::new(property))
    }

    /// The values the build of `group` may read.
    pub(crate) fn reads_of(&self, group: usize) -> &GroupReads {
        &self.reads[group]
    }

    /// The groups the build of `group` reads values of.
    pub(crate) fn read_groups_of(&self, group: usize) -> u32 {
        self.read_groups[group]
    }

    /// The values the payload of `group` is looked for by.
    pub(crate) fn key_reads_of(&self, group: usize) -> &GroupReads {
        &self.key_reads[group]
    }

    /// Whether a build reading `reads` reads the value at `place`.
    pub(crate) fn reads(reads: &GroupReads, place: LonghandPlace) -> bool {
        place != LonghandPlace::NONE
            && place.bit_field().is_none()
            && match place.handle_offset() {
                Some(_) => reads[place.group()] & READS_PAYLOAD_HANDLES != 0,
                None => reads[place.group()] & (1 << place.index) != 0,
            }
    }

    /// The places of the payload handles holding values of `group`.
    pub(crate) fn handles_of(group: usize) -> impl Iterator<Item = (u16, LonghandPlace)> {
        PAYLOAD_HANDLE_LONGHANDS
            .iter()
            .filter(move |(_, handle_group, _)| *handle_group == group)
            .map(|&(property, _, _)| property)
            .chain(
                PAYLOAD_TYPED_LONGHANDS
                    .iter()
                    .filter(move |(_, typed_group, _, _)| *typed_group == group)
                    .map(|&(property, _, _, _)| property),
            )
            .map(|property| (property, GROUP_VALUE_LAYOUT.place_of(property)))
    }
}

/// The recorded specified value of a longhand whose computed value depends on what the element
/// inherits.
#[derive(Clone)]
pub(crate) struct InheritanceDependentValue {
    property: u16,
    value: RetainedStyleValueData,
    /// Whether a group build reads the value in place of the computed value, as it does for a
    /// currentcolor-dependent value of a longhand a drive did not evaluate.
    read_by_groups: bool,
}

impl crate::heap_size::HeapSize for InheritanceDependentValue {
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.value.heap_size(sizer)
    }
}

/// What an element's groups are built from beside their computed values: the recorded specified
/// values of the longhands whose computed values depend on what the element inherits, its zoom,
/// its display before the box type transformation, and its color scheme.
#[derive(Clone)]
pub(crate) struct GroupValueFacts {
    inheritance_dependent_values: Option<std::sync::Arc<[InheritanceDependentValue]>>,
    effective_zoom: f32,
    display_before_box_type_transformation: u32,
    effective_color_scheme: i8,
    /// https://drafts.csswg.org/css-ui/#appearance-disabling-properties
    /// Whether an author declaration disables the element's native appearance.
    native_appearance_disabled: bool,
    /// What of the recorded specified values a group build reads in place of computed values, by
    /// [`read_in_place_bits_of`], so that a build of another group mostly need not look through them.
    read_in_place_bits: u16,
    /// The groups holding colors resolved against `currentcolor`, through the recorded specified
    /// values or the inherited values that read it.
    current_color_groups: u32,
}

/// The bits standing for `groups` among the recorded values a group build reads in place: those of
/// their indices, each group sharing its bit with any other sixteen indices away.
fn read_in_place_bits_of(groups: u32) -> u16 {
    (groups as u16) | ((groups >> u16::BITS) as u16)
}

/// The bit standing for `property` among the recorded values a group build reads in place: that of
/// its group, or one beyond the groups for the longhands no group holds.
fn read_in_place_bit(property: u16) -> u16 {
    let place = GROUP_VALUE_LAYOUT.place_of(property);
    read_in_place_bits_of(match place == LonghandPlace::NONE || place.bit_field().is_some() {
        true => 1 << group_index::COUNT,
        false => 1 << place.group(),
    })
}

impl crate::heap_size::HeapSize for GroupValueFacts {
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.inheritance_dependent_values.heap_size(sizer)
    }
}

impl GroupValueFacts {
    /// The facts of no values: no recorded specified values, no zoom, no display before the box
    /// type transformation and no color scheme.
    pub(crate) fn empty() -> Self {
        Self {
            inheritance_dependent_values: None,
            effective_zoom: 1.0,
            display_before_box_type_transformation: 0,
            effective_color_scheme: -1,
            native_appearance_disabled: false,
            read_in_place_bits: 0,
            current_color_groups: 0,
        }
    }

    /// The groups holding colors resolved against `currentcolor`.
    pub(crate) fn current_color_groups(&self) -> u32 {
        self.current_color_groups
    }

    /// The recorded specified values of the longhands whose computed values depend on what the
    /// element inherits.
    pub(crate) fn inheritance_dependent_values(&self) -> impl Iterator<Item = (u16, &RetainedStyleValueData)> + Clone {
        self.inheritance_dependent_values
            .iter()
            .flat_map(|values| values.iter())
            .map(|held| (held.property, &held.value))
    }

    /// The recorded value a group build reads for `property` in place of its computed value: its
    /// currentcolor-dependent specified value where a drive did not evaluate it.
    pub(crate) fn value_read_in_place(&self, property: u16) -> Option<&RetainedStyleValueData> {
        if self.read_in_place_bits & read_in_place_bit(property) == 0 {
            return None;
        }
        self.inheritance_dependent_values
            .as_deref()?
            .iter()
            .find(|held| held.property == property && held.read_by_groups)
            .map(|held| &held.value)
    }

    /// The recorded values a group build reading `reads`, the values of the groups `read_groups`
    /// names, reads in place of computed values.
    pub(crate) fn values_read_in_place<'a>(
        &'a self,
        reads: &'a GroupReads,
        read_groups: u32,
    ) -> impl Iterator<Item = (u16, &'a RetainedStyleValueData)> + 'a {
        self.inheritance_dependent_values
            .iter()
            .filter(move |_| self.read_in_place_bits & read_in_place_bits_of(read_groups) != 0)
            .flat_map(|values| values.iter())
            .filter(move |held| {
                held.read_by_groups && GroupValueLayout::reads(reads, GROUP_VALUE_LAYOUT.place_of(held.property))
            })
            .map(|held| (held.property, &held.value))
    }

    pub(crate) fn effective_zoom(&self) -> f32 {
        self.effective_zoom
    }

    pub(crate) fn display_before_box_type_transformation(&self) -> u32 {
        self.display_before_box_type_transformation
    }

    pub(crate) fn effective_color_scheme(&self) -> i16 {
        i16::from(self.effective_color_scheme)
    }

    pub(crate) fn native_appearance_disabled(&self) -> bool {
        self.native_appearance_disabled
    }

    /// Whether these and `other` publish the same recorded specified values, zoom, box display
    /// and color scheme.
    pub(crate) fn publication_equals(&self, other: &Self) -> bool {
        use crate::css::style_value::retained_style_values_equal;
        let (mine, theirs) = (
            self.inheritance_dependent_values.as_deref().unwrap_or_default(),
            other.inheritance_dependent_values.as_deref().unwrap_or_default(),
        );
        let same_recorded_values = std::ptr::eq(mine, theirs);
        self.effective_zoom.to_bits() == other.effective_zoom.to_bits()
            && self.display_before_box_type_transformation == other.display_before_box_type_transformation
            && self.effective_color_scheme == other.effective_color_scheme
            && self.native_appearance_disabled == other.native_appearance_disabled
            && (same_recorded_values
                || (mine.len() == theirs.len()
                    && mine.iter().all(|held| {
                        theirs
                            .iter()
                            .find(|other| other.property == held.property)
                            .is_some_and(|other| retained_style_values_equal(&held.value, &other.value))
                    })))
    }

    /// What these and `other` hold differently, if anything.
    pub(crate) fn difference(&self, other: &Self) -> Option<String> {
        let recorded = |facts: &Self| {
            let mut values: Vec<_> = facts
                .inheritance_dependent_values
                .iter()
                .flat_map(|values| values.iter())
                .map(|held| (held.property, held.value.pointer() as usize, held.read_by_groups))
                .collect();
            values.sort_unstable();
            values
        };
        if recorded(self) != recorded(other) {
            return Some("the recorded specified values".into());
        }
        if self.effective_zoom.to_bits() != other.effective_zoom.to_bits()
            || self.display_before_box_type_transformation != other.display_before_box_type_transformation
            || self.effective_color_scheme != other.effective_color_scheme
            || self.native_appearance_disabled != other.native_appearance_disabled
        {
            return Some("the zoom, box display, color scheme or native appearance".into());
        }
        None
    }
}

/// The groups an element's values start from: those of its parent's style or of the initial
/// style, with what those were built from beside their values.
#[derive(Clone, Copy)]
pub(crate) struct GroupSeed<'a> {
    groups: &'a StyleGroups,
    facts: &'a GroupValueFacts,
}

impl<'a> GroupSeed<'a> {
    pub(crate) fn new(groups: &'a StyleGroups, facts: &'a GroupValueFacts) -> Self {
        Self { groups, facts }
    }

    fn group(&self, group: usize) -> GroupPayloadRef<'a> {
        self.groups.group(StyleGroupIndex::new(group))
    }

    fn style_bits(&self) -> StyleBits {
        self.groups.style_bits()
    }

    /// The color the groups resolve `currentcolor` to.
    pub(crate) fn current_color(&self) -> u32 {
        self.groups.current_color()
    }
}

/// One payload of each group holding the group's defaults and no values: what a computation
/// starting from nothing starts from. The process style holds them.
pub(crate) struct DefaultGroups(StyleGroups);

impl Default for DefaultGroups {
    fn default() -> Self {
        // SAFETY: Each payload is a default-constructed payload of its group with one reference,
        //         which the groups take over.
        Self(unsafe {
            StyleGroups::adopt(
                std::array::from_fn(|group| {
                    let payload = allocate_payload(group, 1);
                    default_construct(group, payload);
                    payload.cast_const()
                }),
                StyleBits::INITIAL,
            )
        })
    }
}

impl DefaultGroups {
    pub(crate) fn groups(&self) -> &StyleGroups {
        &self.0
    }
}

/// Calls `compute` with a seed of `defaults`, which hold no values, and the facts of no values:
/// what a computation starting from nothing starts from.
pub(crate) fn with_default_seed<R>(defaults: &DefaultGroups, compute: impl FnOnce(GroupSeed<'_>) -> R) -> R {
    let facts = GroupValueFacts::empty();
    compute(GroupSeed::new(defaults.groups(), &facts))
}

/// The groups an element's values start from, one of each index: its parent's or the initial
/// style's. Written values stay beside these until the group's allocation takes them, as a style
/// struct stays borrowed until it is first mutated.
#[derive(Clone)]
struct SeedGroups<'a> {
    pointers: [*const c_void; group_index::COUNT],
    styles: std::marker::PhantomData<&'a StyleGroups>,
}

// SAFETY: As for `StyleGroups`.
unsafe impl Send for SeedGroups<'_> {}
unsafe impl Sync for SeedGroups<'_> {}

/// A group, with a reference to it.
pub(crate) struct HeldGroup {
    group: usize,
    payload: *const c_void,
}

// SAFETY: As for `StyleGroups`.
unsafe impl Send for HeldGroup {}
unsafe impl Sync for HeldGroup {}

impl HeldGroup {
    /// The payload, whose reference goes to the caller.
    pub(crate) fn into_retained(self) -> *const c_void {
        std::mem::ManuallyDrop::new(self).payload
    }

    /// The group `payload`, a payload of the group at `group`, taking over the caller's
    /// reference to it.
    ///
    /// # Safety
    /// `payload` must be a live payload of the group, with a reference the caller hands over.
    pub(crate) unsafe fn adopt(group: usize, payload: *const c_void) -> Self {
        assert!(!payload.is_null());
        Self { group, payload }
    }

    pub(crate) fn as_ptr(&self) -> *const c_void {
        self.payload
    }

    /// A new group holding a copy of the payload of `base` and the values `values` yields.
    fn new_copy<'v>(base: GroupPayloadRef<'_>, values: impl Iterator<Item = &'v RetainedStyleValueData>) -> Self {
        let payload = allocate_payload(base.group_index, 1);
        // SAFETY: The payload was just allocated for the group, which `base` is a live payload of,
        //         and nothing else references it yet.
        unsafe {
            copy_construct(base.group_index, payload, base.as_ptr());
            for (slot, value) in values_of_mut(base.group_index, payload).iter_mut().zip(values) {
                *slot = value.clone();
            }
        }
        Self {
            group: base.group_index,
            payload: payload.cast_const(),
        }
    }

    /// Puts `value` in the payload field at `place`, where this is the only reference to the group.
    fn set_payload_field(&mut self, place: LonghandPlace, value: &RetainedStyleValueData) {
        assert_eq!(
            refcount_of(self.payload, payload_align(self.group)).load(Ordering::Acquire),
            1
        );
        // SAFETY: The group is live, and this is its only reference.
        unsafe { place.set_in_payload(self.payload.cast_mut(), value) };
    }
}

impl Drop for HeldGroup {
    fn drop(&mut self) {
        release_group_payload(self.group, self.payload);
    }
}

impl<'a> SeedGroups<'a> {
    /// The value at `place`.
    fn value(&self, place: LonghandPlace) -> &RetainedStyleValueData {
        let group = place.group();
        // SAFETY: The groups are live while these are, and each holds the values of its index.
        unsafe { place.value_in(self.pointers[group], group_values(group, self.pointers[group])) }
    }

    /// The values `group` holds.
    fn values(&self, group: usize) -> &[RetainedStyleValueData] {
        // SAFETY: As for `value`.
        unsafe { group_values(group, self.pointers[group]) }
    }

    /// These groups, but with `seed` as the group at `group`.
    fn replace(&mut self, group: usize, seed: GroupPayloadRef<'a>) {
        self.pointers[group] = seed.as_ptr();
    }

    /// The group at `group`, with another reference to it.
    fn held(&self, group: usize) -> HeldGroup {
        retain_group_payload(group, self.pointers[group]);
        HeldGroup {
            group,
            payload: self.pointers[group],
        }
    }
}

/// The values of a group with `writes`, its writes in place order, over `values`, those of its seed.
fn written_values<'v>(
    values: &'v [RetainedStyleValueData],
    writes: &'v [ValueWrite],
) -> impl Iterator<Item = &'v RetainedStyleValueData> {
    let mut writes = writes
        .iter()
        .filter(|write| write.place.handle_offset().is_none())
        .peekable();
    values.iter().enumerate().map(
        move |(at, value)| match writes.next_if(|write| write.place.index() == at) {
            Some(write) => &write.value,
            None => value,
        },
    )
}

/// An element's computed values by group while its style is computed, before its groups are
/// built: each group's values are those of a seed group, its parent's or the initial style's, with
/// the writes over it where the element's values differ. Building a group then puts its values
/// into the group's allocation, the one a group of the element's own has.
pub(crate) struct GroupValues<'a> {
    seeds: SeedGroups<'a>,
    /// The values written over those of `seeds`, in place order, each differing from the value it
    /// is written over.
    writes: smallvec::SmallVec<[ValueWrite; WRITES_INLINE]>,
    /// The groups `writes` writes into, one bit per group.
    written_groups: u32,
    /// The values of the longhands held as bits.
    bits: StyleBits,
    facts: GroupValueFacts,
}

impl GroupValues<'_> {
    /// Whether the values of `group` are those of `payload` itself, a seed the element holds as it
    /// is, which taking the values into it leaves.
    #[inline]
    pub(crate) fn hold_the_seed(&self, group: usize, payload: *const c_void) -> bool {
        self.written_groups & (1 << group) == 0 && self.seeds.pointers[group] == payload
    }

    /// The writes into `group`, in place order.
    fn writes_of(&self, group: usize) -> &[ValueWrite] {
        if self.written_groups & (1 << group) == 0 {
            return &[];
        }
        let start = self.writes.partition_point(|write| write.place.group() < group);
        let end = self.writes.partition_point(|write| write.place.group() <= group);
        &self.writes[start..end]
    }

    /// Whether the groups these hold colors resolved against `currentcolor` in are those of `seed`:
    /// they hold its inherited groups and its recorded specified values.
    fn holds_what_the_current_color_groups_of(&self, seed: &GroupSeed<'_>) -> bool {
        let same_recorded = match (
            &seed.facts.inheritance_dependent_values,
            &self.facts.inheritance_dependent_values,
        ) {
            (Some(held), Some(values)) => std::sync::Arc::ptr_eq(held, values),
            (None, None) => true,
            _ => false,
        };
        same_recorded
            && INHERITED_SET_GROUPS.iter().all(|&group| {
                self.written_groups & (1 << group) == 0 && self.seeds.pointers[group] == seed.group(group).as_ptr()
            })
    }

    /// The groups holding colors resolved against `currentcolor`, read from the values.
    fn current_color_groups(&self) -> u32 {
        use crate::css::table_group_builder::{
            group_holds_current_color_for_its_reader, groups_reading_current_color_through_inherited_values,
        };
        self.facts
            .inheritance_dependent_values()
            .filter(|(property, value)| {
                !group_holds_current_color_for_its_reader(*property, value.optional_data())
                    && crate::css::style_value::retained_value_depends_on_current_color(value)
            })
            // NB: Logical aliases retain their specified values but own no output payload. Their
            //     resolved physical longhands are recorded separately and name the actual group.
            .filter_map(|(property, _)| computed_group_output_mask(property))
            .fold(0, |mask, groups| mask | groups)
            | groups_reading_current_color_through_inherited_values(|property| self.value(property).optional_data())
    }

    /// Whether the element's `color` is that of its parent, whose groups are `parent`, and resolves
    /// to the same color: it holds the parent's group of it, in the same color scheme.
    pub(crate) fn keeps_the_color_of(&self, parent: GroupSeed<'_>) -> bool {
        let group = GROUP_VALUE_LAYOUT
            .place_of(crate::css::property_metadata::property_id::COLOR)
            .group();
        self.written_groups & (1 << group) == 0
            && self.seeds.pointers[group] == parent.group(group).as_ptr()
            && self.facts.effective_color_scheme == parent.facts.effective_color_scheme
    }

    /// The computed value of `property`.
    pub(crate) fn value(&self, property: u16) -> &RetainedStyleValueData {
        let property = GROUP_VALUE_LAYOUT.resolved(property, |property| self.value(property));
        let place = GROUP_VALUE_LAYOUT.place_of(property);
        if let Some(field) = place.bit_field() {
            return self.bits.value(field);
        }
        if self.written_groups & (1 << place.group()) != 0
            && let Some(write) = self.writes.iter().find(|write| write.place == place)
        {
            return &write.value;
        }
        self.seeds.value(place)
    }

    /// What the groups holding these values hold as bits beside them.
    pub(crate) fn style_bits(&self) -> StyleBits {
        StyleBits {
            inherited: InheritedSetValues {
                bits: self.bits.inherited.bits,
                effective_zoom_bits: self.facts.effective_zoom.to_bits(),
            },
            reset: self.bits.reset,
        }
    }

    /// The value a group build reads for `property`: its recorded currentcolor-dependent specified
    /// value where a drive did not evaluate it, else its computed value.
    pub(crate) fn effective_value(&self, property: u16) -> &RetainedStyleValueData {
        self.facts
            .value_read_in_place(property)
            .unwrap_or_else(|| self.value(property))
    }

    pub(crate) fn facts(&self) -> &GroupValueFacts {
        &self.facts
    }

    /// A hash of the values `reads` names, the values of the groups `read_groups` names: the seed
    /// of each by its identity and what is written over it by content, so that it costs what the
    /// element declares.
    pub(crate) fn read_hash(&self, reads: &GroupReads, read_groups: u32, hasher: &mut impl std::hash::Hasher) {
        let mut remaining = read_groups;
        while remaining != 0 {
            let group = remaining.trailing_zeros() as usize;
            remaining &= remaining - 1;
            hasher.write_usize(self.seeds.pointers[group] as usize);
            for write in self.writes_of(group) {
                if write.place.is_read_by(reads) {
                    hasher.write_u32(write.place.order());
                    hasher.write_u64(crate::css::style_value::style_value_content_hash(
                        write.value.optional_data(),
                    ));
                }
            }
        }
    }

    /// The seed of each group `read_groups` names, held, in group order.
    pub(crate) fn held_read_seeds(&self, read_groups: u32) -> impl Iterator<Item = HeldGroup> + '_ {
        (0..group_index::COUNT)
            .filter(move |&group| read_groups & (1 << group) != 0)
            .map(|group| self.seeds.held(group))
    }

    /// What is written over the values `reads` names, of the groups `read_groups` names, in place
    /// order, by where each comes in it.
    pub(crate) fn read_writes<'r>(
        &'r self,
        reads: &'r GroupReads,
        read_groups: u32,
    ) -> impl Iterator<Item = (u32, &'r RetainedStyleValueData)> + 'r {
        // NB: A group's build mostly reads its own values alone.
        let writes = match read_groups.is_power_of_two() {
            true => self.writes_of(read_groups.trailing_zeros() as usize),
            false => &self.writes[..],
        };
        writes
            .iter()
            .filter(|write| write.place.is_read_by(reads))
            .map(|write| (write.place.order(), &write.value))
    }

    /// Whether these start from the seeds `seeds`, those of each group `read_groups` names in group
    /// order, and write `written`, by where each value comes in place order, over the values `reads`
    /// names, each value by identity or else by value.
    pub(crate) fn hold_read_values(
        &self,
        reads: &GroupReads,
        read_groups: u32,
        seeds: &[HeldGroup],
        written: &[(u32, RetainedStyleValueData)],
    ) -> bool {
        let mut seeds = seeds.iter();
        let mut remaining = read_groups;
        while remaining != 0 {
            let group = remaining.trailing_zeros() as usize;
            remaining &= remaining - 1;
            if seeds
                .next()
                .is_none_or(|seed| seed.as_ptr() != self.seeds.pointers[group])
            {
                return false;
            }
        }
        let mut written = written.iter();
        self.read_writes(reads, read_groups).all(|(place, value)| {
            written
                .next()
                .is_some_and(|(order, held)| *order == place && (held.pointer() == value.pointer() || held == value))
        }) && written.next().is_none()
    }

    /// Puts the values of each group into the group `groups` holds for it, built from them,
    /// taking over the reference `groups` holds: the very group where it holds them already, or
    /// else the built group with the values in it. The groups `holding` names hold them already.
    ///
    /// # Safety
    /// `groups` must hold one live payload of each group, each with a reference the caller holds.
    pub(crate) unsafe fn put_into(&self, groups: &mut [*const c_void; group_index::COUNT], holding: u32) {
        for (group, payload) in groups.iter_mut().enumerate() {
            let seed = self.seeds.pointers[group];
            if holding & (1 << group) != 0 || (*payload == seed && self.written_groups & (1 << group) == 0) {
                continue;
            }
            let writes = self.writes_of(group);
            // SAFETY: The caller passes live payloads, and the seeds are live while these are.
            *payload = unsafe {
                match GROUPS_OF_TYPED_LONGHANDS & (1 << group) != 0 {
                    true => {
                        let untold = self.typed_values_untold(group, *payload);
                        group_with_values(group, *payload, seed, std::slice::from_ref(&untold), writes)
                    }
                    false => group_with_values(group, *payload, seed, self.seeds.values(group), writes),
                }
            };
        }
    }

    /// The value of the slot of the group at `group`, whose longhands typed fields hold, in `payload`,
    /// built from these values: none where the typed fields tell every value, or else a list of them.
    fn typed_values_untold(&self, group: usize, payload: *const c_void) -> RetainedStyleValueData {
        let told = |place: LonghandPlace, value: &RetainedStyleValueData| {
            let offset = place.handle_offset().expect("a typed field place");
            // SAFETY: The caller passes a live payload of the group.
            let held = unsafe { place.value_in_payload(payload, offset) };
            held.pointer() == value.pointer() || held == value
        };
        let seed_tells_all = self.seeds.values(group)[TYPED_VALUES_SLOT].pointer().is_null();
        let tells_all = match seed_tells_all {
            true => self
                .writes_of(group)
                .iter()
                .all(|write| told(write.place, &write.value)),
            false => GroupValueLayout::handles_of(group).all(|(property, place)| told(place, self.value(property))),
        };
        if tells_all {
            return RetainedStyleValueData::none();
        }
        let values: Vec<_> = GroupValueLayout::handles_of(group)
            .map(|(property, _)| self.value(property).clone())
            .collect();
        RetainedStyleValueData::from_owned(StyleValueData::ValueList {
            values: crate::css::style_value::RetainedStyleValueDataList::from_retained_values(values),
            separator: 0,
            collapsible: false,
        })
    }

    /// The groups holding these values, a seed group where nothing is written over it or else one
    /// of its own, with a reference to each: groups of values, whose payloads are not built from
    /// them.
    pub(crate) fn into_groups(self) -> StyleGroups {
        let bits = self.style_bits();
        let pointers = std::array::from_fn(|group| {
            let writes = self.writes_of(group);
            if writes.is_empty() {
                return std::mem::ManuallyDrop::new(self.seeds.held(group)).payload;
            }
            // SAFETY: The seed is live while these values are.
            let base = unsafe { GroupPayloadRef::new(group, self.seeds.pointers[group]) }.expect("a seed has a group");
            let mut own = HeldGroup::new_copy(base, written_values(self.seeds.values(group), writes));
            for write in writes.iter().filter(|write| write.place.handle_offset().is_some()) {
                own.set_payload_field(write.place, &write.value);
            }
            if GROUPS_OF_TYPED_LONGHANDS & (1 << group) != 0 {
                let untold = self.typed_values_untold(group, own.as_ptr());
                // SAFETY: The group was just made, and this is its only reference.
                let slots = unsafe { values_of_mut(group, own.as_ptr().cast_mut()) };
                slots[TYPED_VALUES_SLOT] = untold;
            }
            std::mem::ManuallyDrop::new(own).payload
        });
        // SAFETY: Each pointer is a live group of its index, with a reference handed over.
        unsafe { StyleGroups::adopt(pointers, bits) }
    }

    /// What the groups are built from beside their values.
    pub(crate) fn into_facts(self) -> GroupValueFacts {
        self.facts
    }
}

/// An element's computed values as the engine computes them, directly or through the longhand drive,
/// over the values the element holds before its own declarations, with the flags its record keeps
/// beside them. The writes are kept beside those values, which the writer only borrows where its
/// parent or the initial style holds them: a group's values are copied only as its allocation is built,
/// for the groups whose written values no seed holds: a copy-on-write style builder, at the
/// granularity of groups.
#[derive(Clone)]
pub(crate) struct GroupValuesWriter<'a> {
    /// The values the element holds before its own declarations, which no write changes.
    seeded: SeedGroups<'a>,
    /// The values written over the seeded ones, one per longhand whose value differs from the
    /// seeded one where it was written.
    writes: smallvec::SmallVec<[ValueWrite; WRITES_INLINE]>,
    /// The groups `writes` writes into, one bit per group.
    written_groups: u32,
    /// The longhands `writes` writes, one bit per longhand.
    written_longhands: [u64; crate::css::property_metadata::LONGHAND_WORD_COUNT],
    /// The values of the longhands held as bits, as written so far. The zoom beside them is
    /// `effective_zoom`'s.
    bits: StyleBits,
    /// The recorded specified values of the longhands whose computed values depend on what the
    /// element inherits, as the initial style records them.
    seeded_inheritance_dependent_values: Option<std::sync::Arc<[InheritanceDependentValue]>>,
    /// The recorded specified values of the longhands whose computed values depend on what the
    /// element inherits, once the computation records others than the seeded ones.
    inheritance_dependent_values: Option<Vec<(u16, RetainedStyleValueData)>>,
    effective_zoom: f32,
    display_before_box_type_transformation: u32,
    effective_color_scheme: i16,
    /// Whether each longhand's value is important, inherited and evaluated by the computation,
    /// and what the box adjustments and the font computation settled.
    flags: crate::css::computed_longhand_table::LonghandTableFlags,
    /// What the computed values depend on, as `DEPENDS_ON_VIEWPORT_METRICS` and the like.
    dependency_flags: u8,
}

/// A value written over an element's seeded values, at its place among them.
#[derive(Clone)]
struct ValueWrite {
    place: LonghandPlace,
    value: RetainedStyleValueData,
}

/// How many written values a [`GroupValuesWriter`] holds before they go to an allocation of their
/// own.
const WRITES_INLINE: usize = 16;

impl<'a> GroupValuesWriter<'a> {
    /// The values a child of the element whose groups are `parent` holds before its own
    /// declarations: the initial values, but the parent's values of the longhands that inherit, and
    /// then likewise the values `inherited_overrides` hands down in place of those. The child starts with the parent's zoom and color scheme and
    /// the initial recorded specified values.
    ///
    /// The writer starts with the flags `flags` and the dependency flags `dependency_flags`.
    #[inline]
    pub(crate) fn seeded_for_child(
        parent: GroupSeed<'a>,
        initial: GroupSeed<'a>,
        inherited_overrides: impl Iterator<Item = (u16, RetainedStyleValueData)>,
        flags: crate::css::computed_longhand_table::LonghandTableFlags,
        dependency_flags: u8,
    ) -> Self {
        let mut writer = Self::seeded(parent, initial, flags, dependency_flags);
        writer.inherit(initial, inherited_overrides);
        writer
    }

    /// A writer over the groups a child holds of `parent` and `initial`, before what its parent
    /// hands down, which [`Self::inherit`] writes.
    pub(crate) fn seeded(
        parent: GroupSeed<'a>,
        initial: GroupSeed<'a>,
        flags: crate::css::computed_longhand_table::LonghandTableFlags,
        dependency_flags: u8,
    ) -> Self {
        // NB: A group's longhands all inherit or all do not, so a child borrows its parent's group of
        //     those that do and the initial group of those that do not.
        let mut pointers = initial.groups.pointers();
        for (&group, pointer) in INHERITED_SET_GROUPS.iter().zip(&parent.groups.inherited.get().groups) {
            pointers[group] = pointer.as_ptr();
        }
        let seeded = SeedGroups {
            pointers,
            styles: std::marker::PhantomData,
        };
        Self {
            seeded,
            writes: smallvec::SmallVec::new(),
            written_groups: 0,
            written_longhands: [0; crate::css::property_metadata::LONGHAND_WORD_COUNT],
            // NB: A child inherits its parent's inherited bits, and starts from the initial reset ones.
            bits: StyleBits {
                inherited: parent.style_bits().inherited,
                reset: initial.style_bits().reset,
            },
            seeded_inheritance_dependent_values: initial.facts.inheritance_dependent_values.clone(),
            inheritance_dependent_values: None,
            effective_zoom: parent.facts.effective_zoom,
            display_before_box_type_transformation: initial.facts.display_before_box_type_transformation,
            effective_color_scheme: parent.facts.effective_color_scheme(),
            flags,
            dependency_flags,
        }
    }

    /// Writes what the parent hands down over the seeded values, with the recorded specified values
    /// of `initial` as a child holds them.
    pub(crate) fn inherit(
        &mut self,
        initial: GroupSeed<'_>,
        inherited_overrides: impl Iterator<Item = (u16, RetainedStyleValueData)>,
    ) {
        use crate::css::property_metadata::property_is_inherited;
        // NB: What the parent hands down is not the child's computation, so it is not evaluated.
        for (property, value) in inherited_overrides {
            let held = self.value(property);
            if held.pointer().is_null() || *held != value {
                self.put(property, std::borrow::Cow::Owned(value));
            }
        }
        // NB: An initial recorded value is that of the initial value. A longhand that inherits another one holds none,
        //     or a group build would read the initial `currentcolor` in place of what the parent hands down.
        for held in initial
            .facts
            .inheritance_dependent_values
            .as_deref()
            .unwrap_or_default()
        {
            if property_is_inherited(held.property)
                && crate::css::style_value::retained_value_depends_on_current_color(&held.value)
                && *self.value(held.property) != held.value
            {
                self.set_inheritance_dependent_value(held.property, None);
            }
        }
    }

    fn was_written(&self, property: u16) -> bool {
        self.flags.is_evaluated(property)
    }

    /// The recorded specified values the initial style holds of the longhands whose computed
    /// values depend on what the element inherits.
    fn seeded_inheritance_dependent_values(&self) -> &[InheritanceDependentValue] {
        self.seeded_inheritance_dependent_values.as_deref().unwrap_or_default()
    }

    /// The write of `property`, whose place is `place`, if there is one.
    fn write_of(&self, property: u16, place: LonghandPlace) -> Option<&ValueWrite> {
        let index = LonghandID::new(property).index();
        if self.written_longhands[index / 64] & (1 << (index % 64)) == 0 {
            return None;
        }
        self.writes.iter().find(|write| write.place == place)
    }

    /// The computed value of `property` written so far.
    pub(crate) fn value(&self, property: u16) -> &RetainedStyleValueData {
        let property = GROUP_VALUE_LAYOUT.resolved(property, |property| self.value(property));
        let place = GROUP_VALUE_LAYOUT.place_of(property);
        if let Some(field) = place.bit_field() {
            return self.bits.value(field);
        }
        match self.write_of(property, place) {
            Some(write) => &write.value,
            None => self.seeded.value(place),
        }
    }

    /// The values of the longhands held as bits, as written so far.
    pub(crate) fn bits(&self) -> StyleBits {
        self.bits
    }

    /// Whether the value of `property` differs from what the writer was seeded with, the bits
    /// standing as `seeded_bits`.
    pub(crate) fn moved_from_seed(&self, property: u16, seeded_bits: StyleBits) -> bool {
        let property = GROUP_VALUE_LAYOUT.resolved(property, |property| self.value(property));
        let place = GROUP_VALUE_LAYOUT.place_of(property);
        if let Some(field) = place.bit_field() {
            return self.bits.get(field) != seeded_bits.get(field);
        }
        self.write_of(property, place)
            .is_some_and(|write| write.value != *self.seeded.value(place))
    }

    /// The computed value of `property` written so far, where it has one.
    pub(crate) fn get(&self, property: u16) -> Option<&RetainedStyleValueData> {
        Some(self.value(property)).filter(|value| !value.pointer().is_null())
    }

    /// The value a group build reads for `property`: its recorded currentcolor-dependent specified
    /// value where the computation did not evaluate it, else its computed value.
    pub(crate) fn effective_value(&self, property: u16) -> &RetainedStyleValueData {
        if !self.was_written(property) {
            let recorded = match &self.inheritance_dependent_values {
                Some(values) => values
                    .iter()
                    .find(|(held, _)| *held == property)
                    .map(|(_, value)| value),
                None => self
                    .seeded_inheritance_dependent_values()
                    .iter()
                    .find(|held| held.property == property)
                    .map(|held| &held.value),
            };
            if let Some(recorded) = recorded
                && crate::css::style_value::retained_value_depends_on_current_color(recorded)
            {
                return recorded;
            }
        }
        self.value(property)
    }

    /// Writes `value` as the computed value of `property`, which the computation evaluated.
    pub(crate) fn set(&mut self, property: u16, value: RetainedStyleValueData) {
        self.flags.mark_evaluated(property);
        self.put(property, std::borrow::Cow::Owned(value));
    }

    /// Writes `value` as the computed value of `property`, which the computation evaluated,
    /// holding a reference to it only where it stays as a write.
    pub(crate) fn set_borrowed(&mut self, property: u16, value: &RetainedStyleValueData) {
        self.flags.mark_evaluated(property);
        self.put(property, std::borrow::Cow::Borrowed(value));
    }

    /// Writes `value` as the computed value of `property`, keeping the value held where it is
    /// equal, which the computation evaluated.
    pub(crate) fn set_or_keep_equal(&mut self, property: u16, value: &RetainedStyleValueData) {
        let held = self.value(property);
        if held.pointer().is_null() || held != value {
            self.put(property, std::borrow::Cow::Borrowed(value));
        }
        self.flags.mark_evaluated(property);
    }

    fn put(&mut self, property: u16, value: std::borrow::Cow<'_, RetainedStyleValueData>) {
        // NB: A logical alias holding no value of its own computes as the physical longhand it
        //     maps to, which the computation writes.
        if !GROUP_VALUE_LAYOUT.holds_value(LonghandID::new(property)) {
            return;
        }
        let place = GROUP_VALUE_LAYOUT.place_of(property);
        if let Some(field) = place.bit_field() {
            self.bits = self.bits.with_value(field, &value);
            return;
        }
        let index = LonghandID::new(property).index();
        let (word, bit) = (index / 64, 1 << (index % 64));
        if self.written_longhands[word] & bit != 0 {
            let write = self
                .writes
                .iter_mut()
                .find(|write| write.place == place)
                .expect("a longhand written has a write");
            write.value = value.into_owned();
            return;
        }
        if self.seeded.value(place).pointer() == value.pointer() {
            return;
        }
        self.written_longhands[word] |= bit;
        self.written_groups |= 1 << place.group();
        self.writes.push(ValueWrite {
            place,
            value: value.into_owned(),
        });
    }

    /// Records `value` as the specified value of `property` where its computed value depends on
    /// what the element inherits, or that it does not depend on it for none.
    pub(crate) fn set_inheritance_dependent_value(&mut self, property: u16, value: Option<RetainedStyleValueData>) {
        if self.inheritance_dependent_values.is_none() {
            let seeded = self.seeded_inheritance_dependent_values();
            match (seeded.iter().find(|held| held.property == property), &value) {
                (None, None) => return,
                (Some(held), Some(value)) if held.value.pointer() == value.pointer() => return,
                _ => {}
            }
            self.inheritance_dependent_values =
                Some(seeded.iter().map(|held| (held.property, held.value.clone())).collect());
        }
        let values = self
            .inheritance_dependent_values
            .as_mut()
            .expect("the seeded values were just copied");
        let existing = values.iter().position(|(held, _)| *held == property);
        match (existing, value) {
            (Some(index), Some(value)) => {
                if values[index].1.pointer() != value.pointer() {
                    values[index].1 = value;
                }
            }
            (Some(index), None) => {
                values.swap_remove(index);
            }
            (None, Some(value)) => values.push((property, value)),
            (None, None) => {}
        }
    }

    /// The recorded specified values of the longhands whose computed values depend on what the
    /// element inherits, each with whether a group build reads it: those of the first of `seeds`
    /// holding the very same ones, shared, as the parent of an element writing none of them does.
    fn finished_inheritance_dependent_values(
        &self,
        seeds: &[GroupSeed<'_>],
    ) -> Option<std::sync::Arc<[InheritanceDependentValue]>> {
        let read_by_groups = |property: u16, value: &RetainedStyleValueData| {
            !self.was_written(property) && crate::css::style_value::retained_value_depends_on_current_color(value)
        };
        let seeded = self.seeded_inheritance_dependent_values();
        let edited = self.inheritance_dependent_values.as_deref();
        let length = edited.map_or(seeded.len(), <[_]>::len);
        if length == 0 {
            return None;
        }
        let entry = |index: usize| match edited {
            Some(values) => (values[index].0, &values[index].1),
            None => (seeded[index].property, &seeded[index].value),
        };
        let same = |held: &[InheritanceDependentValue]| {
            held.len() == length
                && held.iter().enumerate().all(|(index, held)| {
                    let (property, value) = entry(index);
                    held.property == property
                        && held.value.pointer() == value.pointer()
                        && held.read_by_groups == read_by_groups(property, value)
                })
        };
        if let Some(shared) = seeds
            .iter()
            .filter_map(|seed| seed.facts.inheritance_dependent_values.as_ref())
            .find(|held| same(held))
        {
            return Some(shared.clone());
        }
        Some(
            (0..length)
                .map(|index| {
                    let (property, value) = entry(index);
                    InheritanceDependentValue {
                        property,
                        value: value.clone(),
                        read_by_groups: read_by_groups(property, value),
                    }
                })
                .collect(),
        )
    }

    pub(crate) fn set_display_before_box_type_transformation(&mut self, display: u32) {
        self.display_before_box_type_transformation = display;
    }

    pub(crate) fn effective_color_scheme(&self) -> i16 {
        self.effective_color_scheme
    }

    pub(crate) fn set_effective_color_scheme(&mut self, scheme: i16) {
        self.effective_color_scheme = scheme;
    }

    pub(crate) fn set_effective_zoom(&mut self, zoom: f32) {
        self.effective_zoom = zoom;
    }

    /// Writes `value` as the computed value of `property`, keeping the value held where it is
    /// equal.
    pub(crate) fn set_computed(&mut self, property: u16, value: StyleValueData) {
        if self.value(property).optional_data() == Some(&value) {
            self.flags.mark_evaluated(property);
            return;
        }
        self.set(property, RetainedStyleValueData::from_owned(value));
    }

    pub(crate) fn note_viewport_dependency(&mut self) {
        self.dependency_flags |= crate::css::computed_longhand_table::DEPENDS_ON_VIEWPORT_METRICS;
    }

    /// Adds `flags` to what the computed values depend on.
    pub(crate) fn merge_dependency_flags(&mut self, flags: u8) {
        self.dependency_flags |= flags;
    }

    /// Records whether an author declaration disables the element's native appearance.
    pub(crate) fn set_native_appearance_disabled(&mut self, disabled: bool) {
        use crate::css::computed_longhand_table::NATIVE_APPEARANCE_DISABLED;
        if disabled {
            self.dependency_flags |= NATIVE_APPEARANCE_DISABLED;
        } else {
            self.dependency_flags &= !NATIVE_APPEARANCE_DISABLED;
        }
    }

    pub(crate) fn flags_mut(&mut self) -> &mut crate::css::computed_longhand_table::LonghandTableFlags {
        &mut self.flags
    }

    pub(crate) fn set_overflow_before_adjustment(&mut self, overflow: [u16; 2]) {
        self.flags.set_overflow_before_adjustment(overflow);
    }

    /// Records that the computed value of `property` is no longer important or inherited.
    pub(crate) fn clear_flags(&mut self, property: u16) {
        self.flags.set_important(property, false);
        self.flags.set_inherited(property, false);
    }

    /// The values written, sharing each group of `seeds` that holds the same values. A group build
    /// reads the recorded currentcolor-dependent specified value of a longhand the computation
    /// did not write in place of its computed value. Also answers the flags and dependency flags
    /// the computation settled beside the values.
    #[inline]
    pub(crate) fn finish(
        self,
        seeds: &[GroupSeed<'a>],
    ) -> (
        GroupValues<'a>,
        crate::css::computed_longhand_table::LonghandTableFlags,
        u8,
    ) {
        let mut finished = self.finish_without_current_color(seeds);
        // NB: What a parent's groups read of `currentcolor` a child holding the same inherited groups
        //     and recorded values reads too.
        finished.0.facts.current_color_groups = match seeds.first() {
            Some(parent) if finished.0.holds_what_the_current_color_groups_of(parent) => {
                parent.facts.current_color_groups
            }
            _ => finished.0.current_color_groups(),
        };
        finished
    }

    fn finish_without_current_color(
        mut self,
        seeds: &[GroupSeed<'a>],
    ) -> (
        GroupValues<'a>,
        crate::css::computed_longhand_table::LonghandTableFlags,
        u8,
    ) {
        let inheritance_dependent_values = self.finished_inheritance_dependent_values(seeds);
        let read_in_place_bits = match (
            seeds.iter().find(
                |seed| match (&seed.facts.inheritance_dependent_values, &inheritance_dependent_values) {
                    (Some(held), Some(values)) => std::sync::Arc::ptr_eq(held, values),
                    _ => false,
                },
            ),
            &inheritance_dependent_values,
        ) {
            (Some(seed), _) => seed.facts.read_in_place_bits,
            (None, Some(values)) => values
                .iter()
                .filter(|held| held.read_by_groups)
                .fold(0, |bits, held| bits | read_in_place_bit(held.property)),
            (None, None) => 0,
        };
        // NB: A value written back over the seed's leaves the seed's.
        let seeded = &self.seeded;
        self.writes
            .retain(|write| seeded.value(write.place).pointer() != write.value.pointer());
        self.writes.sort_unstable_by_key(|write| write.place.order());
        let written_groups = self
            .writes
            .iter()
            .fold(0_u32, |groups, write| groups | (1 << write.place.group()));
        // The first of `seeds` holding the very values the element's group does holds them for it.
        let mut remaining = written_groups;
        let mut shared_groups = 0_u32;
        while remaining != 0 {
            let group = remaining.trailing_zeros() as usize;
            remaining &= remaining - 1;
            let start = self.writes.partition_point(|write| write.place.group() < group);
            let end = self.writes.partition_point(|write| write.place.group() <= group);
            let group_writes = &self.writes[start..end];
            // NB: A write to a payload handle leaves the element's group its own.
            if group_writes.iter().any(|write| write.place.handle_offset().is_some()) {
                continue;
            }
            let shared = seeds.iter().find(|seed| {
                let seed_group = seed.group(group);
                group_writes
                    .iter()
                    .all(|write| value_at(seed_group, write.place).pointer() == write.value.pointer())
                    && written_values(self.seeded.values(group), group_writes)
                        .zip(seed_group.group_values())
                        .all(|(value, held)| value.pointer() == held.pointer())
                    && GroupValueLayout::handles_of(group)
                        .all(|(_, place)| value_at(seed_group, place).pointer() == self.seeded.value(place).pointer())
            });
            if let Some(seed) = shared {
                self.seeded.replace(group, seed.group(group));
                shared_groups |= 1 << group;
            }
        }
        if shared_groups != 0 {
            self.writes
                .retain(|write| shared_groups & (1 << write.place.group()) == 0);
        }
        let native_appearance_disabled =
            self.dependency_flags & crate::css::computed_longhand_table::NATIVE_APPEARANCE_DISABLED != 0;
        (
            GroupValues {
                seeds: self.seeded,
                writes: self.writes,
                written_groups: written_groups & !shared_groups,
                bits: self.bits,
                facts: GroupValueFacts {
                    inheritance_dependent_values,
                    effective_zoom: self.effective_zoom,
                    display_before_box_type_transformation: self.display_before_box_type_transformation,
                    effective_color_scheme: i8::try_from(self.effective_color_scheme)
                        .expect("a color scheme is a small code"),
                    native_appearance_disabled,
                    read_in_place_bits,
                    current_color_groups: 0,
                },
            },
            self.flags,
            self.dependency_flags,
        )
    }
}

/// One gathered value for the generic group builder: the computed value's
/// data, plus the resolved raw color for color-kind fields.
pub(crate) struct GroupValueEntry<'a> {
    pub(crate) data: &'a RetainedStyleValueData,
    pub(crate) resolved_color: u32,
    pub(crate) has_resolved_color: bool,
    pub(crate) resolved_number: f64,
    pub(crate) has_resolved_number: bool,
}

/// One decoded write into a scratch payload, at an offset into it.
#[derive(Clone, Copy)]
enum GroupFieldPoke<'a> {
    U8(usize, u8),
    F32(usize, f32),
    F64(usize, f64),
    I32(usize, i32),
    U64(usize, u64),
    U32(usize, u32),
    HeldColor(usize, crate::css::computed_value_types::StyleColor),
    Data(usize, &'a RetainedStyleValueData),
}

pub(crate) const MAX_GROUP_FIELD_COUNT: usize = 48;

/// The pokes of one group, which has at most `MAX_GROUP_FIELD_COUNT` fields.
type GroupFieldPokes<'a> = smallvec::SmallVec<[GroupFieldPoke<'a>; MAX_GROUP_FIELD_COUNT]>;

/// Decodes one group's gathered values against its descriptors into scratch
/// pokes. Constraint descriptors (the Require kinds) fail the decode when the
/// value diverges and constraints are enforced. A Rust group builder that
/// completes complex members itself skips those constraints.
fn decode_group_field_pokes<'a>(
    descriptors: &[GroupFieldDescriptor],
    values: &[GroupValueEntry<'a>],
    enforce_constraints: bool,
) -> Option<GroupFieldPokes<'a>> {
    use crate::css::style_compute::px_length_unit;
    use crate::css::style_value::StyleValueData;
    use GroupFieldKind as Kind;
    use GroupFieldPoke as Poke;

    assert!(values.len() <= MAX_GROUP_FIELD_COUNT);
    let mut pokes = GroupFieldPokes::new();
    for (descriptor, value) in descriptors.iter().zip(values) {
        let data = value.data.optional_data()?;
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
            Kind::HeldColorOrKeyword(keyword) if matches!(data, StyleValueData::Keyword { keyword: value_keyword } if *value_keyword == keyword) =>
                {}
            Kind::HeldColor | Kind::HeldColorOrKeyword(_) => {
                use crate::css::computed_value_types::StyleColor;
                if matches!(data, StyleValueData::Keyword { keyword } if *keyword == crate::css::css_enums::keyword::CURRENTCOLOR)
                {
                    pokes.push(Poke::HeldColor(offset, StyleColor::CURRENT_COLOR));
                } else if value.has_resolved_color {
                    pokes.push(Poke::HeldColor(offset, StyleColor::rgba(value.resolved_color)));
                } else if enforce_constraints {
                    return None;
                }
            }
            Kind::ResolvedF32 => pokes.push(Poke::F32(offset, resolved_number()? as f32)),
            Kind::ResolvedF64 => pokes.push(Poke::F64(offset, resolved_number()?)),
            Kind::ResolvedU8 => pokes.push(Poke::U8(offset, resolved_number()? as u8)),
            Kind::RetainedData => pokes.push(Poke::Data(offset, value.data)),
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
                if value.data.pointer() != crate::css::style_compute::initial_value_data(descriptor.property_id).cast()
                {
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
                Poke::HeldColor(offset, value) => {
                    *(base.add(offset) as *mut crate::css::computed_value_types::StyleColor) = value;
                }
                Poke::Data(offset, data) => {
                    // The slot's constructor default is null, so nothing is released.
                    *(base.add(offset) as *mut *const StyleValueData) = data.clone().into_pointer();
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
unsafe fn free_scratch_payload(group: usize, scratch: *mut c_void) {
    unsafe { destroy_allocation(group, scratch) }
}

/// Builds a style group payload generically from its registered field
/// descriptors: decodes every descriptor's value (returning null for the C++
/// population path when any value cannot be decoded or a constraint fails),
/// default-constructs a scratch payload, pokes the simple fields, and shares
/// the parent or default payload when the result compares equal.
pub(crate) fn build_style_group(
    group_index: usize,
    values: &[GroupValueEntry<'_>],
    parent: GroupShares<'_>,
) -> *const c_void {
    let build_or_reuse_group_payload = || {
        let descriptors = group_field_descriptors(group_index);
        if descriptors.len() != values.len() {
            return None;
        }
        let pokes = decode_group_field_pokes(descriptors, values, true)?;

        // A group whose descriptors are all satisfied constraints would
        // scratch-build an exact copy of the default payload, so skip the
        // allocation and share directly.
        if pokes.is_empty() {
            return Some(share_default_group_payload(group_index, parent));
        }

        let mut scratch = ScratchPayload::new_default(group_index);
        scratch.apply_pokes(&pokes);
        Some(share_equal_group_payload(scratch.as_ref(), parent).unwrap_or_else(|| scratch.publish()))
    };
    build_or_reuse_group_payload().unwrap_or(std::ptr::null())
}

/// Shares one group's default payload - or the parent payload when
/// it equals the default, keeping the identity - for a build whose inputs are
/// all initial values.
pub(crate) fn share_default_group_payload(group_index: usize, shares: GroupShares<'_>) -> *const c_void {
    share_matching_group_payload(group_index, shares, |candidate| candidate.equals(shares.default))
        .expect("the default payload equals itself")
}

/// Builds a Rust-native group whose descriptor-backed fields are completed by
/// a Rust closure before the normal parent/default sharing checks.
pub(crate) fn build_group_payload_with_rust_fill<T: StyleGroupPayload>(
    group_index: usize,
    values: &[GroupValueEntry<'_>],
    fill: impl FnOnce(&mut T),
    parent: GroupShares<'_>,
) -> *const c_void {
    let descriptors = group_field_descriptors(group_index);
    assert_eq!(descriptors.len(), values.len());
    let pokes = decode_group_field_pokes(descriptors, values, false)
        .expect("computed values decode for every non-constraint descriptor");

    let mut scratch = ScratchPayload::new_default(group_index);
    scratch.apply_pokes(&pokes);
    fill(scratch.values_mut::<T>());
    share_equal_group_payload(scratch.as_ref(), parent).unwrap_or_else(|| scratch.publish())
}

impl ComputedSize {
    /// The computed value this size holds, or the keyword its kind tells.
    fn computed_value(&self) -> &RetainedStyleValueData {
        use crate::css::css_enums::keyword;
        if self.value.pointer.is_null() {
            return RetainedStyleValueData::shared_keyword(match self.kind {
                ComputedSizeKind::Auto => keyword::AUTO,
                ComputedSizeKind::MinContent => keyword::MIN_CONTENT,
                ComputedSizeKind::MaxContent => keyword::MAX_CONTENT,
                ComputedSizeKind::FitContent => keyword::FIT_CONTENT,
                ComputedSizeKind::Stretch => keyword::STRETCH,
                ComputedSizeKind::None => keyword::NONE,
                ComputedSizeKind::Calculated | ComputedSizeKind::Length | ComputedSizeKind::Percentage => {
                    unreachable!("a size of this kind holds a value")
                }
            });
        }
        // SAFETY: A handle is a single retained pointer laid out as a `RetainedStyleValueData` is.
        unsafe { &*std::ptr::from_ref(&self.value).cast::<RetainedStyleValueData>() }
    }

    /// A size of `kind` holding `value`, which keeps the plain length or percentage `value` holds inline.
    pub(crate) fn new(kind: ComputedSizeKind, value: ComputedStyleValueHandle) -> Self {
        let (inline_form, inline_value) = InlineLengthPercentage::encode(InlineLengthPercentage::of(value.data()));
        Self {
            kind,
            inline_form,
            inline_value,
            value,
        }
    }

    pub(crate) fn keyword(kind: ComputedSizeKind) -> Self {
        Self::new(kind, ComputedStyleValueHandle::empty())
    }

    fn retained(kind: ComputedSizeKind, data: &RetainedStyleValueData) -> Self {
        Self::new(kind, ComputedStyleValueHandle::from_retained(data))
    }

    pub(crate) fn from_data(data: &RetainedStyleValueData) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        match data.optional_data() {
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
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::STRETCH => {
                Self::keyword(ComputedSizeKind::Stretch)
            }
            // NB: -webkit-fill-available behaves as stretch, and keeps its own keyword for serialization.
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::_WEBKIT_FILL_AVAILABLE => {
                Self::retained(ComputedSizeKind::Stretch, data)
            }
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::NONE => {
                Self::keyword(ComputedSizeKind::None)
            }
            Some(value @ StyleValueData::Function { .. })
                if crate::css::calc_size::calc_size_arguments(value).is_some() =>
            {
                // NB: A calc-size() acts like its basis keyword, or like a calculation for any other basis.
                let kind = match crate::css::calc_size::calc_size_basis_keyword(value) {
                    Some(keyword::AUTO) => ComputedSizeKind::Auto,
                    Some(keyword::MIN_CONTENT) => ComputedSizeKind::MinContent,
                    Some(keyword::MAX_CONTENT) => ComputedSizeKind::MaxContent,
                    Some(keyword::FIT_CONTENT) => ComputedSizeKind::FitContent,
                    Some(keyword::STRETCH) => ComputedSizeKind::Stretch,
                    _ => ComputedSizeKind::Calculated,
                };
                Self::retained(kind, data)
            }
            Some(StyleValueData::Function(function_payload)) => {
                let FunctionPayload { value, .. } = &**function_payload;
                Self::retained(ComputedSizeKind::FitContent, value)
            }
            Some(StyleValueData::Calculated { .. }) => Self::retained(ComputedSizeKind::Calculated, data),
            Some(StyleValueData::Percentage { .. }) => Self::retained(ComputedSizeKind::Percentage, data),
            Some(StyleValueData::Length { .. }) => Self::retained(ComputedSizeKind::Length, data),
            // NB: Layout resolves anchor-size() itself, through the calculation that wraps it.
            Some(StyleValueData::AnchorSize { .. }) => {
                Self::new(ComputedSizeKind::Calculated, anchor_function_calculated_handle(data))
            }
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
    fn from_data(data: &RetainedStyleValueData) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let is_content = matches!(
            data.optional_data(),
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::CONTENT
        );
        // A calc-size() with a content basis acts as content, and keeps the function for resolving the size.
        let is_calc_size_of_content = data
            .optional_data()
            .and_then(crate::css::calc_size::calc_size_basis_keyword)
            == Some(keyword::CONTENT);
        Self {
            is_content: is_content || is_calc_size_of_content,
            size: if is_content {
                ComputedSize::keyword(ComputedSizeKind::Auto)
            } else if is_calc_size_of_content {
                ComputedSize::retained(ComputedSizeKind::Auto, data)
            } else {
                ComputedSize::from_data(data)
            },
        }
    }
}

impl ComputedGap {
    fn from_data(data: &RetainedStyleValueData) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let is_normal = matches!(
            data.optional_data(),
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL
        );
        Self {
            is_normal,
            value: if is_normal {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::from_retained(data)
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
            align_content: align_content::NORMAL,
            align_items: align_items::STRETCH,
            align_self: align_self::AUTO,
            align_items_is_last_baseline: false,
            align_self_is_last_baseline: false,
            align_content_is_last_baseline: false,
            justify_content: justify_content::FLEX_START,
            justify_items: justify_items::LEGACY,
            justify_self: justify_self::AUTO,
            align_items_overflow: crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
            justify_items_overflow: crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
            align_self_overflow: crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
            justify_self_overflow: crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
            align_content_overflow: crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
            justify_content_overflow: crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
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

/// A handle to the calculated style value wrapping the anchor() or anchor-size() value `data` holds.
fn anchor_function_calculated_handle(data: &RetainedStyleValueData) -> ComputedStyleValueHandle {
    // SAFETY: The caller matched `data` as live anchor() or anchor-size() style value data.
    let calculated = unsafe { crate::css::calc::create_anchor_function_calculated(data.pointer().cast()) };
    ComputedStyleValueHandle {
        pointer: StyleValueArc::into_raw(calculated).cast(),
    }
}

impl ComputedLengthPercentageOrAuto {
    /// The computed value this side holds, or `auto`.
    fn computed_value(&self) -> &RetainedStyleValueData {
        if self.value.pointer.is_null() {
            return RetainedStyleValueData::shared_keyword(crate::css::css_enums::keyword::AUTO);
        }
        // SAFETY: A handle is a single retained pointer laid out as a `RetainedStyleValueData` is.
        unsafe { &*std::ptr::from_ref(&self.value).cast::<RetainedStyleValueData>() }
    }

    /// A value holding `value`, or auto, which keeps the plain length or percentage `value` holds inline.
    pub(crate) fn new(is_auto: bool, value: ComputedStyleValueHandle) -> Self {
        let (inline_form, inline_value) = InlineLengthPercentage::encode(InlineLengthPercentage::of(value.data()));
        Self {
            is_auto,
            inline_form,
            inline_value,
            value,
        }
    }

    fn auto() -> Self {
        Self::new(true, ComputedStyleValueHandle::empty())
    }

    fn zero() -> Self {
        Self::new(false, ComputedStyleValueHandle::length(0.0))
    }

    pub(crate) fn from_data(data: &RetainedStyleValueData) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        let is_auto = matches!(
            data.optional_data(),
            Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::AUTO
        );
        Self::new(
            is_auto,
            if is_auto {
                ComputedStyleValueHandle::empty()
            } else {
                ComputedStyleValueHandle::from_retained(data)
            },
        )
    }

    fn from_length_box_data(data: &RetainedStyleValueData, default_is_auto: bool) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::style_value::StyleValueData;

        match data.optional_data() {
            Some(StyleValueData::Keyword { keyword: value }) if *value == keyword::AUTO => Self::auto(),
            Some(
                StyleValueData::Length { .. } | StyleValueData::Percentage { .. } | StyleValueData::Calculated { .. },
            ) => Self::new(false, ComputedStyleValueHandle::from_retained(data)),
            // NB: Layout resolves an anchor-size() margin itself, through the calculation that wraps it. Insets keep
            //     their anchor functions in separate wrappers instead, and expose auto here.
            Some(StyleValueData::AnchorSize { .. }) if !default_is_auto => {
                Self::new(false, anchor_function_calculated_handle(data))
            }
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
        let side = || ComputedLengthPercentageOrAuto::new(false, zero.clone());
        Self {
            top: side(),
            right: side(),
            bottom: side(),
            left: side(),
        }
    }

    pub(crate) fn from_data(
        top: &RetainedStyleValueData,
        right: &RetainedStyleValueData,
        bottom: &RetainedStyleValueData,
        left: &RetainedStyleValueData,
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
        use crate::css::css_enums::{baseline_source, field_sizing, resize, text_overflow, vertical_align};
        use crate::css::display::FfiDisplay;

        Self {
            display: FfiDisplay::inline(),
            display_before_box_type_transformation: FfiDisplay::inline(),
            field_sizing: field_sizing::FIXED,
            baseline_source: baseline_source::AUTO,
            resize: resize::NONE,
            text_overflow: text_overflow::CLIP,
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
            container_name: RetainedUtf16FlyStringList::from_vec(Vec::new()),
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
            rx: ComputedLengthPercentageOrAuto::auto(),
            ry: ComputedLengthPercentageOrAuto::auto(),
            x: zero.clone(),
            y: zero,
            stop_color: crate::css::computed_value_types::StyleColor::rgba(OPAQUE_BLACK_BGRA),
            stop_opacity: 1.0,
            flood_color: crate::css::computed_value_types::StyleColor::rgba(OPAQUE_BLACK_BGRA),
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
            text_decoration_color: crate::css::computed_value_types::StyleColor::CURRENT_COLOR,
            white_space_trim_discard_before: false,
            white_space_trim_discard_after: false,
            white_space_trim_discard_inner: false,
        }
    }
}

impl TransformValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
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
            has_offset_path: false,
            offset_path: initial(property_id::OFFSET_PATH),
            offset_distance: initial(property_id::OFFSET_DISTANCE),
            offset_position: initial(property_id::OFFSET_POSITION),
            offset_anchor: initial(property_id::OFFSET_ANCHOR),
            offset_rotate: initial(property_id::OFFSET_ROTATE),
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

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
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
            anchor_names: RetainedUtf16FlyStringList::from_vec(Vec::new()),
            anchor_scope_all: false,
            anchor_scope_names: RetainedUtf16FlyStringList::from_vec(Vec::new()),
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
                color: crate::css::computed_value_types::StyleColor::CURRENT_COLOR,
            },
            accent_color: ComputedColorOrAuto {
                is_auto: true,
                color: crate::css::computed_value_types::StyleColor::rgba(0xff00_0000),
            },
            cursor: RetainedComputedCursorList::from_vec(vec![ComputedCursor {
                is_cursor_value: false,
                cursor: ComputedStyleValueHandle::empty(),
                predefined: crate::css::css_enums::cursor_predefined::AUTO,
            }]),
            forced_color_adjust: crate::css::css_enums::forced_color_adjust::AUTO,
            interpolate_size: crate::css::css_enums::interpolate_size::NUMERIC_ONLY,
            scrollbar_color: crate::css::computed_value_types::ComputedScrollbarColor {
                thumb_color: 0,
                track_color: 0,
                is_auto: true,
            },
            color_scheme: 0,
            color_schemes: RetainedUtf16FlyStringList::from_vec(Vec::new()),
            image_rendering: crate::css::css_enums::image_rendering::AUTO,
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
            color: crate::css::computed_value_types::StyleColor::rgba(color),
        }
    }

    fn initial() -> Self {
        use crate::css::css_enums;
        use crate::css::property_metadata::property_id;

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
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
            text_justify: enum_value(css_enums::keyword_to_text_justify(css_enums::keyword::AUTO)),
            line_break: enum_value(css_enums::keyword_to_line_break(css_enums::keyword::AUTO)),
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

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
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
            animation_range_start: initial(property_id::ANIMATION_RANGE_START),
            animation_range_end: initial(property_id::ANIMATION_RANGE_END),
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

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
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

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
        Self {
            background_color: crate::css::computed_value_types::StyleColor::rgba(0),
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

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
        let side = || ComputedBorderSide {
            color: crate::css::computed_value_types::StyleColor::rgba(0),
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
            border_shape: initial(property_id::BORDER_SHAPE),
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

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
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
            let Some(StyleValueData::CounterDefinitions(counter_definitions_payload)) = counters.data() else {
                continue;
            };
            let CounterDefinitionsPayload { counter_definitions } = &**counter_definitions_payload;
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
        let Some(StyleValueData::Content(content_payload)) = self.content.data() else {
            return facts;
        };
        let ContentPayload { content, alt_text } = &**content_payload;
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
            matches!(item.data(), StyleValueData::Counter(counter) if counter.counter_name.units() == LIST_ITEM_COUNTER_NAME)
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

impl InheritedListTableValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
        Self {
            list_style_type: initial(property_id::LIST_STYLE_TYPE),
            list_style_image: initial(property_id::LIST_STYLE_IMAGE),
            quotes: initial(property_id::QUOTES),
            border_spacing_horizontal: 0,
            border_spacing_vertical: 0,
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

impl MiscLayoutValues {
    fn initial() -> Self {
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id;

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
        Self {
            outline_offset_style_value: initial(property_id::OUTLINE_OFFSET),
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
            native_appearance_disabled: false,
            outline_style: 1,
            object_fit: 0,
            column_height: ComputedSize::keyword(ComputedSizeKind::Auto),
            outline_color: crate::css::computed_value_types::StyleColor::CURRENT_COLOR,
            outline_width: CssPixels::from_integer(3),
            column_rule_color: crate::css::computed_value_types::StyleColor::CURRENT_COLOR,
            column_rule_width: CssPixels::from_integer(3),
            outline_offset: CssPixels::from_raw(0),
            object_position_x: ComputedStyleValueHandle::percentage(50.0),
            object_position_y: ComputedStyleValueHandle::percentage(50.0),
            shape_image_threshold: 0.0,
            shape_margin: initial(property_id::SHAPE_MARGIN),
            shape_outside: initial(property_id::SHAPE_OUTSIDE),
            content_visibility: crate::css::css_enums::content_visibility::VISIBLE,
        }
    }
}

impl MiscInteractionValues {
    fn initial() -> Self {
        use crate::css::property_metadata::property_id;

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
        Self {
            scroll_margin: ComputedLengthBox::zero(),
            scroll_padding: ComputedLengthBox::auto(),
            user_select: 1,
            view_transition_name: initial(property_id::VIEW_TRANSITION_NAME),
            scroll_behavior: 0,
            scroll_snap_align_block: crate::css::css_enums::scroll_snap_align::NONE,
            scroll_snap_align_inline: crate::css::css_enums::scroll_snap_align::NONE,
            scroll_snap_stop: crate::css::css_enums::scroll_snap_stop::NORMAL,
            scroll_snap_axis: crate::css::css_enums::scroll_snap_axis::BOTH,
            scroll_snap_strictness: crate::css::css_enums::scroll_snap_strictness::NONE,
            scrollbar_gutter: 0,
            scrollbar_width: 0,
            overscroll_behavior_x: crate::css::css_enums::overscroll_behavior::AUTO,
            overscroll_behavior_y: crate::css::css_enums::overscroll_behavior::AUTO,
            will_change: initial(property_id::WILL_CHANGE),
            implicit_will_change: RetainedWillChangePropertyList::from_vec(Vec::new()),
        }
    }
}

impl FontValues {
    fn initial() -> Self {
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id;

        let initial = |property| {
            ComputedStyleValueHandle::from_retained(crate::css::style_compute::initial_value_handle(property))
        };
        Self {
            font_size: CssPixels::from_integer(16),
            resolved_line_height_used: CssPixels::from_raw(0),
            font_variant_emoji: 0,
            resolved_font_ascent: 0.0,
            resolved_font_descent: 0.0,
            resolved_font_x_height: 0.0,
            resolved_font_zero_advance: 0.0,
            resolved_first_available_font: std::ptr::null(),
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
            font_synthesis_weight: initial(property_id::FONT_SYNTHESIS_WEIGHT),
            font_synthesis_style: initial(property_id::FONT_SYNTHESIS_STYLE),
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

// Layout only needs the position keyword, which is the last one in a list such as `safe center`.
fn alignment_position_keyword(data: &RetainedStyleValueData, is_content: bool) -> Option<u16> {
    use crate::css::css_enums::keyword as k;
    use crate::css::style_value::StyleValueData;

    match data.optional_data()? {
        StyleValueData::Keyword { keyword } => Some(*keyword),
        StyleValueData::ValueList { values, .. } => match values.as_slice() {
            [first, _]
                if !is_content
                    && matches!(
                        first.optional_data(),
                        Some(StyleValueData::Keyword { keyword: k::LAST })
                    ) =>
            {
                // https://drafts.csswg.org/css-align-3/#baseline-values
                // The fallback alignment for last baseline is safe self-end (for self-alignment) or safe end (for
                // content-distribution).
                // FIXME: Participate in last-baseline alignment instead of always using the fallback alignment.
                Some(k::SELF_END)
            }
            values => match values.last()?.optional_data()? {
                StyleValueData::Keyword { keyword } => Some(*keyword),
                _ => None,
            },
        },
        _ => None,
    }
}

fn alignment_is_last_baseline(data: &RetainedStyleValueData) -> bool {
    matches!(data.optional_data(), Some(StyleValueData::ValueList { values, .. })
        if matches!(values.as_slice().first().and_then(|value| value.optional_data()),
            Some(StyleValueData::Keyword { keyword: crate::css::css_enums::keyword::LAST })))
}

/// The explicit overflow alignment of a computed alignment value.
fn alignment_overflow(data: &RetainedStyleValueData) -> u8 {
    use crate::css::css_enums::keyword as k;
    use crate::css::style_value::StyleValueData;

    let Some(StyleValueData::ValueList { values, .. }) = data.optional_data() else {
        return crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT;
    };
    match values.as_slice().first().and_then(|value| value.optional_data()) {
        Some(StyleValueData::Keyword { keyword: k::SAFE }) => crate::css::computed_value_types::OVERFLOW_ALIGNMENT_SAFE,
        Some(StyleValueData::Keyword { keyword: k::UNSAFE }) => {
            crate::css::computed_value_types::OVERFLOW_ALIGNMENT_UNSAFE
        }
        // NB: Last baseline computes to its fallback alignment, which is safe.
        Some(StyleValueData::Keyword { keyword: k::LAST }) => crate::css::computed_value_types::OVERFLOW_ALIGNMENT_SAFE,
        _ => crate::css::computed_value_types::OVERFLOW_ALIGNMENT_DEFAULT,
    }
}

/// Builds the complete alignment group from its computed property values.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, and shared payloads are retained.
#[allow(clippy::too_many_arguments)]
pub(crate) fn rust_build_alignment_group(
    group_index: usize,
    webkit_box_orient: &RetainedStyleValueData,
    flex_direction: &RetainedStyleValueData,
    flex_wrap: &RetainedStyleValueData,
    flex_basis: &RetainedStyleValueData,
    flex_grow: f64,
    flex_shrink: f64,
    order: i32,
    align_content: &RetainedStyleValueData,
    align_items: &RetainedStyleValueData,
    align_self: &RetainedStyleValueData,
    justify_content: &RetainedStyleValueData,
    justify_items: &RetainedStyleValueData,
    justify_self: &RetainedStyleValueData,
    column_gap: &RetainedStyleValueData,
    row_gap: &RetainedStyleValueData,
    parent: GroupShares<'_>,
) -> *const c_void {
    use crate::css::css_enums;
    use crate::css::style_value::StyleValueData;

    let build_or_reuse_group_payload = || {
        let keyword_code = |data: &RetainedStyleValueData, map: fn(u16) -> Option<u8>| -> Option<u8> {
            match data.optional_data() {
                Some(StyleValueData::Keyword { keyword }) => map(*keyword),
                _ => None,
            }
        };
        let alignment_code = |data: &RetainedStyleValueData, map: fn(u16) -> Option<u8>, is_content: bool| {
            map(alignment_position_keyword(data, is_content)?)
        };
        let built = AlignmentValues {
            webkit_box_orient: keyword_code(webkit_box_orient, crate::css::css_enums::keyword_to_webkit_box_orient)?,
            flex_direction: keyword_code(flex_direction, crate::css::css_enums::keyword_to_flex_direction)?,
            flex_wrap: keyword_code(flex_wrap, crate::css::css_enums::keyword_to_flex_wrap)?,
            flex_basis: ComputedFlexBasis::from_data(flex_basis),
            flex_grow,
            flex_shrink,
            order,
            align_content: alignment_code(align_content, css_enums::keyword_to_align_content, true)?,
            align_items: alignment_code(align_items, css_enums::keyword_to_align_items, false)?,
            align_self: alignment_code(align_self, css_enums::keyword_to_align_self, false)?,
            align_items_is_last_baseline: alignment_is_last_baseline(align_items),
            align_self_is_last_baseline: alignment_is_last_baseline(align_self),
            align_content_is_last_baseline: alignment_is_last_baseline(align_content),
            justify_content: alignment_code(justify_content, css_enums::keyword_to_justify_content, true)?,
            justify_items: alignment_code(justify_items, css_enums::keyword_to_justify_items, false)?,
            justify_self: alignment_code(justify_self, css_enums::keyword_to_justify_self, false)?,
            align_items_overflow: alignment_overflow(align_items),
            justify_items_overflow: alignment_overflow(justify_items),
            align_self_overflow: alignment_overflow(align_self),
            justify_self_overflow: alignment_overflow(justify_self),
            align_content_overflow: alignment_overflow(align_content),
            justify_content_overflow: alignment_overflow(justify_content),
            column_gap: ComputedGap::from_data(column_gap),
            row_gap: ComputedGap::from_data(row_gap),
        };

        Some(share_or_publish_group_payload::<AlignmentValues>(
            group_index,
            built,
            parent,
        ))
    };
    build_or_reuse_group_payload().unwrap_or(std::ptr::null())
}

/// Interns a complete non-inherited SVG geometry and painting group.
///
/// The returned payload carries one reference for the caller; fresh payloads
/// start at one, and shared payloads are retained.
pub(crate) fn build_svg_reset_group_payload(
    group_index: usize,
    built: SVGResetValues,
    parent: GroupShares<'_>,
) -> *const c_void {
    abort_on_panic(|| share_or_publish_group_payload::<SVGResetValues>(group_index, built, parent))
}

/// Builds the complete text-decoration reset group from its lowered values.
#[allow(clippy::too_many_arguments)]
pub(crate) fn rust_build_text_reset_group(
    group_index: usize,
    lines: &[u8],
    text_decoration_thickness_kind: u8,
    text_decoration_thickness: &RetainedStyleValueData,
    text_decoration_style: u8,
    text_decoration_color: crate::css::computed_value_types::StyleColor,
    white_space_trim_discard_before: bool,
    white_space_trim_discard_after: bool,
    white_space_trim_discard_inner: bool,
    parent: GroupShares<'_>,
) -> *const c_void {
    let built = TextResetValues {
        text_decoration_lines: RetainedTextDecorationLineList::from_vec(lines.to_vec()),
        text_decoration_thickness_kind,
        text_decoration_thickness: if text_decoration_thickness_kind == 2 {
            ComputedStyleValueHandle::from_retained(text_decoration_thickness)
        } else {
            ComputedStyleValueHandle::empty()
        },
        text_decoration_style,
        text_decoration_color,
        white_space_trim_discard_before,
        white_space_trim_discard_after,
        white_space_trim_discard_inner,
    };

    share_or_publish_group_payload::<TextResetValues>(group_index, built, parent)
}

/// Builds the complete surround group from the physical inset, margin, and
/// padding properties. Anchor insets retain their original value separately
/// while exposing auto through the length-box facade, matching layout's
/// existing representation.
#[allow(clippy::too_many_arguments)]
pub(crate) fn rust_build_surround_group(
    group_index: usize,
    top: &RetainedStyleValueData,
    right: &RetainedStyleValueData,
    bottom: &RetainedStyleValueData,
    left: &RetainedStyleValueData,
    margin_top: &RetainedStyleValueData,
    margin_right: &RetainedStyleValueData,
    margin_bottom: &RetainedStyleValueData,
    margin_left: &RetainedStyleValueData,
    padding_top: &RetainedStyleValueData,
    padding_right: &RetainedStyleValueData,
    padding_bottom: &RetainedStyleValueData,
    padding_left: &RetainedStyleValueData,
    position_anchor: Option<&RetainedStyleValueData>,
    parent: GroupShares<'_>,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;

    let build_or_reuse_group_payload = || {
        let anchor = |data: &RetainedStyleValueData| {
            if matches!(
                data.optional_data(),
                Some(StyleValueData::Anchor { .. } | StyleValueData::AnchorSize { .. })
            ) {
                ComputedStyleValueHandle::from_retained(data)
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
            position_anchor: position_anchor
                .map_or_else(ComputedStyleValueHandle::empty, ComputedStyleValueHandle::from_retained),
            margin: ComputedLengthBox::from_data(margin_top, margin_right, margin_bottom, margin_left, false),
            padding: ComputedLengthBox::from_data(padding_top, padding_right, padding_bottom, padding_left, false),
        };

        if let Some(shared) = share_group_payload::<SurroundValues>(group_index, &built, parent) {
            return Some(shared);
        }

        // Only a fresh payload builds wrappers: equality excludes them, and a
        // shared parent payload already carries its own.
        let wrapper = |handle: &ComputedStyleValueHandle| {
            if handle.pointer.is_null() {
                return ComputedStyleValueHandle::empty();
            }
            // SAFETY: The handle retains anchor style value data.
            let calculated = unsafe { crate::css::calc::create_anchor_function_calculated(handle.pointer.cast()) };
            ComputedStyleValueHandle {
                pointer: StyleValueArc::into_raw(calculated).cast(),
            }
        };
        built.top_anchor_inset_wrapper = wrapper(&built.top_anchor_inset);
        built.right_anchor_inset_wrapper = wrapper(&built.right_anchor_inset);
        built.bottom_anchor_inset_wrapper = wrapper(&built.bottom_anchor_inset);
        built.left_anchor_inset_wrapper = wrapper(&built.left_anchor_inset);

        Some(publish_group_payload(group_index, built))
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
/// start at one, and shared payloads are retained.
#[allow(clippy::too_many_arguments)]
pub(crate) fn rust_build_box_group(group_index: usize, built: BoxValues, parent: GroupShares<'_>) -> *const c_void {
    share_or_publish_group_payload::<BoxValues>(group_index, built, parent)
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn rust_build_grid_group(group_index: usize, built: GridValues, parent: GroupShares<'_>) -> *const c_void {
    share_or_publish_group_payload::<GridValues>(group_index, built, parent)
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
#[allow(clippy::too_many_arguments)]
pub(crate) fn build_sizing_group(
    group_index: usize,
    width: &RetainedStyleValueData,
    min_width: &RetainedStyleValueData,
    max_width: &RetainedStyleValueData,
    height: &RetainedStyleValueData,
    min_height: &RetainedStyleValueData,
    max_height: &RetainedStyleValueData,
    parent: GroupShares<'_>,
) -> *const c_void {
    let built = SizingValues {
        width: ComputedSize::from_data(width),
        min_width: ComputedSize::from_data(min_width),
        max_width: ComputedSize::from_data(max_width),
        height: ComputedSize::from_data(height),
        min_height: ComputedSize::from_data(min_height),
        max_height: ComputedSize::from_data(max_height),
    };

    share_or_publish_group_payload::<SizingValues>(group_index, built, parent)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_payload_type_names_the_group_it_is_dispatched_for() {
        for group in 0..group_index::COUNT {
            with_style_group_type!(group, Payload => assert_eq!(Payload::GROUP_INDEX, group));
        }
    }

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
    fn every_longhand_has_one_place_among_the_group_values() {
        use crate::css::computed_longhand_table::LONGHAND_COUNT;
        use crate::css::property_metadata::property_id;

        let layout = GroupValueLayout::get();
        let mut held = 0;
        for group in 0..group_index::COUNT {
            let mut values = (crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID)
                .map(|property| layout.place(LonghandID::new(property)))
                .filter(|place| {
                    *place != LonghandPlace::NONE
                        && place.bit_field().is_none()
                        && place.handle_offset().is_none()
                        && place.group() == group
                })
                .collect::<Vec<_>>();
            values.sort_unstable_by_key(|place| place.index());
            let typed_slots = usize::from(GROUPS_OF_TYPED_LONGHANDS & (1 << group) != 0);
            assert_eq!(values.len() + typed_slots, usize::from(layout.counts[group]));
            for (index, place) in values.into_iter().enumerate() {
                assert_eq!(place.index(), index);
                assert!(GroupValueLayout::reads(layout.reads_of(group), place));
                held += 1;
            }
            for (_, place) in GroupValueLayout::handles_of(group) {
                assert_eq!(place.group(), group);
                assert!(GroupValueLayout::reads(layout.reads_of(group), place));
                held += 1;
            }
        }
        let slotless = (crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
            ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID)
            .filter(|&property| !layout.holds_value(LonghandID::new(property)))
            .count();
        assert!(slotless > 0);
        let bits = StyleBitField::ALL
            .iter()
            .filter(|field| layout.place(LonghandID::new(field.property())).bit_field() == Some(**field))
            .count();
        assert_eq!(bits, StyleBitField::ALL.len());
        assert_eq!(held + slotless + bits, LONGHAND_COUNT);
        assert_eq!(
            layout.place(LonghandID::new(property_id::WIDTH)).group(),
            group_index::SIZING
        );
        assert_eq!(
            layout.place(LonghandID::new(property_id::ZOOM)).group(),
            group_index::BOX
        );
    }

    #[test]
    fn group_values_share_the_seed_groups_holding_the_same_values() {
        use crate::css::computed_longhand_table::LonghandTableFlags;
        use crate::css::property_metadata::property_id;

        let number = |value| RetainedStyleValueData::from_owned(StyleValueData::Number { value });
        let (color, initial_width) = (number(1.0), number(2.0));
        let defaults = DefaultGroups::default();
        let mut initial_groups: [*const c_void; group_index::COUNT] = std::array::from_fn(|group| {
            // SAFETY: The default payload is a live payload of its group.
            unsafe { clone_group_payload(group, defaults.groups()[group].as_ptr()) }.cast_const()
        });
        let initial_facts = with_default_seed(&defaults, |nothing| {
            let mut writer = GroupValuesWriter::seeded_for_child(
                nothing,
                nothing,
                std::iter::empty(),
                LonghandTableFlags::empty(),
                0,
            );
            writer.set(property_id::COLOR, color.clone());
            writer.set(property_id::WIDTH, initial_width.clone());
            let (initial_values, ..) = writer.finish(&[]);
            // SAFETY: Each group holds one reference to a fresh payload of its group.
            unsafe { initial_values.put_into(&mut initial_groups, 0) };
            initial_values.into_facts()
        });
        // SAFETY: The groups stay live until the end of the test.
        let initial_style_groups = unsafe { StyleGroups::retained(&initial_groups, StyleBits::INITIAL) };
        let seed = GroupSeed::new(&initial_style_groups, &initial_facts);
        let sizing = group_index::SIZING;

        // Only the group whose values the element changes holds its own.
        let width = number(3.0);
        let mut writer =
            GroupValuesWriter::seeded_for_child(seed, seed, std::iter::empty(), LonghandTableFlags::empty(), 0);
        writer.set(property_id::COLOR, color.clone());
        writer.set(property_id::WIDTH, width.clone());
        let (values, ..) = writer.finish(&[seed]);
        for group in 0..group_index::COUNT {
            assert_eq!(values.writes_of(group).is_empty(), group != sizing);
        }
        assert_eq!(values.value(property_id::WIDTH).pointer(), width.pointer());

        // A writer writing the same value holds the same values, and one writing the seed's value
        // back shares the seed's group again.
        let mut writer = GroupValuesWriter::seeded_for_child(
            seed,
            seed,
            std::iter::empty(),
            crate::css::computed_longhand_table::LonghandTableFlags::empty(),
            0,
        );
        writer.set(property_id::WIDTH, width.clone());
        let (written, ..) = writer.finish(&[seed]);
        assert!(!written.writes_of(sizing).is_empty());
        assert_eq!(written.value(property_id::WIDTH).pointer(), width.pointer());
        let mut writer = GroupValuesWriter::seeded_for_child(
            seed,
            seed,
            std::iter::empty(),
            crate::css::computed_longhand_table::LonghandTableFlags::empty(),
            0,
        );
        writer.set(property_id::WIDTH, number(7.0));
        writer.set(property_id::WIDTH, initial_width.clone());
        let (restored, ..) = writer.finish(&[seed]);
        assert!(restored.hold_the_seed(sizing, initial_groups[sizing]));

        for (group, payload) in initial_groups.into_iter().enumerate() {
            release_group_payload(group, payload);
        }
    }

    #[test]
    fn computed_groups_release_what_no_style_adopted() {
        let defaults = DefaultGroups::default();
        let references = |pointers: &[*const c_void; group_index::COUNT]| -> Vec<usize> {
            pointers
                .iter()
                .enumerate()
                .map(|(group, &pointer)| refcount_of(pointer, payload_align(group)).load(Ordering::Relaxed))
                .collect()
        };
        // SAFETY: Each default payload is a live payload of its group.
        let held: [*const c_void; group_index::COUNT] = std::array::from_fn(|group| {
            unsafe { clone_group_payload(group, defaults.groups()[group].as_ptr()) }.cast_const()
        });
        let before = references(&held);
        let mut borrowed = BorrowedGroups::default();
        for &group in &INHERITED_SET_GROUPS {
            borrowed.note_if_of_a_set(group);
        }

        // A computation thrown away releases the references it took, and none of the groups it
        // borrowed.
        for (group, &pointer) in held.iter().enumerate() {
            if !borrowed.contains(group) {
                retain_group_payload(group, pointer);
            }
        }
        // SAFETY: Each group holds a reference for these groups but the borrowed ones.
        drop(unsafe { ComputedGroups::new(held, StyleBits::INITIAL, 0, borrowed) });
        assert_eq!(references(&held), before);

        // A computation a style adopts hands its references to the style.
        for (group, &pointer) in held.iter().enumerate() {
            if !borrowed.contains(group) {
                retain_group_payload(group, pointer);
            }
        }
        // SAFETY: Each group holds a reference for these groups but the borrowed ones.
        let adopted = unsafe { ComputedGroups::new(held, StyleBits::INITIAL, 0, borrowed) }.adopt_sharing(&[]);
        assert!(
            references(&held)
                .iter()
                .zip(&before)
                .all(|(after, before)| after == &(before + 1))
        );
        drop(adopted);
        assert_eq!(references(&held), before);

        for (group, payload) in held.into_iter().enumerate() {
            release_group_payload(group, payload);
        }
    }

    #[test]
    fn payload_lifecycle() {
        let defaults = DefaultGroups::default();
        for group_index in 0..group_index::COUNT {
            let default = defaults.groups()[group_index].as_ptr();
            // SAFETY: The default payload is a live payload of its group.
            let clone = unsafe { clone_group_payload(group_index, default) };
            // SAFETY: The payloads are live defaults and clones of this group.
            assert!(unsafe { style_group_payloads_equal(group_index, default, clone) });
            refcount_of(clone, payload_align(group_index)).store(0, Ordering::Relaxed);
            // SAFETY: The clone holds the last reference and is a payload of this group.
            unsafe { rust_style_group_free(group_index, clone) };
        }

        let table_default = defaults.groups()[group_index::INHERITED_LIST_TABLE].as_ptr();
        // SAFETY: The default payload is a live payload of its group.
        let changed_payload = unsafe { clone_group_payload(group_index::INHERITED_LIST_TABLE, table_default) };
        // SAFETY: The clone is uniquely owned and a list and table group.
        unsafe { &mut *changed_payload.cast::<InheritedListTableValues>() }.border_spacing_horizontal = 17;
        // SAFETY: The payloads are live payloads of this group.
        assert!(!unsafe {
            style_group_payloads_equal(group_index::INHERITED_LIST_TABLE, table_default, changed_payload)
        });
        refcount_of(changed_payload, payload_align(group_index::INHERITED_LIST_TABLE)).store(0, Ordering::Relaxed);
        // SAFETY: The clone holds the last reference and is a payload of this group.
        unsafe { rust_style_group_free(group_index::INHERITED_LIST_TABLE, changed_payload) };
    }
}

pub(crate) fn copy_fieldset_content_alignment(source: &AlignmentValues, target: &mut AlignmentValues) {
    target.flex_direction = source.flex_direction;
    target.flex_wrap = source.flex_wrap;
    target.align_content = source.align_content;
    target.align_content_is_last_baseline = source.align_content_is_last_baseline;
    target.align_content_overflow = source.align_content_overflow;
    target.align_items = source.align_items;
    target.align_items_is_last_baseline = source.align_items_is_last_baseline;
    target.align_items_overflow = source.align_items_overflow;
    target.justify_content = source.justify_content;
    target.justify_content_overflow = source.justify_content_overflow;
    target.justify_items = source.justify_items;
    target.justify_items_overflow = source.justify_items_overflow;
    target.row_gap = source.row_gap.clone();
    target.column_gap = source.column_gap.clone();
}
