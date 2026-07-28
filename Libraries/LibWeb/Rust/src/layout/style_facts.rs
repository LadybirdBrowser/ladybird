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
/// inner length-percentage; an all-zero payload is its keyword/zero form.
///
/// Values decoded in Rust from the node's style group payloads borrow their
/// calc pointer from those payloads, which outlive the synchronous layout
/// pass. Only values built by LayoutRustBridge carry a bridge-retained handle
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

/// Every scalar computed-value field that still lives in a hand-written C++
/// style group and therefore crosses as a C++-registered byte offset. Fields
/// in Rust-native groups are read as typed payloads and never appear here.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiStyleField {
    BorderTopWidth,
    BorderRightWidth,
    BorderBottomWidth,
    BorderLeftWidth,
    BorderTopStyle,
    BorderRightStyle,
    BorderBottomStyle,
    BorderLeftStyle,
    Position,
    Float,
    Clear,
    TextAlign,
    TextJustify,
    WhiteSpaceCollapse,
    TextWrapMode,
    LineHeight,
    FontSize,
    BoxSizing,
    OverflowX,
    OverflowY,
    TextOverflow,
    TableLayout,
    LetterSpacing,
    WordSpacing,
    UnicodeBidi,
    GridAutoFlowRow,
    GridAutoFlowDense,
    Count,
}

/// Every sizing-shaped value the layout engine reads. The Rust-native entries
/// decode straight from the node's typed group payloads; ColumnWidth and
/// TextIndent come from the C++ residual batch.
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
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleFieldEncoding {
    U8,
    Bool,
    I32,
    F64,
    CssPixels,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleFieldSchema {
    pub field: FfiStyleField,
    pub group_index: u8,
    pub offset: u32,
    pub group_size: u32,
    pub encoding: FfiStyleFieldEncoding,
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

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleSnapshot {
    pub payloads: FfiStylePayloads,
    pub display: FfiDisplay,
}

impl Default for FfiStyleSnapshot {
    fn default() -> Self {
        Self {
            payloads: FfiStylePayloads::default(),
            display: FfiDisplay::block(),
        }
    }
}

impl Default for FfiStylePayloads {
    fn default() -> Self {
        Self {
            groups: [std::ptr::null(); STYLE_GROUP_COUNT],
        }
    }
}

pub(crate) const STYLE_FIELD_COUNT: usize = FfiStyleField::Count as usize;

struct StyleSchema([FfiStyleFieldSchema; STYLE_FIELD_COUNT]);

// SAFETY: Schema entries contain only immutable integers and enum values.
unsafe impl Send for StyleSchema {}
unsafe impl Sync for StyleSchema {}

static STYLE_SCHEMA: OnceLock<StyleSchema> = OnceLock::new();

/// The registered group indices of the Rust-native style groups, resolved
/// once from the css module's lifecycle registration.
struct NativeGroupIndices {
    sizing: usize,
    surround: usize,
    alignment: usize,
    svg_reset: usize,
    inherited_box: usize,
    inherited_table: usize,
}

static NATIVE_GROUP_INDICES: OnceLock<NativeGroupIndices> = OnceLock::new();

fn native_group_indices() -> &'static NativeGroupIndices {
    use crate::css::computed_values::{StyleGroupLifecycle, style_group_index_with_lifecycle};
    NATIVE_GROUP_INDICES.get_or_init(|| NativeGroupIndices {
        sizing: style_group_index_with_lifecycle(StyleGroupLifecycle::Sizing),
        surround: style_group_index_with_lifecycle(StyleGroupLifecycle::Surround),
        alignment: style_group_index_with_lifecycle(StyleGroupLifecycle::Alignment),
        svg_reset: style_group_index_with_lifecycle(StyleGroupLifecycle::SVGReset),
        inherited_box: style_group_index_with_lifecycle(StyleGroupLifecycle::InheritedBox),
        inherited_table: style_group_index_with_lifecycle(StyleGroupLifecycle::InheritedTable),
    })
}

/// # Safety
///
/// `entries` must point to `count` valid schema entries for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_register_style_schema(entries: *const FfiStyleFieldSchema, count: usize) {
    abort_on_panic(|| {
        let entries = unsafe { std::slice::from_raw_parts(entries, count) };
        assert_eq!(entries.len(), STYLE_FIELD_COUNT);
        assert_eq!(crate::css::computed_values::registered_style_group_count(), STYLE_GROUP_COUNT);
        let mut schema = [entries[0]; STYLE_FIELD_COUNT];
        let mut present = [false; STYLE_FIELD_COUNT];
        for entry in entries {
            let index = entry.field as usize;
            assert!(index < STYLE_FIELD_COUNT);
            assert!((entry.group_index as usize) < STYLE_GROUP_COUNT);
            assert!(!present[index], "duplicate style schema field");
            let width = match entry.encoding {
                FfiStyleFieldEncoding::U8 | FfiStyleFieldEncoding::Bool => 1,
                FfiStyleFieldEncoding::I32 | FfiStyleFieldEncoding::CssPixels => 4,
                FfiStyleFieldEncoding::F64 => 8,
            };
            assert!(entry.offset as usize + width <= entry.group_size as usize);
            schema[index] = *entry;
            present[index] = true;
        }
        assert!(present.iter().all(|present| *present));
        assert!(
            STYLE_SCHEMA.set(StyleSchema(schema)).is_ok(),
            "style schema registered twice"
        );
    });
}

fn style_schema() -> &'static StyleSchema {
    STYLE_SCHEMA.get().expect("style schema used before registration")
}

/// Every field whose immutable payload still requires C++ interpretation.
/// One callback populates the whole value on the first residual read for a
/// node. Anchor inset entries are populated only when the corresponding
/// Rust-visible anchor handle is non-null.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiResidualStyleValues {
    pub anchor_inset_top: FfiSizeValue,
    pub anchor_inset_right: FfiSizeValue,
    pub anchor_inset_bottom: FfiSizeValue,
    pub anchor_inset_left: FfiSizeValue,
    pub position_anchor_has_value: bool,
    /// A transferred Utf16FlyString reference, when non-zero.
    pub position_anchor_retained_name: usize,
    pub vertical_align_is_keyword: bool,
    pub vertical_align_keyword: u8,
    pub vertical_align_value: FfiSizeValue,
    pub first_available_font: *const c_void,
    pub font_ascent: f32,
    pub font_descent: f32,
    pub font_x_height: f32,
    pub box_sizing_for_aspect_ratio: u8,
    /// The computed `aspect-ratio` <ratio> term. A zero denominator means no
    /// usable ratio (either none was specified or the value was degenerate).
    pub css_preferred_aspect_ratio_numerator: CssPixels,
    pub css_preferred_aspect_ratio_denominator: CssPixels,
    pub aspect_ratio_uses_natural_when_available: bool,
    pub column_width: FfiSizeValue,
    pub column_count_has_value: bool,
    pub column_count: i32,
    pub containment_bits: u8,
    pub text_indent: FfiSizeValue,
    pub text_indent_each_line: bool,
    pub text_indent_hanging: bool,
    pub tab_size_is_number: bool,
    pub tab_size: CssPixels,
    pub tab_size_number: f64,
}

impl Default for FfiResidualStyleValues {
    fn default() -> Self {
        Self {
            anchor_inset_top: FfiSizeValue::auto_value(),
            anchor_inset_right: FfiSizeValue::auto_value(),
            anchor_inset_bottom: FfiSizeValue::auto_value(),
            anchor_inset_left: FfiSizeValue::auto_value(),
            position_anchor_has_value: false,
            position_anchor_retained_name: 0,
            vertical_align_is_keyword: false,
            vertical_align_keyword: 0,
            vertical_align_value: FfiSizeValue::auto_value(),
            first_available_font: std::ptr::null(),
            font_ascent: 0.0,
            font_descent: 0.0,
            font_x_height: 0.0,
            box_sizing_for_aspect_ratio: 0,
            css_preferred_aspect_ratio_numerator: CssPixels::default(),
            css_preferred_aspect_ratio_denominator: CssPixels::default(),
            aspect_ratio_uses_natural_when_available: false,
            column_width: FfiSizeValue::auto_value(),
            column_count_has_value: false,
            column_count: 0,
            containment_bits: 0,
            text_indent: FfiSizeValue::auto_value(),
            text_indent_each_line: false,
            text_indent_hanging: false,
            tab_size_is_number: false,
            tab_size: CssPixels::default(),
            tab_size_number: 0.0,
        }
    }
}

impl FfiResidualStyleValues {
    fn release_handles(
        self,
        release_calc_handle: FfiReleaseCalcHandleCallback,
        release_anchor_name_handle: FfiReleaseAnchorNameHandleCallback,
    ) {
        self.anchor_inset_top.release_bridge_calc_handle(release_calc_handle);
        self.anchor_inset_right.release_bridge_calc_handle(release_calc_handle);
        self.anchor_inset_bottom.release_bridge_calc_handle(release_calc_handle);
        self.anchor_inset_left.release_bridge_calc_handle(release_calc_handle);
        self.vertical_align_value.release_bridge_calc_handle(release_calc_handle);
        self.column_width.release_bridge_calc_handle(release_calc_handle);
        self.text_indent.release_bridge_calc_handle(release_calc_handle);
        if self.position_anchor_retained_name != 0 {
            // SAFETY: The C++ residual decode transferred one raw fly-string
            // reference for this node's position-anchor name.
            unsafe {
                release_anchor_name_handle(self.position_anchor_retained_name);
            }
        }
    }
}

pub type FfiDecodeResidualStyleCallback =
    unsafe extern "C" fn(*mut c_void, *const FfiStylePayloads) -> FfiResidualStyleValues;

#[derive(Clone, Copy)]
struct StyleReader {
    payloads: FfiStylePayloads,
    schema: &'static StyleSchema,
}

impl StyleReader {
    fn new(payloads: FfiStylePayloads, schema: &'static StyleSchema) -> Self {
        Self { payloads, schema }
    }

    #[inline]
    fn address(&self, field: FfiStyleField, encoding: FfiStyleFieldEncoding) -> *const u8 {
        let schema = &self.schema.0[field as usize];
        debug_assert_eq!(schema.encoding, encoding);
        let payload = self.payloads.groups[schema.group_index as usize];
        debug_assert!(!payload.is_null());
        unsafe { (payload as *const u8).add(schema.offset as usize) }
    }

    #[inline]
    fn u8(&self, field: FfiStyleField) -> u8 {
        unsafe { self.address(field, FfiStyleFieldEncoding::U8).read_unaligned() }
    }

    #[inline]
    fn bool(&self, field: FfiStyleField) -> bool {
        let value = unsafe { self.address(field, FfiStyleFieldEncoding::Bool).read_unaligned() };
        debug_assert!(value <= 1);
        value != 0
    }

    #[inline]
    fn css_pixels(&self, field: FfiStyleField) -> CssPixels {
        let raw = unsafe { (self.address(field, FfiStyleFieldEncoding::CssPixels) as *const i32).read_unaligned() };
        CssPixels::from_raw(raw)
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
}

const SIZE_FIELD_COUNT: usize = 25;

pub(crate) struct StyleDecodeCache {
    reader: StyleReader,
    sizes: [Cell<FfiSizeValue>; SIZE_FIELD_COUNT],
    size_presence: Cell<u32>,
    residual: RefCell<Option<FfiResidualStyleValues>>,
    release_calc_handle: FfiReleaseCalcHandleCallback,
    release_anchor_name_handle: FfiReleaseAnchorNameHandleCallback,
}

impl StyleDecodeCache {
    fn new(
        reader: StyleReader,
        release_calc_handle: FfiReleaseCalcHandleCallback,
        release_anchor_name_handle: FfiReleaseAnchorNameHandleCallback,
    ) -> Self {
        Self {
            reader,
            sizes: std::array::from_fn(|_| Cell::new(FfiSizeValue::auto_value())),
            size_presence: Cell::new(0),
            residual: RefCell::new(None),
            release_calc_handle,
            release_anchor_name_handle,
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

    fn cached_residual(&self) -> Option<FfiResidualStyleValues> {
        *self.residual.borrow()
    }

    fn cache_residual(&self, value: FfiResidualStyleValues) {
        let mut residual = self.residual.borrow_mut();
        assert!(residual.is_none());
        *residual = Some(value);
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
        if let Some(residual) = *self.residual.get_mut() {
            residual.release_handles(self.release_calc_handle, self.release_anchor_name_handle);
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
}

pub(crate) struct StyleDecodeValues {
    scalars: DecodedStyleScalars,
    cache: StyleDecodeCache,
}

impl StyleDecodeValues {
    pub(crate) fn new(
        payloads: FfiStylePayloads,
        display: FfiDisplay,
        release_calc_handle: FfiReleaseCalcHandleCallback,
        release_anchor_name_handle: FfiReleaseAnchorNameHandleCallback,
    ) -> Self {
        let reader = StyleReader::new(payloads, style_schema());
        Self {
            scalars: DecodedStyleScalars::decode(&reader, display),
            cache: StyleDecodeCache::new(reader, release_calc_handle, release_anchor_name_handle),
        }
    }

    pub(crate) fn replace_size(&self, field: SizeField, value: FfiSizeValue) {
        self.cache.replace_size(field, value);
    }
}

/// A thin Rust-only view over a node's immutable computed-value group
/// payloads.
///
/// Plain fields are decoded once into `DecodedStyleScalars`. Rust-owned
/// complex payloads are interpreted in-crate; the first read of any genuinely
/// C++-owned field fetches the one residual batch and caches it for the node.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    scalars: &'a DecodedStyleScalars,
    cache: &'a StyleDecodeCache,
    callback_context: *mut c_void,
    decode_residual_callback: FfiDecodeResidualStyleCallback,
    vertical_align_override: u16,
}

impl DecodedStyleScalars {
    fn decode(reader: &StyleReader, display: FfiDisplay) -> Self {
        let inherited_box = reader.inherited_box();
        let alignment = reader.alignment();
        let inherited_table = reader.inherited_table();
        Self {
            display,
            border_top_width: reader.css_pixels(FfiStyleField::BorderTopWidth),
            border_right_width: reader.css_pixels(FfiStyleField::BorderRightWidth),
            border_bottom_width: reader.css_pixels(FfiStyleField::BorderBottomWidth),
            border_left_width: reader.css_pixels(FfiStyleField::BorderLeftWidth),
            border_top_style: reader.u8(FfiStyleField::BorderTopStyle),
            border_right_style: reader.u8(FfiStyleField::BorderRightStyle),
            border_bottom_style: reader.u8(FfiStyleField::BorderBottomStyle),
            border_left_style: reader.u8(FfiStyleField::BorderLeftStyle),
            position: reader.u8(FfiStyleField::Position),
            float_: reader.u8(FfiStyleField::Float),
            clear: reader.u8(FfiStyleField::Clear),
            writing_mode: inherited_box.writing_mode,
            direction: inherited_box.direction,
            text_align: reader.u8(FfiStyleField::TextAlign),
            text_justify: reader.u8(FfiStyleField::TextJustify),
            white_space_collapse: reader.u8(FfiStyleField::WhiteSpaceCollapse),
            text_wrap_mode: reader.u8(FfiStyleField::TextWrapMode),
            line_height: reader.css_pixels(FfiStyleField::LineHeight),
            font_size: reader.css_pixels(FfiStyleField::FontSize),
            box_sizing: reader.u8(FfiStyleField::BoxSizing),
            overflow_x: reader.u8(FfiStyleField::OverflowX),
            overflow_y: reader.u8(FfiStyleField::OverflowY),
            text_overflow: reader.u8(FfiStyleField::TextOverflow),
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
            table_layout: reader.u8(FfiStyleField::TableLayout),
            visibility: inherited_box.visibility,
            letter_spacing: reader.css_pixels(FfiStyleField::LetterSpacing),
            word_spacing: reader.css_pixels(FfiStyleField::WordSpacing),
            unicode_bidi: reader.u8(FfiStyleField::UnicodeBidi),
            grid_auto_flow_row: reader.bool(FfiStyleField::GridAutoFlowRow),
            grid_auto_flow_dense: reader.bool(FfiStyleField::GridAutoFlowDense),
        }
    }
}

impl<'a> StyleValues<'a> {
    #[inline]
    pub(crate) fn new(
        values: &'a StyleDecodeValues,
        callback_context: *mut c_void,
        decode_residual_callback: FfiDecodeResidualStyleCallback,
    ) -> Self {
        Self {
            scalars: &values.scalars,
            cache: &values.cache,
            callback_context,
            decode_residual_callback,
            vertical_align_override: u16::MAX,
        }
    }

    fn residual(self) -> FfiResidualStyleValues {
        if let Some(value) = self.cache.cached_residual() {
            return value;
        }
        // SAFETY: The payload snapshot and bridge callback remain live for
        // the synchronous layout pass.
        let value =
            unsafe { (self.decode_residual_callback)(self.callback_context, &raw const self.cache.reader.payloads) };
        if let Some(existing) = self.cache.cached_residual() {
            value.release_handles(self.cache.release_calc_handle, self.cache.release_anchor_name_handle);
            existing
        } else {
            self.cache.cache_residual(value);
            value
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
            SizeField::ColumnWidth | SizeField::TextIndent => unreachable!(),
        }
    }

    fn size_value(self, field: SizeField) -> FfiSizeValue {
        if let Some(value) = self.cache.cached_size(field) {
            return value;
        }
        let residual = match field {
            SizeField::InsetTop | SizeField::InsetRight | SizeField::InsetBottom | SizeField::InsetLeft
                if self.inset_has_anchor(field) =>
            {
                let values = self.residual();
                Some(match field {
                    SizeField::InsetTop => values.anchor_inset_top,
                    SizeField::InsetRight => values.anchor_inset_right,
                    SizeField::InsetBottom => values.anchor_inset_bottom,
                    SizeField::InsetLeft => values.anchor_inset_left,
                    _ => unreachable!(),
                })
            }
            SizeField::ColumnWidth => Some(self.residual().column_width),
            SizeField::TextIndent => Some(self.residual().text_indent),
            _ => None,
        };
        residual.unwrap_or_else(|| {
            let value = self.direct_size(field);
            self.cache.cache_size(field, value)
        })
    }

    pub(crate) fn with_vertical_align_keyword(mut self, keyword: u8) -> Self {
        self.vertical_align_override = keyword as u16;
        self
    }

    pub(crate) fn vertical_align_is_keyword(self) -> bool {
        self.vertical_align_override != u16::MAX || self.residual().vertical_align_is_keyword
    }

    pub(crate) fn vertical_align_keyword(self) -> u8 {
        if self.vertical_align_override != u16::MAX {
            self.vertical_align_override as u8
        } else {
            self.residual().vertical_align_keyword
        }
    }

    pub(crate) fn vertical_align_value(self) -> FfiSizeValue {
        self.residual().vertical_align_value
    }

    pub(crate) fn has_position_anchor(self) -> bool {
        self.residual().position_anchor_has_value
    }

    pub(crate) fn position_anchor_name(self) -> usize {
        self.residual().position_anchor_retained_name
    }

    pub(crate) fn first_available_font(self) -> *const c_void {
        self.residual().first_available_font
    }

    pub(crate) fn font_ascent(self) -> f32 {
        self.residual().font_ascent
    }

    pub(crate) fn font_descent(self) -> f32 {
        self.residual().font_descent
    }

    pub(crate) fn font_x_height(self) -> f32 {
        self.residual().font_x_height
    }

    pub(crate) fn box_sizing_for_aspect_ratio(self) -> u8 {
        self.residual().box_sizing_for_aspect_ratio
    }

    pub(crate) fn css_preferred_aspect_ratio_numerator(self) -> CssPixels {
        self.residual().css_preferred_aspect_ratio_numerator
    }

    pub(crate) fn css_preferred_aspect_ratio_denominator(self) -> CssPixels {
        self.residual().css_preferred_aspect_ratio_denominator
    }

    pub(crate) fn aspect_ratio_uses_natural_when_available(self) -> bool {
        self.residual().aspect_ratio_uses_natural_when_available
    }

    pub(crate) fn flex_basis_is_content(self) -> bool {
        self.cache.reader.alignment().flex_basis.is_content
    }

    pub(crate) fn flex_basis(self) -> FfiSizeValue {
        self.size_value(SizeField::FlexBasis)
    }

    pub(crate) fn has_column_count(self) -> bool {
        self.residual().column_count_has_value
    }

    pub(crate) fn column_count(self) -> i32 {
        self.residual().column_count
    }

    pub(crate) fn containment_bits(self) -> u8 {
        self.residual().containment_bits
    }

    pub(crate) fn text_indent_each_line(self) -> bool {
        self.residual().text_indent_each_line
    }

    pub(crate) fn text_indent_hanging(self) -> bool {
        self.residual().text_indent_hanging
    }

    pub(crate) fn tab_size_is_number(self) -> bool {
        self.residual().tab_size_is_number
    }

    pub(crate) fn tab_size(self) -> CssPixels {
        self.residual().tab_size
    }

    pub(crate) fn tab_size_number(self) -> f64 {
        self.residual().tab_size_number
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
