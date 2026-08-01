/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub type FfiReleaseAnchorNameHandleCallback = unsafe extern "C" fn(usize);

/// The four inset properties; the discriminant indexes the anchor-inset
/// store fields.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum InsetField {
    Top = 0,
    Right = 1,
    Bottom = 2,
    Left = 3,
}

/// A resolved px-or-auto inset written back by anchor resolution.
#[derive(Clone, Copy)]
pub(crate) struct ResolvedInsetOverride {
    pub(crate) is_auto: bool,
    pub(crate) px: CssPixels,
}

#[derive(Default)]
struct AnchorInsetField {
    /// Resolved value written by replace_resolved_anchor_insets. It takes
    /// precedence over every style read and reports
    /// contains_anchor_function() == false; the abspos engine's early-out on
    /// re-entry depends on both properties.
    resolved_override: Cell<Option<ResolvedInsetOverride>>,
    /// Memoized in-crate calculated wrapper for a bare anchor() inset: the
    /// single owner of the Arc that the returned inset values borrow.
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

    fn override_for(&self, slot_index: u32, field: InsetField) -> Option<ResolvedInsetOverride> {
        if !self.any_overrides.get() {
            return None;
        }
        self.slots.get(slot_index)?.fields[field as usize].resolved_override.get()
    }

    fn memoized_bare_anchor_wrapper(
        &self,
        slot_index: u32,
        field: InsetField,
        build_wrapper: impl FnOnce() -> std::sync::Arc<crate::css::style_value::StyleValueData>,
    ) -> &crate::css::style_value::StyleValueData {
        self.slot(slot_index).fields[field as usize]
            .wrapper
            .get_or_init(build_wrapper)
    }

    pub(crate) fn set_override(&self, slot_index: u32, field: InsetField, value: ResolvedInsetOverride) {
        self.slot(slot_index).fields[field as usize].resolved_override.set(Some(value));
        self.any_overrides.set(true);
    }
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
/// plus the per-LayoutState anchor-inset store the four inset reads consult
/// and the line builder's vertical-align keyword substitution. Every other
/// read derefs to ComputedValuesView.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    style: ComputedValuesView<'a>,
    anchor_insets: &'a AnchorInsetStore,
    slot_index: u32,
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
    pub(crate) fn new(payloads: &'a FfiStylePayloads, anchor_insets: &'a AnchorInsetStore, slot_index: u32) -> Self {
        Self {
            style: ComputedValuesView::new(&payloads.groups),
            anchor_insets,
            slot_index,
            vertical_align_override: u16::MAX,
        }
    }

    fn anchor_inset_handle(self, field: InsetField) -> Option<&'a ComputedStyleValueHandle> {
        let values = self.style.surround();
        let handle = match field {
            InsetField::Top => &values.top_anchor_inset,
            InsetField::Right => &values.right_anchor_inset,
            InsetField::Bottom => &values.bottom_anchor_inset,
            InsetField::Left => &values.left_anchor_inset,
        };
        (!handle.pointer.is_null()).then_some(handle)
    }

    fn inset_value(self, field: InsetField) -> InsetValue<'a> {
        if let Some(resolved) = self.anchor_insets.override_for(self.slot_index, field) {
            return InsetValue::Resolved(resolved);
        }
        if let Some(handle) = self.anchor_inset_handle(field) {
            return InsetValue::BareAnchor(self.anchor_insets.memoized_bare_anchor_wrapper(
                self.slot_index,
                field,
                || {
                    // SAFETY: The handle is non-null, and the node's style
                    // group payload keeps the anchor value alive for the
                    // synchronous layout pass.
                    unsafe { crate::css::calc::create_anchor_inset_calculated(handle.pointer.cast()) }
                },
            ));
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
