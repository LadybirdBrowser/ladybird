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
#[derive(Clone, Copy)]
pub(crate) struct ResolvedInsetOverride {
    pub(crate) is_auto: bool,
    pub(crate) px: CssPixels,
}

#[derive(Clone, Copy)]
pub(crate) enum InsetValue<'a> {
    FromStyle(&'a ComputedLengthPercentageOrAuto),
    BareAnchor(&'a crate::css::style_value::StyleValueData),
    Resolved(ResolvedInsetOverride),
}

impl InsetValue<'_> {
    pub(crate) fn auto_value() -> Self {
        Self::Resolved(ResolvedInsetOverride {
            is_auto: true,
            px: CssPixels::default(),
        })
    }

    pub(crate) fn is_auto(self) -> bool {
        match self {
            Self::FromStyle(value) => value.is_auto(),
            Self::BareAnchor(_) => false,
            Self::Resolved(resolved) => resolved.is_auto,
        }
    }

    pub(crate) fn to_px(self, reference: CssPixels) -> CssPixels {
        match self {
            Self::FromStyle(value) => value.to_px(reference),
            Self::BareAnchor(wrapper) => resolve_calc_to_px(std::ptr::from_ref(wrapper).cast(), reference),
            Self::Resolved(resolved) if resolved.is_auto => CssPixels::default(),
            Self::Resolved(resolved) => resolved.px,
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

/// The layout pass view of a node's computed style: the pure payload view
/// plus the resolved anchor insets the abspos engine threads to the four
/// inset reads of the box it is placing, and the line builder's
/// vertical-align keyword substitution. Every other read derefs to
/// ComputedValuesView.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    style: ComputedValuesView<'a>,
    resolved_anchor_insets: Option<&'a formatting_context::ResolvedAnchorInsets>,
    vertical_align_override: u16,
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
            vertical_align_override: u16::MAX,
        }
    }

    #[inline]
    pub(crate) fn for_node(callbacks: &FfiLayoutFcCallbacks, node: Node) -> StyleValues<'static> {
        StyleValues::new(callbacks.style_payloads(node))
    }

    pub(crate) fn with_resolved_insets(
        mut self,
        resolved: Option<&'a formatting_context::ResolvedAnchorInsets>,
    ) -> Self {
        self.resolved_anchor_insets = resolved;
        self
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

    pub(crate) fn with_vertical_align_keyword(mut self, keyword: u8) -> Self {
        self.vertical_align_override = keyword as u16;
        self
    }

    pub(crate) fn vertical_align_is_keyword(self) -> bool {
        self.vertical_align_override != u16::MAX || self.style.box_values().vertical_align.is_keyword
    }

    pub(crate) fn vertical_align_keyword(self) -> u8 {
        if self.vertical_align_override != u16::MAX {
            self.vertical_align_override as u8
        } else {
            self.style.box_values().vertical_align.keyword
        }
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
