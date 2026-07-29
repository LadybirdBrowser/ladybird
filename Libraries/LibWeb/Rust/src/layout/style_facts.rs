/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The full C++ StyleGroupIndex space; LayoutRustBridge.cpp static-asserts the
// count so the payload snapshot and the registered group indices line up.
pub const STYLE_GROUP_COUNT: usize = 23;

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
/// pass; anchor() inset values borrow theirs from the wrappers the node's
/// decode cache owns. Only values built by LayoutRustBridge carry a
/// bridge-retained handle (`calc_is_bridge_retained`), released through the
/// pass callback table.
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
#[repr(C)]
pub struct FfiStylePayloads {
    /// Borrowed pointers to every `ComputedValues` group payload. A
    /// `NodeWithStyle` keeps its immutable `ComputedValues` alive, and style
    /// replacement cannot run during the synchronous layout pass, so these
    /// pointers remain valid until the pass returns to C++.
    pub groups: [*const c_void; STYLE_GROUP_COUNT],
}

impl Default for FfiStylePayloads {
    fn default() -> Self {
        Self {
            groups: [std::ptr::null(); STYLE_GROUP_COUNT],
        }
    }
}

/// The registered group indices of the Rust-native style groups, resolved
/// once from the css module's lifecycle registration.
struct NativeGroupIndices {
    sizing: usize,
    surround: usize,
    alignment: usize,
    svg_reset: usize,
    inherited_box: usize,
    inherited_table: usize,
    box_values: usize,
    border_facts: usize,
    inherited_text_facts: usize,
    font_facts: usize,
}

static NATIVE_GROUP_INDICES: OnceLock<NativeGroupIndices> = OnceLock::new();

fn native_group_indices() -> &'static NativeGroupIndices {
    use crate::css::computed_values::{StyleGroupLifecycle, style_group_index_with_lifecycle};
    NATIVE_GROUP_INDICES.get_or_init(|| {
        assert_eq!(crate::css::computed_values::registered_style_group_count(), STYLE_GROUP_COUNT);
        NativeGroupIndices {
            sizing: style_group_index_with_lifecycle(StyleGroupLifecycle::Sizing),
            surround: style_group_index_with_lifecycle(StyleGroupLifecycle::Surround),
            alignment: style_group_index_with_lifecycle(StyleGroupLifecycle::Alignment),
            svg_reset: style_group_index_with_lifecycle(StyleGroupLifecycle::SVGReset),
            inherited_box: style_group_index_with_lifecycle(StyleGroupLifecycle::InheritedBox),
            inherited_table: style_group_index_with_lifecycle(StyleGroupLifecycle::InheritedTable),
            box_values: style_group_index_with_lifecycle(StyleGroupLifecycle::Box),
            border_facts: style_group_index_with_lifecycle(StyleGroupLifecycle::CppWithBorderFacts),
            inherited_text_facts: style_group_index_with_lifecycle(StyleGroupLifecycle::CppWithInheritedTextFacts),
            font_facts: style_group_index_with_lifecycle(StyleGroupLifecycle::CppWithFontFacts),
        }
    })
}

#[derive(Clone, Copy)]
struct StyleReader {
    payloads: FfiStylePayloads,
}

impl StyleReader {
    fn new(payloads: FfiStylePayloads) -> Self {
        Self { payloads }
    }

    #[inline]
    fn native_group<T>(&self, group_index: usize) -> &T {
        let payload = self.payloads.groups[group_index];
        debug_assert!(!payload.is_null());
        // SAFETY: The payload is the Rust-defined group struct itself; C++
        // derives its mirror from the cbindgen twin of the same type, and the
        // node's ComputedValues keep it alive for the synchronous pass.
        unsafe { &*payload.cast::<T>() }
    }

    #[inline]
    fn sizing(&self) -> &crate::layout::SizingValues {
        self.native_group(native_group_indices().sizing)
    }

    #[inline]
    fn surround(&self) -> &crate::layout::SurroundValues {
        self.native_group(native_group_indices().surround)
    }

    #[inline]
    fn alignment(&self) -> &crate::layout::AlignmentValues {
        self.native_group(native_group_indices().alignment)
    }

    #[inline]
    fn svg_reset(&self) -> &crate::layout::SVGResetValues {
        self.native_group(native_group_indices().svg_reset)
    }

    #[inline]
    fn inherited_box(&self) -> &crate::css::computed_values::InheritedBoxValues {
        self.native_group(native_group_indices().inherited_box)
    }

    #[inline]
    fn inherited_table(&self) -> &crate::css::computed_values::InheritedTableValues {
        self.native_group(native_group_indices().inherited_table)
    }

    #[inline]
    fn box_values(&self) -> &crate::layout::BoxValues {
        self.native_group(native_group_indices().box_values)
    }

    #[inline]
    fn border_facts(&self) -> &crate::layout::BorderLayoutFacts {
        self.native_group(native_group_indices().border_facts)
    }

    #[inline]
    fn inherited_text_facts(&self) -> &crate::layout::InheritedTextLayoutFacts {
        self.native_group(native_group_indices().inherited_text_facts)
    }

    #[inline]
    fn font_facts(&self) -> &crate::layout::FontLayoutFacts {
        self.native_group(native_group_indices().font_facts)
    }
}

const SIZE_FIELD_COUNT: usize = 26;

pub(crate) struct StyleDecodeCache {
    reader: StyleReader,
    sizes: [Cell<FfiSizeValue>; SIZE_FIELD_COUNT],
    size_presence: Cell<u32>,
    /// The in-crate-built calculated wrappers for anchor() insets, owned here
    /// so the cached size slots can borrow their calc pointers for the cache
    /// lifetime.
    anchor_inset_calculated_wrappers: RefCell<Vec<std::sync::Arc<crate::css::style_value::StyleValueData>>>,
    release_calc_handle: FfiReleaseCalcHandleCallback,
}

impl StyleDecodeCache {
    fn new(reader: StyleReader, release_calc_handle: FfiReleaseCalcHandleCallback) -> Self {
        Self {
            reader,
            sizes: std::array::from_fn(|_| Cell::new(FfiSizeValue::auto_value())),
            size_presence: Cell::new(0),
            anchor_inset_calculated_wrappers: RefCell::new(Vec::new()),
            release_calc_handle,
        }
    }

    #[inline]
    fn cached_size(&self, field: SizeField) -> Option<FfiSizeValue> {
        let index = field as usize;
        (self.size_presence.get() & (1 << index) != 0).then(|| self.sizes[index].get())
    }

    #[inline]
    fn cache_size(&self, field: SizeField, value: FfiSizeValue) -> FfiSizeValue {
        let index = field as usize;
        debug_assert_eq!(self.size_presence.get() & (1 << index), 0);
        self.sizes[index].set(value);
        self.size_presence.set(self.size_presence.get() | (1 << index));
        value
    }

    pub(crate) fn replace_size(&self, field: SizeField, value: FfiSizeValue) {
        let index = field as usize;
        let presence = self.size_presence.get();
        if presence & (1 << index) != 0 {
            self.sizes[index]
                .replace(value)
                .release_bridge_calc_handle(self.release_calc_handle);
        } else {
            self.sizes[index].set(value);
            self.size_presence.set(presence | (1 << index));
        }
    }
}

impl Drop for StyleDecodeCache {
    fn drop(&mut self) {
        let presence = self.size_presence.get();
        for (index, value) in self.sizes.iter().enumerate() {
            if presence & (1 << index) != 0 {
                value.get().release_bridge_calc_handle(self.release_calc_handle);
            }
        }
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

#[derive(Clone, Copy)]
pub(crate) struct DecodedStyleScalars {
    pub display: FfiDisplay,
    pub border_top_width: CssPixels,
    pub border_right_width: CssPixels,
    pub border_bottom_width: CssPixels,
    pub border_left_width: CssPixels,
    pub border_top_style: u8,
    pub border_right_style: u8,
    pub border_bottom_style: u8,
    pub border_left_style: u8,
    pub position: u8,
    pub float_: u8,
    pub clear: u8,
    pub writing_mode: u8,
    pub direction: u8,
    pub text_align: u8,
    pub text_justify: u8,
    pub white_space_collapse: u8,
    pub text_wrap_mode: u8,
    pub word_break: u8,
    pub font_variant_emoji: u8,
    pub line_height: CssPixels,
    pub font_size: CssPixels,
    pub box_sizing: u8,
    pub overflow_x: u8,
    pub overflow_y: u8,
    pub text_overflow: u8,
    pub flex_direction: u8,
    pub flex_wrap: u8,
    pub flex_grow: f64,
    pub flex_shrink: f64,
    pub order: i32,
    pub align_items: u8,
    pub align_self: u8,
    pub align_content: u8,
    pub justify_content: u8,
    pub justify_items: u8,
    pub justify_self: u8,
    pub border_collapse: u8,
    pub border_spacing_horizontal: CssPixels,
    pub border_spacing_vertical: CssPixels,
    pub caption_side: u8,
    pub table_layout: u8,
    pub visibility: u8,
    pub letter_spacing: CssPixels,
    pub word_spacing: CssPixels,
    pub unicode_bidi: u8,
    pub grid_auto_flow_row: bool,
    pub grid_auto_flow_dense: bool,
    pub css_preferred_aspect_ratio_numerator: CssPixels,
    pub css_preferred_aspect_ratio_denominator: CssPixels,
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

pub(crate) struct StyleDecodeValues {
    scalars: DecodedStyleScalars,
    cache: StyleDecodeCache,
}

impl StyleDecodeValues {
    pub(crate) fn new(payloads: FfiStylePayloads, release_calc_handle: FfiReleaseCalcHandleCallback) -> Self {
        let reader = StyleReader::new(payloads);
        Self {
            scalars: DecodedStyleScalars::decode(&reader),
            cache: StyleDecodeCache::new(reader, release_calc_handle),
        }
    }

    pub(crate) fn replace_size(&self, field: SizeField, value: FfiSizeValue) {
        self.cache.replace_size(field, value);
    }
}

/// A thin Rust-only view over a node's immutable computed-value group
/// payloads.
///
/// Plain fields are decoded once into `DecodedStyleScalars`; everything else
/// is interpreted in-crate from the typed group payloads.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    scalars: &'a DecodedStyleScalars,
    cache: &'a StyleDecodeCache,
    vertical_align_override: u16,
}

impl DecodedStyleScalars {
    fn decode(reader: &StyleReader) -> Self {
        let inherited_box = reader.inherited_box();
        let alignment = reader.alignment();
        let inherited_table = reader.inherited_table();
        let box_values = reader.box_values();
        let border = reader.border_facts();
        let inherited_text = reader.inherited_text_facts();
        let font = reader.font_facts();
        let (css_preferred_aspect_ratio_numerator, css_preferred_aspect_ratio_denominator) =
            decode_css_preferred_aspect_ratio(&box_values.aspect_ratio);
        Self {
            display: box_values.display,
            border_top_width: border.border_top.width,
            border_right_width: border.border_right.width,
            border_bottom_width: border.border_bottom.width,
            border_left_width: border.border_left.width,
            border_top_style: border.border_top.line_style,
            border_right_style: border.border_right.line_style,
            border_bottom_style: border.border_bottom.line_style,
            border_left_style: border.border_left.line_style,
            position: box_values.position,
            float_: box_values.float_,
            clear: box_values.clear,
            writing_mode: inherited_box.writing_mode,
            direction: inherited_box.direction,
            text_align: inherited_text.text_align,
            text_justify: inherited_text.text_justify,
            white_space_collapse: inherited_text.white_space_collapse,
            text_wrap_mode: inherited_text.text_wrap_mode,
            word_break: inherited_text.word_break,
            font_variant_emoji: font.font_variant_emoji,
            line_height: font.line_height_used,
            font_size: font.font_size,
            box_sizing: box_values.box_sizing,
            overflow_x: box_values.overflow_x,
            overflow_y: box_values.overflow_y,
            text_overflow: box_values.text_overflow,
            flex_direction: alignment.flex_direction,
            flex_wrap: alignment.flex_wrap,
            flex_grow: alignment.flex_grow,
            flex_shrink: alignment.flex_shrink,
            order: alignment.order,
            align_items: alignment.align_items,
            align_self: alignment.align_self,
            align_content: alignment.align_content,
            justify_content: alignment.justify_content,
            justify_items: alignment.justify_items,
            justify_self: alignment.justify_self,
            border_collapse: inherited_table.border_collapse,
            border_spacing_horizontal: CssPixels::from_raw(inherited_table.border_spacing_horizontal),
            border_spacing_vertical: CssPixels::from_raw(inherited_table.border_spacing_vertical),
            caption_side: inherited_table.caption_side,
            table_layout: box_values.table_layout,
            visibility: inherited_box.visibility,
            letter_spacing: inherited_text.letter_spacing,
            word_spacing: inherited_text.word_spacing,
            unicode_bidi: box_values.unicode_bidi,
            grid_auto_flow_row: box_values.grid_auto_flow_row,
            grid_auto_flow_dense: box_values.grid_auto_flow_dense,
            css_preferred_aspect_ratio_numerator,
            css_preferred_aspect_ratio_denominator,
        }
    }
}

impl<'a> StyleValues<'a> {
    #[inline]
    pub(crate) fn new(values: &'a StyleDecodeValues) -> Self {
        Self {
            scalars: &values.scalars,
            cache: &values.cache,
            vertical_align_override: u16::MAX,
        }
    }

    fn inset_has_anchor(self, field: SizeField) -> bool {
        let values = self.cache.reader.surround();
        let handle = match field {
            SizeField::InsetTop => &values.top_anchor_inset,
            SizeField::InsetRight => &values.right_anchor_inset,
            SizeField::InsetBottom => &values.bottom_anchor_inset,
            SizeField::InsetLeft => &values.left_anchor_inset,
            _ => unreachable!(),
        };
        !handle.pointer.is_null()
    }

    fn direct_size(self, field: SizeField) -> FfiSizeValue {
        match field {
            SizeField::Width
            | SizeField::Height
            | SizeField::MinWidth
            | SizeField::MinHeight
            | SizeField::MaxWidth
            | SizeField::MaxHeight => {
                let values = self.cache.reader.sizing();
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
                let values = self.cache.reader.surround();
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
                let values = self.cache.reader.alignment();
                if values.flex_basis.is_content {
                    FfiSizeValue::auto_value()
                } else {
                    decode_computed_size(&values.flex_basis.size)
                }
            }
            SizeField::RowGap | SizeField::ColumnGap => {
                let values = self.cache.reader.alignment();
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
                let values = self.cache.reader.svg_reset();
                decode_length_percentage(if field == SizeField::X { &values.x } else { &values.y })
            }
            SizeField::VerticalAlign => decode_length_percentage(&self.cache.reader.box_values().vertical_align.value),
            SizeField::TextIndent => {
                decode_length_percentage(&self.cache.reader.inherited_text_facts().text_indent.length_percentage)
            }
            SizeField::ColumnWidth => decode_computed_size(&self.cache.reader.box_values().column_width),
        }
    }

    fn anchor_inset_size_value(self, field: SizeField) -> FfiSizeValue {
        let values = self.cache.reader.surround();
        let handle = match field {
            SizeField::InsetTop => &values.top_anchor_inset,
            SizeField::InsetRight => &values.right_anchor_inset,
            SizeField::InsetBottom => &values.bottom_anchor_inset,
            SizeField::InsetLeft => &values.left_anchor_inset,
            _ => unreachable!(),
        };
        // SAFETY: The inset_has_anchor guard checked the handle is non-null,
        // and the node's style group payload keeps the anchor value alive for
        // the synchronous layout pass.
        let wrapper = unsafe { crate::css::calc::create_anchor_inset_calculated(handle.pointer.cast()) };
        let calc = std::sync::Arc::as_ptr(&wrapper).cast();
        self.cache.anchor_inset_calculated_wrappers.borrow_mut().push(wrapper);
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

    fn size_value(self, field: SizeField) -> FfiSizeValue {
        if let Some(value) = self.cache.cached_size(field) {
            return value;
        }
        match field {
            SizeField::InsetTop | SizeField::InsetRight | SizeField::InsetBottom | SizeField::InsetLeft
                if self.inset_has_anchor(field) =>
            {
                let value = self.anchor_inset_size_value(field);
                self.cache.cache_size(field, value)
            }
            _ => {
                let value = self.direct_size(field);
                self.cache.cache_size(field, value)
            }
        }
    }

    pub(crate) fn with_vertical_align_keyword(mut self, keyword: u8) -> Self {
        self.vertical_align_override = keyword as u16;
        self
    }

    pub(crate) fn vertical_align_is_keyword(self) -> bool {
        self.vertical_align_override != u16::MAX || self.cache.reader.box_values().vertical_align.is_keyword
    }

    pub(crate) fn vertical_align_keyword(self) -> u8 {
        if self.vertical_align_override != u16::MAX {
            self.vertical_align_override as u8
        } else {
            self.cache.reader.box_values().vertical_align.keyword
        }
    }

    pub(crate) fn vertical_align_value(self) -> FfiSizeValue {
        self.size_value(SizeField::VerticalAlign)
    }

    pub(crate) fn has_position_anchor(self) -> bool {
        self.cache.reader.surround().position_anchor_name.raw() != 0
    }

    /// The raw fly-string representation of the computed position-anchor
    /// name, borrowed from the surround payload for the duration of the pass.
    pub(crate) fn position_anchor_name(self) -> usize {
        self.cache.reader.surround().position_anchor_name.raw()
    }

    pub(crate) fn first_available_font(self) -> *const c_void {
        let font = self.cache.reader.font_facts().first_available_font;
        debug_assert!(!font.is_null(), "layout read a font group that never received a font list");
        font
    }

    pub(crate) fn font_cascade_list(self) -> *const c_void {
        let list = self.cache.reader.font_facts().font_cascade_list;
        debug_assert!(!list.is_null(), "layout read a font group that never received a font list");
        list
    }

    pub(crate) fn font_ascent(self) -> f32 {
        self.cache.reader.font_facts().font_ascent
    }

    pub(crate) fn font_descent(self) -> f32 {
        self.cache.reader.font_facts().font_descent
    }

    pub(crate) fn font_x_height(self) -> f32 {
        self.cache.reader.font_facts().font_x_height
    }

    pub(crate) fn box_sizing_for_aspect_ratio(self) -> u8 {
        let values = self.cache.reader.box_values();
        if values.aspect_ratio.use_natural_aspect_ratio_if_available {
            crate::css::css_enums::box_sizing::CONTENT_BOX
        } else {
            values.box_sizing
        }
    }

    pub(crate) fn css_preferred_aspect_ratio_numerator(self) -> CssPixels {
        self.scalars.css_preferred_aspect_ratio_numerator
    }

    pub(crate) fn css_preferred_aspect_ratio_denominator(self) -> CssPixels {
        self.scalars.css_preferred_aspect_ratio_denominator
    }

    pub(crate) fn aspect_ratio_uses_natural_when_available(self) -> bool {
        self.cache
            .reader
            .box_values()
            .aspect_ratio
            .use_natural_aspect_ratio_if_available
    }

    pub(crate) fn flex_basis_is_content(self) -> bool {
        self.cache.reader.alignment().flex_basis.is_content
    }

    pub(crate) fn flex_basis(self) -> FfiSizeValue {
        self.size_value(SizeField::FlexBasis)
    }

    pub(crate) fn has_column_count(self) -> bool {
        self.cache.reader.box_values().column_count_has_value
    }

    pub(crate) fn column_count(self) -> i32 {
        self.cache.reader.box_values().column_count
    }

    pub(crate) fn has_size_containment(self) -> bool {
        self.cache.reader.box_values().size_containment
    }

    pub(crate) fn is_size_container(self) -> bool {
        self.cache.reader.box_values().is_size_container
    }

    pub(crate) fn text_indent_each_line(self) -> bool {
        self.cache.reader.inherited_text_facts().text_indent.each_line
    }

    pub(crate) fn text_indent_hanging(self) -> bool {
        self.cache.reader.inherited_text_facts().text_indent.hanging
    }

    pub(crate) fn tab_size_is_number(self) -> bool {
        self.cache.reader.inherited_text_facts().tab_size_is_number
    }

    pub(crate) fn tab_size(self) -> CssPixels {
        self.cache.reader.inherited_text_facts().tab_size_length
    }

    pub(crate) fn tab_size_number(self) -> f64 {
        self.cache.reader.inherited_text_facts().tab_size_number
    }
}

impl Deref for StyleValues<'_> {
    type Target = DecodedStyleScalars;

    fn deref(&self) -> &Self::Target {
        self.scalars
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
