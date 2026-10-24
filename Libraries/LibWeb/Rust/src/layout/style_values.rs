/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// The four inset properties.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum InsetField {
    Top,
    Right,
    Bottom,
    Left,
}

/// A resolved px-or-auto inset produced by anchor resolution. It takes
/// precedence over every style read and reports
/// contains_anchor_function() == false.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ResolvedInsetOverride {
    Auto,
    Pixels(CssPixels),
}

#[derive(Clone, Copy)]
pub(crate) enum InsetValue<'a> {
    FromStyle(&'a ComputedLengthPercentageOrAuto),
    BareAnchor(&'a crate::css::style_value::StyleValueData),
    Resolved(ResolvedInsetOverride),
}

impl InsetValue<'_> {
    pub(crate) fn auto_value() -> Self {
        Self::Resolved(ResolvedInsetOverride::Auto)
    }

    pub(crate) fn is_auto(self) -> bool {
        match self {
            Self::FromStyle(value) => value.is_auto(),
            Self::BareAnchor(_) => false,
            Self::Resolved(resolved) => matches!(resolved, ResolvedInsetOverride::Auto),
        }
    }

    pub(crate) fn to_px(self, reference: CssPixels) -> CssPixels {
        match self {
            Self::FromStyle(value) => value.to_px(reference),
            Self::BareAnchor(wrapper) => resolve_calc_to_px(std::ptr::from_ref(wrapper).cast(), reference),
            Self::Resolved(ResolvedInsetOverride::Auto) => CssPixels::default(),
            Self::Resolved(ResolvedInsetOverride::Pixels(px)) => px,
        }
    }

    pub(crate) fn contains_percentage(self) -> bool {
        match self {
            Self::FromStyle(value) => value.contains_percentage(),
            Self::BareAnchor(_) | Self::Resolved(_) => false,
        }
    }

    pub(crate) fn contains_anchor_function(self) -> bool {
        match self {
            Self::FromStyle(value) => value
                .length_percentage()
                .is_some_and(|length_percentage| length_percentage.contains_anchor_function()),
            Self::BareAnchor(_) => true,
            Self::Resolved(_) => false,
        }
    }

    /// The calculated style value carrying the anchor() function, for the
    /// abspos engine's anchor-aware resolution.
    pub(crate) fn anchor_bearing_calculated(self) -> *const c_void {
        match self {
            Self::FromStyle(value) => value
                .length_percentage()
                .expect("anchor-bearing inset must hold a style value")
                .calculated_pointer(),
            Self::BareAnchor(wrapper) => std::ptr::from_ref(wrapper).cast(),
            Self::Resolved(_) => unreachable!("resolved inset overrides never carry anchor functions"),
        }
    }
}

/// A sizing property whose computed value holds anchor-size() functions, as the abspos engine resolved it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ResolvedSizeOverride {
    Length(CssPixels),
    Keyword(ComputedSizeKind),
}

/// The sizing and margin properties of one positioned box whose computed values hold anchor-size() functions, with
/// those functions resolved by the abspos engine. A property without one stays None and reads from style. Margins
/// also carry the zero that auto resolves to in the axes a position area or anchor-center aligns the box in.
/// https://drafts.csswg.org/css-anchor-position-1/#anchor-size-fn
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ResolvedAnchorSizes {
    pub(crate) width: Option<ResolvedSizeOverride>,
    pub(crate) min_width: Option<ResolvedSizeOverride>,
    pub(crate) max_width: Option<ResolvedSizeOverride>,
    pub(crate) height: Option<ResolvedSizeOverride>,
    pub(crate) min_height: Option<ResolvedSizeOverride>,
    pub(crate) max_height: Option<ResolvedSizeOverride>,
    pub(crate) margin_top: Option<CssPixels>,
    pub(crate) margin_right: Option<CssPixels>,
    pub(crate) margin_bottom: Option<CssPixels>,
    pub(crate) margin_left: Option<CssPixels>,
}

impl ResolvedAnchorSizes {
    pub(crate) fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}

/// The computed values `ResolvedAnchorSizes` stands for, which style reads borrow.
#[derive(Default)]
pub(crate) struct AnchorResolvedValues {
    width: Option<ComputedSize>,
    min_width: Option<ComputedSize>,
    max_width: Option<ComputedSize>,
    height: Option<ComputedSize>,
    min_height: Option<ComputedSize>,
    max_height: Option<ComputedSize>,
    margin_top: Option<ComputedLengthPercentageOrAuto>,
    margin_right: Option<ComputedLengthPercentageOrAuto>,
    margin_bottom: Option<ComputedLengthPercentageOrAuto>,
    margin_left: Option<ComputedLengthPercentageOrAuto>,
}

impl std::fmt::Debug for AnchorResolvedValues {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_struct("AnchorResolvedValues").finish_non_exhaustive()
    }
}

impl AnchorResolvedValues {
    pub(crate) fn new(resolved: &ResolvedAnchorSizes) -> Self {
        let size = |value: Option<ResolvedSizeOverride>| {
            value.map(|value| match value {
                ResolvedSizeOverride::Length(px) => ComputedSize {
                    kind: ComputedSizeKind::Length,
                    value: ComputedStyleValueHandle::length(px.to_double()),
                },
                ResolvedSizeOverride::Keyword(kind) => ComputedSize::keyword(kind),
            })
        };
        let margin = |value: Option<CssPixels>| {
            value.map(|px| ComputedLengthPercentageOrAuto {
                is_auto: false,
                value: ComputedStyleValueHandle::length(px.to_double()),
            })
        };
        Self {
            width: size(resolved.width),
            min_width: size(resolved.min_width),
            max_width: size(resolved.max_width),
            height: size(resolved.height),
            min_height: size(resolved.min_height),
            max_height: size(resolved.max_height),
            margin_top: margin(resolved.margin_top),
            margin_right: margin(resolved.margin_right),
            margin_bottom: margin(resolved.margin_bottom),
            margin_left: margin(resolved.margin_left),
        }
    }
}

/// A padding read: the style's, or zero for a non-devolvable widget.
#[derive(Clone, Copy)]
pub(crate) enum PaddingValue<'a> {
    FromStyle(&'a ComputedLengthPercentageOrAuto),
    Zero,
}

impl PaddingValue<'_> {
    pub(crate) fn to_px(self, reference: CssPixels) -> CssPixels {
        match self {
            Self::FromStyle(value) => value.to_px(reference),
            Self::Zero => CssPixels::default(),
        }
    }

    pub(crate) fn contains_percentage(self) -> bool {
        match self {
            Self::FromStyle(value) => value.contains_percentage(),
            Self::Zero => false,
        }
    }
}

/// The layout pass view of a node's computed style: the pure payload view
/// plus the resolved anchor insets the abspos engine threads to the four
/// inset reads of the box it is placing, the line builder's vertical-align
/// keyword substitution, and the padding and border widths a non-devolvable
/// widget drops. Every other read derefs to ComputedValuesView.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    style: ComputedValuesView<'a>,
    resolved_anchor_insets: Option<&'a formatting_context::ResolvedAnchorInsets>,
    anchor_resolved_values: Option<&'a AnchorResolvedValues>,
    vertical_align_override: Option<u8>,
    is_non_devolvable_widget: bool,
}

impl<'a> std::ops::Deref for StyleValues<'a> {
    type Target = ComputedValuesView<'a>;

    fn deref(&self) -> &ComputedValuesView<'a> {
        &self.style
    }
}

impl<'a> StyleValues<'a> {
    #[inline]
    pub(crate) fn new(payloads: &'a FfiStylePayloads) -> Self {
        Self {
            style: ComputedValuesView::new(&payloads.groups),
            resolved_anchor_insets: None,
            anchor_resolved_values: None,
            vertical_align_override: None,
            is_non_devolvable_widget: false,
        }
    }

    #[inline]
    pub(crate) fn for_node(callbacks: &LayoutPass<'a>, node: Node) -> Self {
        StyleValues::new(callbacks.style_payloads(node)).for_kind(callbacks.node_data(node).kind.get())
    }

    /// The view for a box of `kind`. A non-devolvable widget is drawn natively to fill its box, so author padding and
    /// borders don't apply to it.
    #[inline]
    pub(crate) fn for_kind(mut self, kind: NodeKind) -> Self {
        self.is_non_devolvable_widget = node_facts::kind_is_non_devolvable_widget(kind);
        self
    }

    pub(crate) fn with_resolved_insets(
        mut self,
        resolved: Option<&'a formatting_context::ResolvedAnchorInsets>,
    ) -> Self {
        self.resolved_anchor_insets = resolved;
        self
    }

    pub(crate) fn with_anchor_resolved_values(mut self, resolved: Option<&'a AnchorResolvedValues>) -> Self {
        self.anchor_resolved_values = resolved;
        self
    }

    pub(crate) fn width(self) -> &'a ComputedSize {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.width.as_ref())
            .unwrap_or_else(|| unresolvable_anchor_size_or(self.style.width(), auto_computed_size()))
    }

    pub(crate) fn min_width(self) -> &'a ComputedSize {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.min_width.as_ref())
            .unwrap_or_else(|| unresolvable_anchor_size_or(self.style.min_width(), auto_computed_size()))
    }

    pub(crate) fn max_width(self) -> &'a ComputedSize {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.max_width.as_ref())
            .unwrap_or_else(|| unresolvable_anchor_size_or(self.style.max_width(), none_computed_size()))
    }

    pub(crate) fn height(self) -> &'a ComputedSize {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.height.as_ref())
            .unwrap_or_else(|| unresolvable_anchor_size_or(self.style.height(), auto_computed_size()))
    }

    pub(crate) fn min_height(self) -> &'a ComputedSize {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.min_height.as_ref())
            .unwrap_or_else(|| unresolvable_anchor_size_or(self.style.min_height(), auto_computed_size()))
    }

    pub(crate) fn max_height(self) -> &'a ComputedSize {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.max_height.as_ref())
            .unwrap_or_else(|| unresolvable_anchor_size_or(self.style.max_height(), none_computed_size()))
    }

    pub(crate) fn margin_top(self) -> &'a ComputedLengthPercentageOrAuto {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.margin_top.as_ref())
            .unwrap_or_else(|| self.style.margin_top())
    }

    pub(crate) fn margin_right(self) -> &'a ComputedLengthPercentageOrAuto {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.margin_right.as_ref())
            .unwrap_or_else(|| self.style.margin_right())
    }

    pub(crate) fn margin_bottom(self) -> &'a ComputedLengthPercentageOrAuto {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.margin_bottom.as_ref())
            .unwrap_or_else(|| self.style.margin_bottom())
    }

    pub(crate) fn margin_left(self) -> &'a ComputedLengthPercentageOrAuto {
        self.anchor_resolved_values
            .and_then(|resolved| resolved.margin_left.as_ref())
            .unwrap_or_else(|| self.style.margin_left())
    }

    fn bare_anchor_wrapper(self, field: InsetField) -> Option<&'a crate::css::style_value::StyleValueData> {
        let values = self.style.surround();
        let handle = match field {
            InsetField::Top => &values.top_anchor_inset_wrapper,
            InsetField::Right => &values.right_anchor_inset_wrapper,
            InsetField::Bottom => &values.bottom_anchor_inset_wrapper,
            InsetField::Left => &values.left_anchor_inset_wrapper,
        };
        // SAFETY: The node's style group payload retains the wrapper for the
        // view's lifetime.
        unsafe {
            handle
                .pointer
                .cast::<crate::css::style_value::StyleValueData>()
                .as_ref()
        }
    }

    fn inset_value(self, field: InsetField) -> InsetValue<'a> {
        if let Some(resolved) = self
            .resolved_anchor_insets
            .and_then(|insets| insets.override_for(field))
        {
            return InsetValue::Resolved(resolved);
        }
        if let Some(wrapper) = self.bare_anchor_wrapper(field) {
            return InsetValue::BareAnchor(wrapper);
        }
        let values = self.style.surround();
        InsetValue::FromStyle(match field {
            InsetField::Top => &values.inset.top,
            InsetField::Right => &values.inset.right,
            InsetField::Bottom => &values.inset.bottom,
            InsetField::Left => &values.inset.left,
        })
    }

    pub(crate) fn inset_top(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Top)
    }

    pub(crate) fn inset_right(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Right)
    }

    pub(crate) fn inset_bottom(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Bottom)
    }

    pub(crate) fn inset_left(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Left)
    }

    // AD-HOC: CSS UI only says a non-devolvable widget keeps its native appearance whatever its border; it doesn't say
    //         the border takes no room, nor anything about padding. A native control fills its box, so its border
    //         widths and its padding read as zero.
    fn padding_unless_non_devolvable(self, padding: &'a ComputedLengthPercentageOrAuto) -> PaddingValue<'a> {
        if self.is_non_devolvable_widget {
            PaddingValue::Zero
        } else {
            PaddingValue::FromStyle(padding)
        }
    }

    fn border_width_unless_non_devolvable(self, width: CssPixels) -> CssPixels {
        if self.is_non_devolvable_widget {
            CssPixels::default()
        } else {
            width
        }
    }

    pub(crate) fn padding_top(self) -> PaddingValue<'a> {
        self.padding_unless_non_devolvable(self.style.padding_top())
    }

    pub(crate) fn padding_right(self) -> PaddingValue<'a> {
        self.padding_unless_non_devolvable(self.style.padding_right())
    }

    pub(crate) fn padding_bottom(self) -> PaddingValue<'a> {
        self.padding_unless_non_devolvable(self.style.padding_bottom())
    }

    pub(crate) fn padding_left(self) -> PaddingValue<'a> {
        self.padding_unless_non_devolvable(self.style.padding_left())
    }

    pub(crate) fn border_top_width(self) -> CssPixels {
        self.border_width_unless_non_devolvable(self.style.border_top_width())
    }

    pub(crate) fn border_right_width(self) -> CssPixels {
        self.border_width_unless_non_devolvable(self.style.border_right_width())
    }

    pub(crate) fn border_bottom_width(self) -> CssPixels {
        self.border_width_unless_non_devolvable(self.style.border_bottom_width())
    }

    pub(crate) fn border_left_width(self) -> CssPixels {
        self.border_width_unless_non_devolvable(self.style.border_left_width())
    }

    pub(crate) fn with_vertical_align_keyword(mut self, keyword: u8) -> Self {
        self.vertical_align_override = Some(keyword);
        self
    }

    pub(crate) fn vertical_align_is_keyword(self) -> bool {
        self.vertical_align_override.is_some() || self.style.box_values().vertical_align.is_keyword
    }

    pub(crate) fn vertical_align_keyword(self) -> u8 {
        self.vertical_align_override
            .unwrap_or_else(|| self.style.box_values().vertical_align.keyword)
    }
}

/// `size`, or `initial` when `size` holds an anchor function that is unresolvable and has no fallback value.
/// https://drafts.csswg.org/css-anchor-position-1/#anchor-size-resolution
/// If no fallback value is specified, it makes the declaration referencing it invalid at computed-value time.
/// NB: Outside of the resolutions the abspos engine installs, an anchor function is unresolvable.
fn unresolvable_anchor_size_or<'a>(size: &'a ComputedSize, initial: &'static ComputedSize) -> &'a ComputedSize {
    if !size.is_length_percentage() || !size.length_percentage().contains_anchor_function() {
        return size;
    }
    let mut percentage_basis = CssPixels::default();
    // SAFETY: The callback context is the percentage basis, live for this synchronous resolution.
    let resolved = unsafe {
        resolve_calc_with_external_resolutions(
            size.length_percentage().calculated_pointer(),
            percentage_basis,
            (&raw mut percentage_basis).cast(),
            Some(resolve_unresolvable_anchor_function),
        )
    };
    if resolved.resolved { size } else { initial }
}

/// A px calc node, holding one reference for the external-resolution snapshot that releases it.
pub(crate) fn calc_node_create_px_dimension(value: f64) -> *const c_void {
    crate::css::calc::rust_calc_node_create_numeric_dimension(
        formatting_context::CALC_NUMERIC_KIND_LENGTH,
        value,
        crate::css::style_compute::px_length_unit(),
    )
    .cast()
}

/// The calc node the fallback value of an unresolvable anchor() or anchor-size() function resolves to, or null when
/// it has none. Nested anchor functions resolve through `resolve_non_math_function`.
/// https://drafts.csswg.org/css-anchor-position-1/#anchor-resolution
///
/// # Safety
/// `callback_context` must be what `resolve_non_math_function` expects, and stay live for the resolution.
pub(crate) unsafe fn anchor_function_fallback_node(
    fallback_value: &crate::css::style_value::RetainedStyleValueData,
    percentage_basis: CssPixels,
    callback_context: *mut c_void,
    resolve_non_math_function: unsafe extern "C" fn(*mut c_void, *const c_void) -> *const c_void,
) -> *const c_void {
    use crate::css::style_value::StyleValueData;
    match fallback_value.optional_data() {
        None => std::ptr::null(),
        Some(StyleValueData::Length { value, unit }) => {
            // Computed anchor() fallbacks hold absolute lengths only; relative
            // units carry NaN canonical ratios.
            let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
            assert!(ratio.is_finite());
            calc_node_create_px_dimension(CssPixels::nearest_value_for(*value * ratio).to_double())
        }
        Some(StyleValueData::Percentage { value }) => {
            calc_node_create_px_dimension(percentage_basis.to_double() * (value / 100.0))
        }
        Some(StyleValueData::Calculated { .. }) => {
            // SAFETY: The anchor function shell retains the fallback calculation for
            // this synchronous resolution.
            let resolved = unsafe {
                resolve_calc_with_external_resolutions(
                    fallback_value.pointer().cast(),
                    percentage_basis,
                    callback_context,
                    Some(resolve_non_math_function),
                )
            };
            if !resolved.resolved {
                return std::ptr::null();
            }
            calc_node_create_px_dimension(resolved.value)
        }
        // SAFETY: The outer anchor function shell retains the nested one for this synchronous resolution.
        Some(StyleValueData::Anchor { .. } | StyleValueData::AnchorSize { .. }) => unsafe {
            resolve_non_math_function(callback_context, fallback_value.pointer().cast())
        },
        Some(_) => {
            unreachable!("anchor function fallback must be a length, percentage, calculation or anchor function")
        }
    }
}

/// Resolves an anchor() or anchor-size() function outside the abspos engine, where it is unresolvable and resolves
/// to its fallback value. The callback context is the percentage basis.
unsafe extern "C" fn resolve_unresolvable_anchor_function(context: *mut c_void, shell: *const c_void) -> *const c_void {
    use crate::css::style_value::StyleValueData;
    // SAFETY: The context is the percentage basis resolve_calc_with_unresolvable_anchor_functions passes.
    let percentage_basis = unsafe { *context.cast::<CssPixels>() };
    // SAFETY: `shell` is the live style value the calc core's non-math-function node retains.
    match unsafe { &*shell.cast::<StyleValueData>() } {
        StyleValueData::Anchor { fallback_value, .. } | StyleValueData::AnchorSize { fallback_value, .. } => unsafe {
            anchor_function_fallback_node(
                fallback_value,
                percentage_basis,
                context,
                resolve_unresolvable_anchor_function,
            )
        },
        _ => std::ptr::null(),
    }
}

/// Resolves `calculated`, whose anchor functions are unresolvable, to px. A function without a fallback value makes
/// the declaration invalid at computed-value time, which this approximates as zero.
pub(crate) fn resolve_calc_with_unresolvable_anchor_functions(
    calculated: *const c_void,
    mut percentage_basis: CssPixels,
) -> f64 {
    // SAFETY: The callback context is the percentage basis, live for this synchronous resolution.
    let resolved = unsafe {
        resolve_calc_with_external_resolutions(
            calculated,
            percentage_basis,
            (&raw mut percentage_basis).cast(),
            Some(resolve_unresolvable_anchor_function),
        )
    };
    if resolved.resolved { resolved.value } else { 0.0 }
}

pub(crate) unsafe fn resolve_calc_with_external_resolutions(
    calculated: *const c_void,
    percentage_basis: CssPixels,
    callback_context: *mut c_void,
    resolve_non_math_function: Option<unsafe extern "C" fn(*mut c_void, *const c_void) -> *const c_void>,
) -> crate::css::calc::FfiResolvedCalc {
    use crate::css::calc::{
        CalcExternalResolutionKind, CalcNode, calc_root_from_calculated, collect_external_resolutions,
        resolve_calculated,
    };

    let context = px_calc_resolution_context(percentage_basis);
    let root = unsafe { &*calc_root_from_calculated(calculated) };
    let mut externals = Vec::new();
    collect_external_resolutions(root, &mut externals);
    if let Some(resolve_non_math_function) = resolve_non_math_function {
        for resolution in &mut externals {
            if resolution.kind == CalcExternalResolutionKind::NonMathFunction {
                let node = unsafe { resolve_non_math_function(callback_context, resolution.source.pointer().cast()) }
                    .cast::<CalcNode>();
                // SAFETY: The resolver hands back one reference to each node it resolves.
                resolution.resolved_node = (!node.is_null()).then(|| unsafe { std::sync::Arc::from_raw(node) });
            }
        }
    }
    resolve_calculated(
        unsafe { &*calculated.cast::<crate::css::style_value::StyleValueData>() },
        &context,
        &externals,
        true,
    )
}
