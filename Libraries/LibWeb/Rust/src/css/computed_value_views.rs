/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lazy readers over the computed style group payload types, mirroring the
//! C++ `CSS::Size` / `CSS::LengthPercentage` method surface: every query
//! resolves on demand from the stored computed representation, so readers
//! need no intermediate decoded value.

use crate::css::calc;
use crate::css::computed_value_types::{
    AlignmentValues, BackgroundValues, BorderLayoutFacts, BorderValues, BoxValues, ComputedAspectRatio, ComputedGap,
    ComputedLengthPercentageOrAuto, ComputedSize, ComputedSizeKind, ComputedStyleValueHandle, EffectsValues,
    FontLayoutFacts, FontValues, GridValues, InheritedListValues, InheritedSVGValues, InheritedTextLayoutFacts,
    InheritedTextValues, InheritedUIValues, MaskValues, MiscResetValues, STYLE_GROUP_INDEX_ALIGNMENT,
    STYLE_GROUP_INDEX_BACKGROUND, STYLE_GROUP_INDEX_BORDER, STYLE_GROUP_INDEX_BOX, STYLE_GROUP_INDEX_EFFECTS,
    STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_GRID, STYLE_GROUP_INDEX_INHERITED_BOX, STYLE_GROUP_INDEX_INHERITED_LIST,
    STYLE_GROUP_INDEX_INHERITED_SVG, STYLE_GROUP_INDEX_INHERITED_TABLE, STYLE_GROUP_INDEX_INHERITED_TEXT,
    STYLE_GROUP_INDEX_INHERITED_UI, STYLE_GROUP_INDEX_MASK, STYLE_GROUP_INDEX_MISC_RESET, STYLE_GROUP_INDEX_SIZING,
    STYLE_GROUP_INDEX_SURROUND, STYLE_GROUP_INDEX_SVG_RESET, STYLE_GROUP_INDEX_TEXT_RESET, STYLE_GROUP_INDEX_TRANSFORM,
    SVGResetValues, SizingValues, SurroundValues, TextResetValues, TransformValues,
};
use crate::css::computed_values::{InheritedBoxValues, InheritedTableValues};
use crate::css::css_enums::{direction, writing_mode};
use crate::css::css_pixels::CssPixels;
use crate::css::display::FfiDisplay;
use crate::css::style_value::StyleValueData;
use std::ffi::c_void;

/// The used-value truncation the C++ layout engine applies when resolving
/// percentages: truncate toward zero at 1/64 precision, collapse NaN to zero,
/// and saturate the raw value.
pub(crate) fn truncated_css_pixels(value: f64) -> CssPixels {
    if value.is_nan() {
        return CssPixels::default();
    }
    let raw = (value * 64.0).trunc();
    CssPixels::from_raw(raw.clamp(i32::MIN as f64, i32::MAX as f64) as i32)
}

pub(crate) fn px_calc_resolution_context(percentage_basis: CssPixels) -> calc::FfiCalcResolutionContext {
    calc::FfiCalcResolutionContext {
        basis_kind: 3,
        basis_value: percentage_basis.to_double(),
        basis_unit: crate::css::style_compute::px_length_unit(),
        length_resolution_context: std::ptr::null(),
        external_resolutions: std::ptr::null(),
        external_resolution_count: 0,
    }
}

pub(crate) fn resolve_calc_to_px(calculated: *const c_void, percentage_basis: CssPixels) -> CssPixels {
    assert!(!calculated.is_null());
    // SAFETY: The style value stays alive for the pass.
    let value = unsafe { &*calculated.cast::<StyleValueData>() };
    let resolved = crate::css::calc::resolve_calculated_length_without_context(value, percentage_basis.to_double())
        .expect("computed length-percentage calc failed to resolve");
    CssPixels::nearest_value_for(resolved)
}

/// A borrowed computed `<length-percentage>`: a retained length, percentage
/// or calculated style value read in place, the Rust twin of the C++
/// `LengthPercentage::view` API.
#[derive(Clone, Copy)]
pub(crate) struct LengthPercentageRef<'a> {
    value: &'a StyleValueData,
}

impl<'a> LengthPercentageRef<'a> {
    pub(crate) fn over(value: &'a StyleValueData) -> Self {
        Self { value }
    }
}

impl LengthPercentageRef<'_> {
    pub(crate) fn is_calculated(self) -> bool {
        matches!(self.value, StyleValueData::Calculated { .. })
    }

    pub(crate) fn absolute_length_to_px(self) -> CssPixels {
        let StyleValueData::Length { value, unit } = self.value else {
            unreachable!("computed length-percentage read as a length holds another style value");
        };
        let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
        assert!(ratio.is_finite(), "computed length is not absolute");
        CssPixels::nearest_value_for(value * ratio)
    }

    /// Matches Percentage::as_fraction(); the multiplication order is
    /// observable for some f64 inputs.
    pub(crate) fn as_fraction(self) -> f64 {
        let StyleValueData::Percentage { value } = self.value else {
            unreachable!("computed length-percentage read as a percentage holds another style value");
        };
        value * 0.01
    }

    /// The retained calculated style value, for handing to calc resolution.
    pub(crate) fn calculated_pointer(self) -> *const c_void {
        assert!(self.is_calculated());
        std::ptr::from_ref(self.value).cast()
    }

    pub(crate) fn contains_percentage(self) -> bool {
        match self.value {
            StyleValueData::Length { .. } => false,
            StyleValueData::Percentage { .. } => true,
            StyleValueData::Calculated { .. } => {
                // SAFETY: The calculated style value outlives this borrowed
                // view, and the root query takes no other state.
                let root = unsafe { calc::rust_calc_root_from_calculated(self.calculated_pointer()) };
                assert!(!root.is_null());
                // SAFETY: The root borrows the same retained calculation.
                unsafe { calc::rust_calc_node_contains_percentage(root) }
            }
            _ => unreachable!("computed length-percentage holds a non-length-percentage style value"),
        }
    }

    pub(crate) fn contains_anchor_function(self) -> bool {
        // SAFETY: The calculated style value outlives this borrowed view.
        self.is_calculated() && unsafe { calc::rust_calc_contains_anchor(self.calculated_pointer()) }
    }

    pub(crate) fn to_px(self, reference: CssPixels) -> CssPixels {
        match self.value {
            StyleValueData::Length { .. } => self.absolute_length_to_px(),
            StyleValueData::Percentage { .. } => truncated_css_pixels(reference.to_double() * self.as_fraction()),
            StyleValueData::Calculated { .. } => resolve_calc_to_px(self.calculated_pointer(), reference),
            _ => unreachable!("computed length-percentage holds a non-length-percentage style value"),
        }
    }
}

impl ComputedStyleValueHandle {
    /// The lifetime is the caller's to choose: the referenced style value is
    /// retained by whatever owns the handle, not by the handle borrow itself.
    pub(crate) fn length_percentage<'a>(&self) -> Option<LengthPercentageRef<'a>> {
        if self.pointer.is_null() {
            return None;
        }
        // SAFETY: A non-null handle points at the retained style value owned
        // by the node's style group payload, which outlives every reader.
        Some(LengthPercentageRef {
            value: unsafe { &*self.pointer.cast::<StyleValueData>() },
        })
    }
}

impl ComputedSize {
    pub(crate) fn is_auto(&self) -> bool {
        self.kind == ComputedSizeKind::Auto
    }

    pub(crate) fn is_length(&self) -> bool {
        self.kind == ComputedSizeKind::Length
    }

    pub(crate) fn is_percentage(&self) -> bool {
        self.kind == ComputedSizeKind::Percentage
    }

    pub(crate) fn is_min_content(&self) -> bool {
        self.kind == ComputedSizeKind::MinContent
    }

    pub(crate) fn is_max_content(&self) -> bool {
        self.kind == ComputedSizeKind::MaxContent
    }

    pub(crate) fn is_fit_content(&self) -> bool {
        self.kind == ComputedSizeKind::FitContent
    }

    pub(crate) fn is_none(&self) -> bool {
        self.kind == ComputedSizeKind::None
    }

    pub(crate) fn is_length_percentage(&self) -> bool {
        matches!(
            self.kind,
            ComputedSizeKind::Calculated | ComputedSizeKind::Length | ComputedSizeKind::Percentage
        )
    }

    pub(crate) fn is_intrinsic_sizing_constraint(&self) -> bool {
        matches!(
            self.kind,
            ComputedSizeKind::MinContent | ComputedSizeKind::MaxContent | ComputedSizeKind::FitContent
        )
    }

    /// The length-percentage term of a Length, Percentage or Calculated size.
    pub(crate) fn length_percentage(&self) -> LengthPercentageRef<'_> {
        debug_assert!(self.is_length_percentage());
        self.value
            .length_percentage()
            .expect("computed length-percentage size lost its style value")
    }

    /// The fit-content argument; None for the keyword-only form.
    pub(crate) fn fit_content_available_space(&self) -> Option<LengthPercentageRef<'_>> {
        debug_assert!(self.is_fit_content());
        self.value.length_percentage()
    }

    pub(crate) fn to_px(&self, reference: CssPixels) -> CssPixels {
        match self.kind {
            ComputedSizeKind::Auto
            | ComputedSizeKind::MinContent
            | ComputedSizeKind::MaxContent
            | ComputedSizeKind::None => CssPixels::default(),
            ComputedSizeKind::Calculated | ComputedSizeKind::Length | ComputedSizeKind::Percentage => {
                self.length_percentage().to_px(reference)
            }
            ComputedSizeKind::FitContent => self
                .fit_content_available_space()
                .map_or(CssPixels::default(), |available_space| available_space.to_px(reference)),
        }
    }

    pub(crate) fn contains_percentage(&self) -> bool {
        match self.kind {
            ComputedSizeKind::Auto
            | ComputedSizeKind::MinContent
            | ComputedSizeKind::MaxContent
            | ComputedSizeKind::None => false,
            ComputedSizeKind::Calculated | ComputedSizeKind::Length | ComputedSizeKind::Percentage => {
                self.length_percentage().contains_percentage()
            }
            ComputedSizeKind::FitContent => self
                .fit_content_available_space()
                .is_some_and(|available_space| available_space.contains_percentage()),
        }
    }
}

impl ComputedLengthPercentageOrAuto {
    pub(crate) fn is_auto(&self) -> bool {
        self.is_auto
    }

    pub(crate) fn length_percentage(&self) -> Option<LengthPercentageRef<'_>> {
        if self.is_auto {
            return None;
        }
        Some(
            self.value
                .length_percentage()
                .expect("non-auto computed length-percentage lost its style value"),
        )
    }

    pub(crate) fn to_px(&self, reference: CssPixels) -> CssPixels {
        self.length_percentage()
            .map_or(CssPixels::default(), |value| value.to_px(reference))
    }

    pub(crate) fn contains_percentage(&self) -> bool {
        self.length_percentage()
            .is_some_and(|value| value.contains_percentage())
    }
}

impl ComputedGap {
    pub(crate) fn is_normal(&self) -> bool {
        self.is_normal
    }

    pub(crate) fn length_percentage(&self) -> Option<LengthPercentageRef<'_>> {
        if self.is_normal {
            return None;
        }
        Some(
            self.value
                .length_percentage()
                .expect("non-normal computed gap lost its style value"),
        )
    }

    pub(crate) fn to_px(&self, reference: CssPixels) -> CssPixels {
        self.length_percentage()
            .map_or(CssPixels::default(), |value| value.to_px(reference))
    }
}

struct SyncComputedSize(ComputedSize);

// SAFETY: The shared value's handle is null, so there is no pointee to race
// on.
unsafe impl Sync for SyncComputedSize {}

static AUTO_COMPUTED_SIZE: SyncComputedSize = SyncComputedSize(ComputedSize {
    kind: ComputedSizeKind::Auto,
    value: ComputedStyleValueHandle {
        pointer: std::ptr::null(),
    },
});

/// A shared computed `auto` size for readers that substitute auto for a
/// stored size.
pub(crate) fn auto_computed_size() -> &'static ComputedSize {
    &AUTO_COMPUTED_SIZE.0
}

// https://drafts.csswg.org/css-contain-2/#containment-types
fn containment_applies_to_principal_box(display: FfiDisplay) -> bool {
    if display.is_internal_table() && !display.is_table_cell() {
        return false;
    }
    if display.is_inline_outside() && display.is_flow_inside() {
        return false;
    }
    true
}

/// The computed `aspect-ratio` <ratio> term as a CSSPixels fraction. A zero
/// denominator means no usable ratio (none specified, degenerate, or collapsed
/// to zero by the fixed-point conversion).
fn decode_css_preferred_aspect_ratio(ratio: &ComputedAspectRatio) -> (CssPixels, CssPixels) {
    let no_usable_ratio = (CssPixels::default(), CssPixels::default());
    if !ratio.has_preferred_ratio {
        return no_usable_ratio;
    }
    let numerator = ratio.preferred_ratio_numerator;
    let denominator = ratio.preferred_ratio_denominator;
    let is_degenerate = !numerator.is_finite() || numerator == 0.0 || !denominator.is_finite() || denominator == 0.0;
    if is_degenerate {
        return no_usable_ratio;
    }
    let (numerator, denominator) = CssPixels::fraction_nearest_values_for(numerator, denominator);
    if numerator.raw_value() == 0 {
        return no_usable_ratio;
    }
    (numerator, denominator)
}

/// A borrowed view over one node's computed style group payloads: the Rust
/// twin of the C++ ComputedValues accessor surface. Every read hands out
/// payload references or lazy views; the payloads must stay alive and
/// unchanged while the view or anything borrowed from it is in use.
#[derive(Clone, Copy)]
pub(crate) struct ComputedValuesView<'a> {
    groups: &'a [*const c_void],
}

macro_rules! scalar_accessors {
    ($($group:ident: { $($name:ident: $ty:ty => $($field:ident).+,)+ })+) => {
        impl ComputedValuesView<'_> {
            $($(
                #[inline]
                pub(crate) fn $name(self) -> $ty {
                    self.$group().$($field).+
                }
            )+)+
        }
    };
}

scalar_accessors! {
    box_values: {
        display: FfiDisplay => display,
        display_before_box_type_transformation: FfiDisplay => display_before_box_type_transformation,
        position: u8 => position,
        float_: u8 => float_,
        clear: u8 => clear,
        box_sizing: u8 => box_sizing,
        overflow_x: u8 => overflow_x,
        overflow_y: u8 => overflow_y,
        text_overflow: u8 => text_overflow,
        table_layout: u8 => table_layout,
        unicode_bidi: u8 => unicode_bidi,
        grid_auto_flow_row: bool => grid_auto_flow_row,
        grid_auto_flow_dense: bool => grid_auto_flow_dense,
        has_column_count: bool => column_count_has_value,
        column_count: i32 => column_count,
        has_size_containment: bool => size_containment,
        is_size_container: bool => is_size_container,
        aspect_ratio_uses_natural_when_available: bool => aspect_ratio.use_natural_aspect_ratio_if_available,
    }
    border_facts: {
        border_top_width: CssPixels => border_top.width,
        border_right_width: CssPixels => border_right.width,
        border_bottom_width: CssPixels => border_bottom.width,
        border_left_width: CssPixels => border_left.width,
        border_top_style: u8 => border_top.line_style,
        border_right_style: u8 => border_right.line_style,
        border_bottom_style: u8 => border_bottom.line_style,
        border_left_style: u8 => border_left.line_style,
        border_top_color: u32 => border_top.color,
        border_right_color: u32 => border_right.color,
        border_bottom_color: u32 => border_bottom.color,
        border_left_color: u32 => border_left.color,
    }
    inherited_box: {
        writing_mode: u8 => writing_mode,
        direction: u8 => direction,
        visibility: u8 => visibility,
    }
    inherited_table: {
        border_collapse: u8 => border_collapse,
        caption_side: u8 => caption_side,
    }
    inherited_text_facts: {
        text_align: u8 => text_align,
        text_justify: u8 => text_justify,
        white_space_collapse: u8 => white_space_collapse,
        text_wrap_mode: u8 => text_wrap_mode,
        word_break: u8 => word_break,
        letter_spacing: CssPixels => letter_spacing,
        word_spacing: CssPixels => word_spacing,
        text_indent_each_line: bool => text_indent.each_line,
        text_indent_hanging: bool => text_indent.hanging,
        tab_size_is_number: bool => tab_size_is_number,
        tab_size: CssPixels => tab_size_length,
        tab_size_number: f64 => tab_size_number,
    }
    font_facts: {
        font_variant_emoji: u8 => font_variant_emoji,
        line_height: CssPixels => line_height_used,
        font_size: CssPixels => font_size,
        font_ascent: f32 => font_ascent,
        font_descent: f32 => font_descent,
        font_x_height: f32 => font_x_height,
    }
    alignment: {
        flex_direction: u8 => flex_direction,
        flex_wrap: u8 => flex_wrap,
        flex_grow: f64 => flex_grow,
        flex_shrink: f64 => flex_shrink,
        order: i32 => order,
        align_items: u8 => align_items,
        align_self: u8 => align_self,
        align_content: u8 => align_content,
        justify_content: u8 => justify_content,
        justify_items: u8 => justify_items,
        justify_self: u8 => justify_self,
        flex_basis_is_content: bool => flex_basis.is_content,
    }
}

macro_rules! reference_accessors {
    ($($group:ident: { $($name:ident: $ty:ty => $($field:ident).+,)+ })+) => {
        impl<'a> ComputedValuesView<'a> {
            $($(
                #[inline]
                pub(crate) fn $name(self) -> &'a $ty {
                    &self.$group().$($field).+
                }
            )+)+
        }
    };
}

reference_accessors! {
    sizing: {
        width: ComputedSize => width,
        height: ComputedSize => height,
        min_width: ComputedSize => min_width,
        min_height: ComputedSize => min_height,
        max_width: ComputedSize => max_width,
        max_height: ComputedSize => max_height,
    }
    box_values: {
        column_width: ComputedSize => column_width,
    }
    surround: {
        margin_top: ComputedLengthPercentageOrAuto => margin.top,
        margin_right: ComputedLengthPercentageOrAuto => margin.right,
        margin_bottom: ComputedLengthPercentageOrAuto => margin.bottom,
        margin_left: ComputedLengthPercentageOrAuto => margin.left,
        padding_top: ComputedLengthPercentageOrAuto => padding.top,
        padding_right: ComputedLengthPercentageOrAuto => padding.right,
        padding_bottom: ComputedLengthPercentageOrAuto => padding.bottom,
        padding_left: ComputedLengthPercentageOrAuto => padding.left,
    }
    alignment: {
        row_gap: ComputedGap => row_gap,
        column_gap: ComputedGap => column_gap,
    }
}

impl<'a> ComputedValuesView<'a> {
    #[inline]
    pub(crate) fn new(groups: &'a [*const c_void]) -> Self {
        Self { groups }
    }

    #[inline]
    fn native_group<T>(self, group_index: usize) -> &'a T {
        let payload = self.groups[group_index];
        debug_assert!(!payload.is_null());
        // SAFETY: The payload is the Rust-defined group struct itself; C++
        // derives its mirror from the cbindgen twin of the same type, and the
        // node's ComputedValues keep it alive while readers exist.
        unsafe { &*payload.cast::<T>() }
    }

    #[inline]
    fn sizing(self) -> &'a SizingValues {
        self.native_group(STYLE_GROUP_INDEX_SIZING)
    }

    #[inline]
    pub(crate) fn surround(self) -> &'a SurroundValues {
        self.native_group(STYLE_GROUP_INDEX_SURROUND)
    }

    #[inline]
    fn alignment(self) -> &'a AlignmentValues {
        self.native_group(STYLE_GROUP_INDEX_ALIGNMENT)
    }

    #[inline]
    pub(crate) fn svg_reset(self) -> &'a SVGResetValues {
        self.native_group(STYLE_GROUP_INDEX_SVG_RESET)
    }

    #[inline]
    fn inherited_box(self) -> &'a InheritedBoxValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_BOX)
    }

    #[inline]
    fn inherited_table(self) -> &'a InheritedTableValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TABLE)
    }

    #[inline]
    pub(crate) fn box_values(self) -> &'a BoxValues {
        self.native_group(STYLE_GROUP_INDEX_BOX)
    }

    #[inline]
    pub(crate) fn grid_values(self) -> &'a GridValues {
        self.native_group(STYLE_GROUP_INDEX_GRID)
    }

    #[inline]
    fn border_facts(self) -> &'a BorderLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_BORDER)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn content_visibility(self) -> u8 {
        self.inherited_box().content_visibility
    }

    #[inline]
    pub(crate) fn image_rendering(self) -> u8 {
        self.inherited_box().image_rendering
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn empty_cells(self) -> u8 {
        self.inherited_table().empty_cells
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn border(self) -> &'a BorderValues {
        self.native_group(STYLE_GROUP_INDEX_BORDER)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn inherited_list(self) -> &'a InheritedListValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_LIST)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn inherited_ui(self) -> &'a InheritedUIValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_UI)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn inherited_svg(self) -> &'a InheritedSVGValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_SVG)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn inherited_text(self) -> &'a InheritedTextValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TEXT)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn font(self) -> &'a FontValues {
        self.native_group(STYLE_GROUP_INDEX_FONT)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn effects(self) -> &'a EffectsValues {
        self.native_group(STYLE_GROUP_INDEX_EFFECTS)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn mask(self) -> &'a MaskValues {
        self.native_group(STYLE_GROUP_INDEX_MASK)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn text_reset(self) -> &'a TextResetValues {
        self.native_group(STYLE_GROUP_INDEX_TEXT_RESET)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn transform(self) -> &'a TransformValues {
        self.native_group(STYLE_GROUP_INDEX_TRANSFORM)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn background(self) -> &'a BackgroundValues {
        self.native_group(STYLE_GROUP_INDEX_BACKGROUND)
    }

    #[inline]
    #[allow(dead_code)]
    pub(crate) fn misc_reset(self) -> &'a MiscResetValues {
        self.native_group(STYLE_GROUP_INDEX_MISC_RESET)
    }

    #[inline]
    fn inherited_text_facts(self) -> &'a InheritedTextLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TEXT)
    }

    #[inline]
    fn font_facts(self) -> &'a FontLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_FONT)
    }

    pub(crate) fn is_floating(self) -> bool {
        self.box_values().float_ != crate::css::css_enums::float::NONE
    }

    pub(crate) fn inline_axis_is_reverse(self) -> bool {
        match self.writing_mode() {
            writing_mode::HORIZONTAL_TB
            | writing_mode::VERTICAL_RL
            | writing_mode::VERTICAL_LR
            | writing_mode::SIDEWAYS_RL => self.direction() == direction::RTL,
            writing_mode::SIDEWAYS_LR => self.direction() == direction::LTR,
            _ => unreachable!("invalid writing mode"),
        }
    }

    pub(crate) fn block_axis_is_reverse(self) -> bool {
        matches!(
            self.writing_mode(),
            writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL
        )
    }

    pub(crate) fn is_absolutely_positioned(self) -> bool {
        matches!(
            self.box_values().position,
            crate::css::css_enums::positioning::ABSOLUTE | crate::css::css_enums::positioning::FIXED
        )
    }

    // https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
    // The computed-style-only half of the block-formatting-context predicate;
    // node_creates_block_formatting_context adds the terms that need the node
    // kind, stamped DOM identity, the live IsFlexItem flag, or the parent's
    // display. The float term is deliberately absent for the same reason: only
    // non-flex-items establish one by floating.
    pub(crate) fn own_style_establishes_block_formatting_context(self) -> bool {
        let box_values = self.box_values();
        let display = box_values.display;

        if self.is_absolutely_positioned() {
            return true;
        }

        if display.is_inline_block() {
            return true;
        }

        if display.is_table_cell() || display.is_table_caption() {
            return true;
        }

        let overflow_establishes_context = |overflow: u8| {
            overflow != crate::css::css_enums::overflow::VISIBLE && overflow != crate::css::css_enums::overflow::CLIP
        };
        if overflow_establishes_context(box_values.overflow_x) || overflow_establishes_context(box_values.overflow_y) {
            return true;
        }

        if display.is_flow_root_inside() {
            return true;
        }

        // https://drafts.csswg.org/css-contain-2/#containment-types
        // 1. The layout containment box establishes an independent formatting context.
        // 4. The paint containment box establishes an independent formatting context.
        let content_visibility_forces_containment =
            self.inherited_box().content_visibility == crate::css::css_enums::content_visibility::AUTO;
        if (box_values.layout_containment || box_values.paint_containment || content_visibility_forces_containment)
            && containment_applies_to_principal_box(display)
        {
            return true;
        }

        // https://drafts.csswg.org/css-conditional-5/#valdef-container-type-size
        // Applies style containment and size containment to the principal box, and establishes an independent
        // formatting context.
        if box_values.is_size_container || box_values.is_inline_size_container {
            return true;
        }

        // https://drafts.csswg.org/css-multicol-2/#the-multi-column-model
        // An element whose 'column-width', 'column-count', or 'column-height' property is not 'auto' establishes a
        // multi-column container (or multicol container for short), and therefore acts as a container for
        // multi-column layout.
        if box_values.column_width.kind != ComputedSizeKind::Auto || box_values.column_count_has_value {
            return true;
        }

        false
    }

    pub(crate) fn x(self) -> LengthPercentageRef<'a> {
        self.svg_reset()
            .x
            .length_percentage()
            .expect("computed x lost its style value")
    }

    pub(crate) fn y(self) -> LengthPercentageRef<'a> {
        self.svg_reset()
            .y
            .length_percentage()
            .expect("computed y lost its style value")
    }

    pub(crate) fn text_indent(self) -> LengthPercentageRef<'a> {
        self.inherited_text_facts()
            .text_indent
            .length_percentage
            .length_percentage()
            .expect("computed text-indent lost its style value")
    }

    pub(crate) fn vertical_align_value(self) -> LengthPercentageRef<'a> {
        self.box_values()
            .vertical_align
            .value
            .length_percentage()
            .expect("computed vertical-align lost its style value")
    }

    pub(crate) fn has_position_anchor(self) -> bool {
        !self.surround().position_anchor.pointer.is_null()
    }

    /// The raw fly-string representation of the computed position-anchor
    /// name, borrowed from the surround payload for the duration of the pass.
    pub(crate) fn position_anchor_name(self) -> usize {
        let pointer = self.surround().position_anchor.pointer.cast::<StyleValueData>();
        match unsafe { pointer.as_ref() } {
            Some(StyleValueData::CustomIdent { custom_ident }) => custom_ident.raw(),
            _ => 0,
        }
    }

    pub(crate) fn first_available_font(self) -> *const c_void {
        let font = self.font_facts().first_available_font;
        debug_assert!(
            !font.is_null(),
            "layout read a font group that never received a font list"
        );
        font
    }

    pub(crate) fn font_cascade_list(self) -> *const c_void {
        let list = self.font_facts().font_cascade_list;
        debug_assert!(
            !list.is_null(),
            "layout read a font group that never received a font list"
        );
        list
    }

    pub(crate) fn box_sizing_for_aspect_ratio(self) -> u8 {
        let values = self.box_values();
        if values.aspect_ratio.use_natural_aspect_ratio_if_available {
            crate::css::css_enums::box_sizing::CONTENT_BOX
        } else {
            values.box_sizing
        }
    }

    pub(crate) fn css_preferred_aspect_ratio(self) -> (CssPixels, CssPixels) {
        decode_css_preferred_aspect_ratio(&self.box_values().aspect_ratio)
    }

    pub(crate) fn border_spacing_horizontal(self) -> CssPixels {
        CssPixels::from_raw(self.inherited_table().border_spacing_horizontal)
    }

    pub(crate) fn border_spacing_vertical(self) -> CssPixels {
        CssPixels::from_raw(self.inherited_table().border_spacing_vertical)
    }

    pub(crate) fn flex_basis(self) -> &'a ComputedSize {
        let flex_basis = &self.alignment().flex_basis;
        if flex_basis.is_content {
            auto_computed_size()
        } else {
            &flex_basis.size
        }
    }
}
