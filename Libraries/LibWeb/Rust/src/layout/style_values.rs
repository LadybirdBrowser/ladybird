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
