/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Registered indices of the style groups the layout engine reads, pinned to
// the C++ StyleGroupIndex enum by static_asserts in LayoutRustBridge.cpp.
pub const STYLE_GROUP_INDEX_INHERITED_TABLE: usize = 0;
pub const STYLE_GROUP_INDEX_INHERITED_TEXT: usize = 4;
pub const STYLE_GROUP_INDEX_INHERITED_BOX: usize = 5;
pub const STYLE_GROUP_INDEX_FONT: usize = 6;
pub const STYLE_GROUP_INDEX_SVG_RESET: usize = 8;
pub const STYLE_GROUP_INDEX_BORDER: usize = 17;
pub const STYLE_GROUP_INDEX_ALIGNMENT: usize = 18;
pub const STYLE_GROUP_INDEX_SIZING: usize = 20;
pub const STYLE_GROUP_INDEX_SURROUND: usize = 21;
pub const STYLE_GROUP_INDEX_BOX: usize = 22;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSizeKind {
    Auto,
    Px,
    Percentage,
    Calc,
    MinContent,
    MaxContent,
    FitContent,
    None_,
}

/// A computed CSS size value with no Rust-owned allocation.
///
/// `kind` is an `FfiSizeKind`. `fraction` is used for Percentage, `px` for Px,
/// and `calc` for Calc. FitContent uses the matching payload for its optional
/// inner length-percentage, and `fit_content_has_argument` distinguishes the
/// keyword-only form from an argument that resolves to zero.
///
/// Values decoded in Rust from the node's style group payloads borrow their
/// calc pointer from those payloads, which outlive the synchronous layout
/// pass; anchor() inset values borrow theirs from the wrappers the
/// LayoutState's anchor-inset store owns. Only values built by
/// LayoutRustBridge carry a bridge-retained handle
/// (`calc_is_bridge_retained`), released through the pass callback table.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiSizeValue {
    pub kind: u8,
    pub px: CssPixels,
    pub fraction: f64,
    pub calc: *const c_void,
    /// True when `calc` is a handle registered by LayoutRustBridge that must
    /// be released through the pass callbacks; false marks a borrowed pointer
    /// into the node's style payloads that must not be released.
    pub calc_is_bridge_retained: bool,
    pub contains_percentage: bool,
    pub contains_anchor_function: bool,
    pub fit_content_has_argument: bool,
}

pub type FfiReleaseCalcHandleCallback = unsafe extern "C" fn(*const c_void);
pub type FfiReleaseAnchorNameHandleCallback = unsafe extern "C" fn(usize);

impl FfiSizeValue {
    pub(crate) fn auto_value() -> Self {
        Self {
            kind: FfiSizeKind::Auto as u8,
            px: CssPixels::default(),
            fraction: 0.0,
            calc: std::ptr::null(),
            calc_is_bridge_retained: false,
            contains_percentage: false,
            contains_anchor_function: false,
            fit_content_has_argument: false,
        }
    }

    pub(crate) fn px_value(px: CssPixels) -> Self {
        Self {
            kind: FfiSizeKind::Px as u8,
            px,
            fraction: 0.0,
            calc: std::ptr::null(),
            calc_is_bridge_retained: false,
            contains_percentage: false,
            contains_anchor_function: false,
            fit_content_has_argument: false,
        }
    }

    #[cfg(test)]
    fn with_kind(kind: FfiSizeKind) -> Self {
        Self {
            kind: kind as u8,
            px: CssPixels::default(),
            fraction: 0.0,
            calc: std::ptr::null(),
            calc_is_bridge_retained: false,
            contains_percentage: false,
            contains_anchor_function: false,
            fit_content_has_argument: false,
        }
    }

    #[cfg(test)]
    pub(crate) fn px(px: CssPixels) -> Self {
        Self::px_value(px)
    }

    #[cfg(test)]
    pub(crate) fn percentage(fraction: f64) -> Self {
        Self {
            fraction,
            ..Self::with_kind(FfiSizeKind::Percentage)
        }
    }

    pub(crate) fn release_bridge_calc_handle(self, release: FfiReleaseCalcHandleCallback) {
        if self.calc_is_bridge_retained && !self.calc.is_null() {
            // SAFETY: Every bridge-retained handle is registered by
            // LayoutRustBridge and released exactly once when the per-pass
            // Rust layout state is dropped.
            unsafe {
                release(self.calc);
            }
        }
    }

    pub(crate) fn kind(self) -> FfiSizeKind {
        assert!(self.kind <= FfiSizeKind::None_ as u8);
        // SAFETY: The range check above covers every repr(u8) variant.
        unsafe { std::mem::transmute(self.kind) }
    }

    pub(crate) fn is_auto(self) -> bool {
        self.kind() == FfiSizeKind::Auto
    }

    pub(crate) fn is_length(self) -> bool {
        self.kind() == FfiSizeKind::Px
    }

    pub(crate) fn is_percentage(self) -> bool {
        self.kind() == FfiSizeKind::Percentage
    }

    pub(crate) fn is_length_percentage(self) -> bool {
        matches!(
            self.kind(),
            FfiSizeKind::Px | FfiSizeKind::Percentage | FfiSizeKind::Calc
        )
    }

    pub(crate) fn is_min_content(self) -> bool {
        self.kind() == FfiSizeKind::MinContent
    }

    pub(crate) fn is_max_content(self) -> bool {
        self.kind() == FfiSizeKind::MaxContent
    }

    pub(crate) fn is_fit_content(self) -> bool {
        self.kind() == FfiSizeKind::FitContent
    }

    pub(crate) fn is_none(self) -> bool {
        self.kind() == FfiSizeKind::None_
    }

    pub(crate) fn is_intrinsic_sizing_constraint(self) -> bool {
        matches!(
            self.kind(),
            FfiSizeKind::MinContent | FfiSizeKind::MaxContent | FfiSizeKind::FitContent
        )
    }

    pub(crate) fn to_px(self, reference: CssPixels) -> CssPixels {
        match self.kind() {
            FfiSizeKind::Px => self.px,
            FfiSizeKind::Percentage => truncated_css_pixels(reference.to_double() * self.fraction),
            FfiSizeKind::Calc => resolve_calc(self.calc, reference),
            FfiSizeKind::FitContent if !self.calc.is_null() => resolve_calc(self.calc, reference),
            FfiSizeKind::FitContent if self.contains_percentage => {
                truncated_css_pixels(reference.to_double() * self.fraction)
            }
            FfiSizeKind::FitContent => self.px,
            FfiSizeKind::Auto | FfiSizeKind::MinContent | FfiSizeKind::MaxContent | FfiSizeKind::None_ => {
                CssPixels::default()
            }
        }
    }
}

fn truncated_css_pixels(value: f64) -> CssPixels {
    if value.is_nan() {
        return CssPixels::default();
    }
    let raw = (value * 64.0).trunc();
    CssPixels::from_raw(raw.clamp(i32::MIN as f64, i32::MAX as f64) as i32)
}

fn resolve_calc(calc: *const c_void, percentage_basis: CssPixels) -> CssPixels {
    assert!(!calc.is_null());
    let context = px_calc_resolution_context(percentage_basis);
    // SAFETY: The style value stays alive for the pass and the context
    // carries no host callbacks.
    let result = unsafe { crate::css::calc::rust_calc_resolve(calc, &raw const context, true) };
    assert!(result.resolved);
    CssPixels::nearest_value_for(result.value)
}

/// Every sizing-shaped value the layout engine reads, each decoding straight
/// from the node's typed group payloads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum SizeField {
    Width,
    Height,
    MinWidth,
    MinHeight,
    MaxWidth,
    MaxHeight,
    MarginTop,
    MarginRight,
    MarginBottom,
    MarginLeft,
    PaddingTop,
    PaddingRight,
    PaddingBottom,
    PaddingLeft,
    InsetTop,
    InsetRight,
    InsetBottom,
    InsetLeft,
    FlexBasis,
    RowGap,
    ColumnGap,
    ColumnWidth,
    TextIndent,
    X,
    Y,
    VerticalAlign,
}

#[derive(Clone, Copy)]
pub(crate) struct StyleReader<'a> {
    payloads: &'a FfiStylePayloads,
}

impl<'a> StyleReader<'a> {
    fn new(payloads: &'a FfiStylePayloads) -> Self {
        Self { payloads }
    }

    #[inline]
    fn native_group<T>(&self, group_index: usize) -> &'a T {
        let payload = self.payloads.groups[group_index];
        debug_assert!(!payload.is_null());
        // SAFETY: The payload is the Rust-defined group struct itself; C++
        // derives its mirror from the cbindgen twin of the same type, and the
        // node's ComputedValues keep it alive for the synchronous pass.
        unsafe { &*payload.cast::<T>() }
    }

    #[inline]
    fn sizing(&self) -> &'a crate::layout::SizingValues {
        self.native_group(STYLE_GROUP_INDEX_SIZING)
    }

    #[inline]
    fn surround(&self) -> &'a crate::layout::SurroundValues {
        self.native_group(STYLE_GROUP_INDEX_SURROUND)
    }

    #[inline]
    fn alignment(&self) -> &'a crate::layout::AlignmentValues {
        self.native_group(STYLE_GROUP_INDEX_ALIGNMENT)
    }

    #[inline]
    fn svg_reset(&self) -> &'a crate::layout::SVGResetValues {
        self.native_group(STYLE_GROUP_INDEX_SVG_RESET)
    }

    #[inline]
    fn inherited_box(&self) -> &'a crate::css::computed_values::InheritedBoxValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_BOX)
    }

    #[inline]
    fn inherited_table(&self) -> &'a crate::css::computed_values::InheritedTableValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TABLE)
    }

    #[inline]
    fn box_values(&self) -> &'a crate::layout::BoxValues {
        self.native_group(STYLE_GROUP_INDEX_BOX)
    }

    #[inline]
    fn border_facts(&self) -> &'a crate::layout::BorderLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_BORDER)
    }

    #[inline]
    fn inherited_text_facts(&self) -> &'a crate::layout::InheritedTextLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TEXT)
    }

    #[inline]
    fn font_facts(&self) -> &'a crate::layout::FontLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_FONT)
    }
}

fn anchor_inset_field_index(field: SizeField) -> usize {
    match field {
        SizeField::InsetTop => 0,
        SizeField::InsetRight => 1,
        SizeField::InsetBottom => 2,
        SizeField::InsetLeft => 3,
        _ => unreachable!(),
    }
}

#[derive(Default)]
struct AnchorInsetField {
    /// Resolved px/auto value written by replace_resolved_anchor_insets. It
    /// takes precedence over every style decode and reports
    /// contains_anchor_function == false; the abspos engine's early-out on
    /// re-entry depends on both properties. Never carries a calc pointer.
    resolved_override: Cell<Option<FfiSizeValue>>,
    /// Memoized in-crate calculated wrapper for a bare anchor() inset: the
    /// single owner of the Arc whose pointer the returned size values borrow.
    /// Written at most once and never replaced or dropped before the owning
    /// LayoutState drops.
    wrapper: std::cell::OnceCell<std::sync::Arc<crate::css::style_value::StyleValueData>>,
}

#[derive(Default)]
struct AnchorInsetSlot {
    fields: [AnchorInsetField; 4],
}

/// Per-LayoutState side store for the four inset fields of anchor-positioned
/// nodes, the only style reads whose results are not a pure function of the
/// immutable group payloads.
#[derive(Default)]
pub(crate) struct AnchorInsetStore {
    slots: PagedStore<AnchorInsetSlot>,
    /// False until the first override write, so inset reads on anchor-free
    /// pages never touch the slots store.
    any_overrides: Cell<bool>,
}

impl AnchorInsetStore {
    fn slot(&self, slot_index: u32) -> &AnchorInsetSlot {
        self.slots
            .get(slot_index)
            .unwrap_or_else(|| self.slots.allocate(slot_index, AnchorInsetSlot::default()))
    }

    fn override_for(&self, slot_index: u32, field: SizeField) -> Option<FfiSizeValue> {
        if !self.any_overrides.get() {
            return None;
        }
        self.slots.get(slot_index)?.fields[anchor_inset_field_index(field)]
            .resolved_override
            .get()
    }

    fn memoized_anchor_inset_value(
        &self,
        slot_index: u32,
        field: SizeField,
        build_wrapper: impl FnOnce() -> std::sync::Arc<crate::css::style_value::StyleValueData>,
    ) -> FfiSizeValue {
        let field = &self.slot(slot_index).fields[anchor_inset_field_index(field)];
        let calc = std::sync::Arc::as_ptr(field.wrapper.get_or_init(build_wrapper)).cast();
        FfiSizeValue {
            kind: FfiSizeKind::Calc as u8,
            px: CssPixels::default(),
            fraction: 0.0,
            calc,
            calc_is_bridge_retained: false,
            contains_percentage: false,
            contains_anchor_function: true,
            fit_content_has_argument: false,
        }
    }

    pub(crate) fn set_override(&self, slot_index: u32, field: SizeField, value: FfiSizeValue) {
        debug_assert!(value.calc.is_null());
        self.slot(slot_index).fields[anchor_inset_field_index(field)]
            .resolved_override
            .set(Some(value));
        self.any_overrides.set(true);
    }
}

fn decode_length_percentage(handle: &crate::layout::ComputedStyleValueHandle) -> FfiSizeValue {
    use crate::css::style_value::StyleValueData;

    let pointer = handle.pointer;
    assert!(!pointer.is_null());
    // SAFETY: The handle points at the retained style-value data held by the
    // node's style group payload, which outlives the synchronous layout pass.
    let data = unsafe { &*pointer.cast::<StyleValueData>() };
    match data {
        StyleValueData::Length { value, unit } => {
            let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
            assert!(ratio.is_finite(), "computed length is not absolute");
            FfiSizeValue::px_value(CssPixels::nearest_value_for(value * ratio))
        }
        StyleValueData::Percentage { value } => FfiSizeValue {
            kind: FfiSizeKind::Percentage as u8,
            px: CssPixels::default(),
            // Match Percentage::as_fraction(); the multiplication order is
            // observable for some f64 inputs.
            fraction: value * 0.01,
            calc: std::ptr::null(),
            calc_is_bridge_retained: false,
            contains_percentage: true,
            contains_anchor_function: false,
            fit_content_has_argument: false,
        },
        StyleValueData::Calculated { .. } => {
            // SAFETY: The style value outlives the pass; the calc pointer
            // below borrows it rather than retaining a second reference.
            let root = unsafe { crate::css::calc::rust_calc_root_from_calculated(pointer) };
            assert!(!root.is_null());
            let contains_percentage = unsafe { crate::css::calc::rust_calc_node_contains_percentage(root) };
            let contains_anchor_function = unsafe { crate::css::calc::rust_calc_contains_anchor(pointer) };
            FfiSizeValue {
                kind: FfiSizeKind::Calc as u8,
                px: CssPixels::default(),
                fraction: 0.0,
                calc: pointer,
                calc_is_bridge_retained: false,
                contains_percentage,
                contains_anchor_function,
                fit_content_has_argument: false,
            }
        }
        _ => unreachable!("computed length-percentage holds a non-length-percentage style value"),
    }
}

fn decode_computed_size(value: &crate::layout::ComputedSize) -> FfiSizeValue {
    use crate::layout::ComputedSizeKind;

    match value.kind {
        ComputedSizeKind::Auto => FfiSizeValue::auto_value(),
        ComputedSizeKind::Calculated => decode_length_percentage(&value.value),
        ComputedSizeKind::Length => {
            let result = decode_length_percentage(&value.value);
            assert_eq!(result.kind(), FfiSizeKind::Px);
            result
        }
        ComputedSizeKind::Percentage => {
            let result = decode_length_percentage(&value.value);
            assert_eq!(result.kind(), FfiSizeKind::Percentage);
            result
        }
        ComputedSizeKind::MinContent => FfiSizeValue {
            kind: FfiSizeKind::MinContent as u8,
            ..FfiSizeValue::auto_value()
        },
        ComputedSizeKind::MaxContent => FfiSizeValue {
            kind: FfiSizeKind::MaxContent as u8,
            ..FfiSizeValue::auto_value()
        },
        ComputedSizeKind::FitContent => {
            if value.value.pointer.is_null() {
                FfiSizeValue {
                    kind: FfiSizeKind::FitContent as u8,
                    ..FfiSizeValue::auto_value()
                }
            } else {
                let mut result = decode_length_percentage(&value.value);
                result.kind = FfiSizeKind::FitContent as u8;
                result.fit_content_has_argument = true;
                result
            }
        }
        ComputedSizeKind::None => FfiSizeValue {
            kind: FfiSizeKind::None_ as u8,
            ..FfiSizeValue::auto_value()
        },
    }
}

fn decode_length_percentage_or_auto(value: &crate::layout::ComputedLengthPercentageOrAuto) -> FfiSizeValue {
    if value.is_auto {
        FfiSizeValue::auto_value()
    } else {
        decode_length_percentage(&value.value)
    }
}

/// The computed `aspect-ratio` <ratio> term as a CSSPixels fraction. A zero
/// denominator means no usable ratio (none specified, degenerate, or collapsed
/// to zero by the fixed-point conversion).
fn decode_css_preferred_aspect_ratio(
    ratio: &crate::css::computed_value_types::ComputedAspectRatio,
) -> (CssPixels, CssPixels) {
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

/// A thin Rust-only view over a node's immutable computed-value group
/// payloads; every read decodes on demand from the typed group payloads. The
/// four inset fields additionally consult the per-LayoutState anchor-inset
/// store, the only style state a pass can change.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    reader: StyleReader<'a>,
    anchor_insets: &'a AnchorInsetStore,
    slot_index: u32,
    vertical_align_override: u16,
}

macro_rules! scalar_accessors {
    ($($group:ident: { $($name:ident: $ty:ty => $($field:ident).+,)+ })+) => {
        impl StyleValues<'_> {
            $($(
                #[inline]
                pub(crate) fn $name(self) -> $ty {
                    self.reader.$group().$($field).+
                }
            )+)+
        }
    };
}

scalar_accessors! {
    box_values: {
        display: FfiDisplay => display,
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
    }
    font_facts: {
        font_variant_emoji: u8 => font_variant_emoji,
        line_height: CssPixels => line_height_used,
        font_size: CssPixels => font_size,
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
    }
}

impl<'a> StyleValues<'a> {
    #[inline]
    pub(crate) fn new(reader: StyleReader<'a>, anchor_insets: &'a AnchorInsetStore, slot_index: u32) -> Self {
        Self {
            reader,
            anchor_insets,
            slot_index,
            vertical_align_override: u16::MAX,
        }
    }

    fn anchor_inset_handle(self, field: SizeField) -> Option<&'a crate::layout::ComputedStyleValueHandle> {
        let values = self.reader.surround();
        let handle = match field {
            SizeField::InsetTop => &values.top_anchor_inset,
            SizeField::InsetRight => &values.right_anchor_inset,
            SizeField::InsetBottom => &values.bottom_anchor_inset,
            SizeField::InsetLeft => &values.left_anchor_inset,
            _ => unreachable!(),
        };
        (!handle.pointer.is_null()).then_some(handle)
    }

    fn direct_size(self, field: SizeField) -> FfiSizeValue {
        match field {
            SizeField::Width
            | SizeField::Height
            | SizeField::MinWidth
            | SizeField::MinHeight
            | SizeField::MaxWidth
            | SizeField::MaxHeight => {
                let values = self.reader.sizing();
                decode_computed_size(match field {
                    SizeField::Width => &values.width,
                    SizeField::Height => &values.height,
                    SizeField::MinWidth => &values.min_width,
                    SizeField::MinHeight => &values.min_height,
                    SizeField::MaxWidth => &values.max_width,
                    SizeField::MaxHeight => &values.max_height,
                    _ => unreachable!(),
                })
            }
            SizeField::MarginTop
            | SizeField::MarginRight
            | SizeField::MarginBottom
            | SizeField::MarginLeft
            | SizeField::PaddingTop
            | SizeField::PaddingRight
            | SizeField::PaddingBottom
            | SizeField::PaddingLeft
            | SizeField::InsetTop
            | SizeField::InsetRight
            | SizeField::InsetBottom
            | SizeField::InsetLeft => {
                let values = self.reader.surround();
                decode_length_percentage_or_auto(match field {
                    SizeField::MarginTop => &values.margin.top,
                    SizeField::MarginRight => &values.margin.right,
                    SizeField::MarginBottom => &values.margin.bottom,
                    SizeField::MarginLeft => &values.margin.left,
                    SizeField::PaddingTop => &values.padding.top,
                    SizeField::PaddingRight => &values.padding.right,
                    SizeField::PaddingBottom => &values.padding.bottom,
                    SizeField::PaddingLeft => &values.padding.left,
                    SizeField::InsetTop => &values.inset.top,
                    SizeField::InsetRight => &values.inset.right,
                    SizeField::InsetBottom => &values.inset.bottom,
                    SizeField::InsetLeft => &values.inset.left,
                    _ => unreachable!(),
                })
            }
            SizeField::FlexBasis => {
                let values = self.reader.alignment();
                if values.flex_basis.is_content {
                    FfiSizeValue::auto_value()
                } else {
                    decode_computed_size(&values.flex_basis.size)
                }
            }
            SizeField::RowGap | SizeField::ColumnGap => {
                let values = self.reader.alignment();
                let gap = if field == SizeField::RowGap {
                    &values.row_gap
                } else {
                    &values.column_gap
                };
                if gap.is_normal {
                    FfiSizeValue::auto_value()
                } else {
                    decode_length_percentage(&gap.value)
                }
            }
            SizeField::X | SizeField::Y => {
                let values = self.reader.svg_reset();
                decode_length_percentage(if field == SizeField::X { &values.x } else { &values.y })
            }
            SizeField::VerticalAlign => decode_length_percentage(&self.reader.box_values().vertical_align.value),
            SizeField::TextIndent => {
                decode_length_percentage(&self.reader.inherited_text_facts().text_indent.length_percentage)
            }
            SizeField::ColumnWidth => decode_computed_size(&self.reader.box_values().column_width),
        }
    }

    fn size_value(self, field: SizeField) -> FfiSizeValue {
        if matches!(
            field,
            SizeField::InsetTop | SizeField::InsetRight | SizeField::InsetBottom | SizeField::InsetLeft
        ) {
            // The resolved override must mask BOTH anchor representations: a
            // bare anchor() inset carries the surround anchor handle below,
            // while a calc() containing anchor() has a null handle and
            // decodes through direct_size with contains_anchor_function set.
            if let Some(value) = self.anchor_insets.override_for(self.slot_index, field) {
                return value;
            }
            if let Some(handle) = self.anchor_inset_handle(field) {
                return self
                    .anchor_insets
                    .memoized_anchor_inset_value(self.slot_index, field, || {
                        // SAFETY: The handle is non-null, and the node's style
                        // group payload keeps the anchor value alive for the
                        // synchronous layout pass.
                        unsafe { crate::css::calc::create_anchor_inset_calculated(handle.pointer.cast()) }
                    });
            }
        }
        self.direct_size(field)
    }

    pub(crate) fn with_vertical_align_keyword(mut self, keyword: u8) -> Self {
        self.vertical_align_override = keyword as u16;
        self
    }

    pub(crate) fn vertical_align_is_keyword(self) -> bool {
        self.vertical_align_override != u16::MAX || self.reader.box_values().vertical_align.is_keyword
    }

    pub(crate) fn vertical_align_keyword(self) -> u8 {
        if self.vertical_align_override != u16::MAX {
            self.vertical_align_override as u8
        } else {
            self.reader.box_values().vertical_align.keyword
        }
    }

    pub(crate) fn vertical_align_value(self) -> FfiSizeValue {
        self.size_value(SizeField::VerticalAlign)
    }

    pub(crate) fn has_position_anchor(self) -> bool {
        self.reader.surround().position_anchor_name.raw() != 0
    }

    /// The raw fly-string representation of the computed position-anchor
    /// name, borrowed from the surround payload for the duration of the pass.
    pub(crate) fn position_anchor_name(self) -> usize {
        self.reader.surround().position_anchor_name.raw()
    }

    pub(crate) fn first_available_font(self) -> *const c_void {
        let font = self.reader.font_facts().first_available_font;
        debug_assert!(!font.is_null(), "layout read a font group that never received a font list");
        font
    }

    pub(crate) fn font_cascade_list(self) -> *const c_void {
        let list = self.reader.font_facts().font_cascade_list;
        debug_assert!(!list.is_null(), "layout read a font group that never received a font list");
        list
    }

    pub(crate) fn font_ascent(self) -> f32 {
        self.reader.font_facts().font_ascent
    }

    pub(crate) fn font_descent(self) -> f32 {
        self.reader.font_facts().font_descent
    }

    pub(crate) fn font_x_height(self) -> f32 {
        self.reader.font_facts().font_x_height
    }

    pub(crate) fn box_sizing_for_aspect_ratio(self) -> u8 {
        let values = self.reader.box_values();
        if values.aspect_ratio.use_natural_aspect_ratio_if_available {
            crate::css::css_enums::box_sizing::CONTENT_BOX
        } else {
            values.box_sizing
        }
    }

    pub(crate) fn css_preferred_aspect_ratio(self) -> (CssPixels, CssPixels) {
        decode_css_preferred_aspect_ratio(&self.reader.box_values().aspect_ratio)
    }

    pub(crate) fn border_spacing_horizontal(self) -> CssPixels {
        CssPixels::from_raw(self.reader.inherited_table().border_spacing_horizontal)
    }

    pub(crate) fn border_spacing_vertical(self) -> CssPixels {
        CssPixels::from_raw(self.reader.inherited_table().border_spacing_vertical)
    }

    pub(crate) fn aspect_ratio_uses_natural_when_available(self) -> bool {
        self.reader.box_values().aspect_ratio.use_natural_aspect_ratio_if_available
    }

    pub(crate) fn flex_basis_is_content(self) -> bool {
        self.reader.alignment().flex_basis.is_content
    }

    pub(crate) fn flex_basis(self) -> FfiSizeValue {
        self.size_value(SizeField::FlexBasis)
    }

    pub(crate) fn has_column_count(self) -> bool {
        self.reader.box_values().column_count_has_value
    }

    pub(crate) fn column_count(self) -> i32 {
        self.reader.box_values().column_count
    }

    pub(crate) fn has_size_containment(self) -> bool {
        self.reader.box_values().size_containment
    }

    pub(crate) fn is_size_container(self) -> bool {
        self.reader.box_values().is_size_container
    }

    pub(crate) fn text_indent_each_line(self) -> bool {
        self.reader.inherited_text_facts().text_indent.each_line
    }

    pub(crate) fn text_indent_hanging(self) -> bool {
        self.reader.inherited_text_facts().text_indent.hanging
    }

    pub(crate) fn tab_size_is_number(self) -> bool {
        self.reader.inherited_text_facts().tab_size_is_number
    }

    pub(crate) fn tab_size(self) -> CssPixels {
        self.reader.inherited_text_facts().tab_size_length
    }

    pub(crate) fn tab_size_number(self) -> f64 {
        self.reader.inherited_text_facts().tab_size_number
    }
}

macro_rules! size_accessors {
    ($($name:ident => $field:ident,)+) => {
        impl StyleValues<'_> {
            $(pub(crate) fn $name(self) -> FfiSizeValue {
                self.size_value(SizeField::$field)
            })+
        }
    };
}

size_accessors! {
    width => Width,
    height => Height,
    min_width => MinWidth,
    min_height => MinHeight,
    max_width => MaxWidth,
    max_height => MaxHeight,
    margin_top => MarginTop,
    margin_right => MarginRight,
    margin_bottom => MarginBottom,
    margin_left => MarginLeft,
    padding_top => PaddingTop,
    padding_right => PaddingRight,
    padding_bottom => PaddingBottom,
    padding_left => PaddingLeft,
    inset_top => InsetTop,
    inset_right => InsetRight,
    inset_bottom => InsetBottom,
    inset_left => InsetLeft,
    row_gap => RowGap,
    column_gap => ColumnGap,
    column_width => ColumnWidth,
    text_indent => TextIndent,
    x => X,
    y => Y,
}

pub(crate) fn px_calc_resolution_context(percentage_basis: CssPixels) -> crate::css::calc::FfiCalcResolutionContext {
    crate::css::calc::FfiCalcResolutionContext {
        basis_kind: 3,
        basis_value: percentage_basis.to_double(),
        basis_unit: crate::css::style_compute::px_length_unit(),
        length_resolution_context: std::ptr::null(),
        external_resolutions: std::ptr::null(),
        external_resolution_count: 0,
    }
}

pub(crate) unsafe fn resolve_calc_with_external_resolutions(
    calculated: *const c_void,
    percentage_basis: CssPixels,
    callback_context: *mut c_void,
    resolve_non_math_function: Option<unsafe extern "C" fn(*mut c_void, *const c_void) -> *const c_void>,
) -> crate::css::calc::FfiResolvedCalc {
    use crate::css::calc::{
        FfiCalcExternalResolutionKind, rust_calc_external_resolutions, rust_calc_external_resolutions_release,
        rust_calc_resolve, rust_calc_root_from_calculated,
    };

    let mut context = px_calc_resolution_context(percentage_basis);
    let root = unsafe { rust_calc_root_from_calculated(calculated) };
    let external =
        unsafe { rust_calc_external_resolutions(root, context.basis_kind, context.basis_value, context.basis_unit) };
    context.external_resolutions = external.resolutions;
    context.external_resolution_count = external.resolution_count;
    if let Some(resolve_non_math_function) = resolve_non_math_function
        && external.resolution_count > 0
    {
        for resolution in unsafe { std::slice::from_raw_parts_mut(external.resolutions, external.resolution_count) } {
            if resolution.kind == FfiCalcExternalResolutionKind::NonMathFunction {
                resolution.resolved_node =
                    unsafe { resolve_non_math_function(callback_context, resolution.source) }.cast();
            }
        }
    }
    let result = unsafe { rust_calc_resolve(calculated, &raw const context, true) };
    unsafe {
        rust_calc_external_resolutions_release(external.storage);
    }
    result
}
