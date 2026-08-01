/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lazy readers over the computed style group payload types, mirroring the
//! C++ `CSS::Size` / `CSS::LengthPercentage` method surface: every query
//! resolves on demand from the stored computed representation, so readers
//! need no intermediate decoded value.

// NB: The layout engine migrates onto these readers property group by
// property group; the allowance leaves together with the FfiSizeValue
// decode path once every group reads through them.
#![allow(dead_code)]

use crate::css::calc;
use crate::css::computed_value_types::{
    ComputedGap, ComputedLengthPercentageOrAuto, ComputedSize, ComputedSizeKind, ComputedStyleValueHandle,
};
use crate::css::css_pixels::CssPixels;
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
    let context = px_calc_resolution_context(percentage_basis);
    // SAFETY: The style value stays alive for the pass and the context
    // carries no host callbacks.
    let result = unsafe { calc::rust_calc_resolve(calculated, &raw const context, true) };
    assert!(result.resolved);
    CssPixels::nearest_value_for(result.value)
}

/// A borrowed computed `<length-percentage>`: a retained length, percentage
/// or calculated style value read in place, the Rust twin of the C++
/// `LengthPercentage::view` API.
#[derive(Clone, Copy)]
pub(crate) struct LengthPercentageRef<'a> {
    value: &'a StyleValueData,
}

impl LengthPercentageRef<'_> {
    pub(crate) fn is_length(self) -> bool {
        matches!(self.value, StyleValueData::Length { .. })
    }

    pub(crate) fn is_percentage(self) -> bool {
        matches!(self.value, StyleValueData::Percentage { .. })
    }

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

    pub(crate) fn is_length_percentage(&self) -> bool {
        !self.is_auto
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

#[cfg(test)]
mod tests {
    use super::*;

    fn handle_for(value: &StyleValueData) -> ComputedStyleValueHandle {
        ComputedStyleValueHandle {
            pointer: std::ptr::from_ref(value).cast(),
        }
    }

    #[test]
    fn absolute_length_resolves_through_the_unit_ratio() {
        let centimeter = crate::css::style_compute::LENGTH_UNIT_NAMES
            .iter()
            .position(|&name| name == "cm")
            .unwrap() as u8;
        let value = StyleValueData::Length {
            value: 1.0,
            unit: centimeter,
        };
        let length = handle_for(&value).length_percentage().unwrap();
        assert!(length.is_length());
        assert!(!length.contains_percentage());
        // 1cm = 96px/2.54 = 37.795..px rounds to 2419 subpixels.
        assert_eq!(length.absolute_length_to_px().raw_value(), 2419);
        assert_eq!(length.to_px(CssPixels::from_integer(100)).raw_value(), 2419);
    }

    #[test]
    fn percentage_to_px_truncates_where_lengths_round() {
        let value = StyleValueData::Percentage { value: 90.0 };
        let percentage = handle_for(&value).length_percentage().unwrap();
        assert!(percentage.is_percentage());
        assert!(percentage.contains_percentage());
        assert_eq!(percentage.as_fraction(), 0.9);
        // 90% of 1px is 57.6 subpixels: the percentage path truncates to 57
        // where the length path would round to 58.
        assert_eq!(percentage.to_px(CssPixels::from_raw(64)).raw_value(), 57);
    }

    #[test]
    fn nan_percentage_resolves_to_zero() {
        let value = StyleValueData::Percentage { value: f64::NAN };
        let percentage = handle_for(&value).length_percentage().unwrap();
        assert_eq!(percentage.to_px(CssPixels::from_integer(100)), CssPixels::default());
    }

    #[test]
    fn keyword_sizes_resolve_to_zero_without_percentages() {
        for kind in [
            ComputedSizeKind::Auto,
            ComputedSizeKind::MinContent,
            ComputedSizeKind::MaxContent,
            ComputedSizeKind::None,
        ] {
            let size = ComputedSize {
                kind,
                value: ComputedStyleValueHandle {
                    pointer: std::ptr::null(),
                },
            };
            assert_eq!(size.to_px(CssPixels::from_integer(100)), CssPixels::default());
            assert!(!size.contains_percentage());
        }
    }

    #[test]
    fn keyword_only_fit_content_has_no_available_space() {
        let size = ComputedSize {
            kind: ComputedSizeKind::FitContent,
            value: ComputedStyleValueHandle {
                pointer: std::ptr::null(),
            },
        };
        assert!(size.is_fit_content());
        assert!(size.fit_content_available_space().is_none());
        assert_eq!(size.to_px(CssPixels::from_integer(100)), CssPixels::default());
        assert!(!size.contains_percentage());
    }

    #[test]
    fn fit_content_argument_resolves_like_its_length_percentage() {
        let argument = StyleValueData::Percentage { value: 50.0 };
        let size = ComputedSize {
            kind: ComputedSizeKind::FitContent,
            value: handle_for(&argument),
        };
        assert!(size.fit_content_available_space().is_some());
        assert!(size.contains_percentage());
        assert_eq!(size.to_px(CssPixels::from_integer(100)), CssPixels::from_integer(50));
    }

    #[test]
    fn auto_computed_size_is_auto() {
        assert!(auto_computed_size().is_auto());
        assert!(!auto_computed_size().contains_percentage());
        assert_eq!(
            auto_computed_size().to_px(CssPixels::from_integer(100)),
            CssPixels::default()
        );
    }

    #[test]
    fn length_percentage_or_auto_resolves_auto_to_zero() {
        let auto = ComputedLengthPercentageOrAuto {
            is_auto: true,
            value: ComputedStyleValueHandle {
                pointer: std::ptr::null(),
            },
        };
        assert!(auto.is_auto());
        assert!(!auto.is_length_percentage());
        assert!(!auto.contains_percentage());
        assert_eq!(auto.to_px(CssPixels::from_integer(100)), CssPixels::default());

        let stored = StyleValueData::Percentage { value: 25.0 };
        let percentage = ComputedLengthPercentageOrAuto {
            is_auto: false,
            value: handle_for(&stored),
        };
        assert!(!percentage.is_auto());
        assert!(percentage.contains_percentage());
        assert_eq!(
            percentage.to_px(CssPixels::from_integer(100)),
            CssPixels::from_integer(25)
        );
    }

    #[test]
    fn normal_gap_resolves_to_zero() {
        let normal = ComputedGap {
            is_normal: true,
            value: ComputedStyleValueHandle {
                pointer: std::ptr::null(),
            },
        };
        assert!(normal.is_normal());
        assert!(normal.length_percentage().is_none());
        assert_eq!(normal.to_px(CssPixels::from_integer(100)), CssPixels::default());
    }
}
