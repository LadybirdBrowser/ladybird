/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The filter graphs painting hands over as bytes, and the device-pixel filter functions hosts
//! build them from outside the style system.

use libgfx_rust::filter::Filter;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFilterFunctionKind {
    Blur,
    DropShadow,
    Color,
    HueRotate,
}

/// One function of a CSS filter list with its lengths already in device pixels, for a host that
/// builds filter graphs outside the style system: compositor animation samples and canvas filters.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiFilterFunction {
    pub kind: FfiFilterFunctionKind,
    /// The blur radius, drop shadow radius, color operation amount, or hue rotation in degrees.
    pub amount: f32,
    /// Drop shadow only.
    pub offset_x: f32,
    pub offset_y: f32,
    pub color: libgfx_rust::Color,
    /// Color only.
    pub color_operation: libgfx_rust::ColorFilterType,
}

impl From<FfiFilterFunction> for Filter {
    fn from(function: FfiFilterFunction) -> Self {
        match function.kind {
            FfiFilterFunctionKind::Blur => Filter::blur(function.amount, function.amount, None),
            FfiFilterFunctionKind::DropShadow => Filter::drop_shadow(
                function.offset_x,
                function.offset_y,
                function.amount,
                function.color,
                None,
            ),
            FfiFilterFunctionKind::Color => Filter::color(function.color_operation, function.amount, None),
            FfiFilterFunctionKind::HueRotate => Filter::hue_rotate(function.amount, None),
        }
    }
}

/// One graph applying each function to the output of the one before it, so the last function in
/// the list ends up outermost; `None` for an empty list.
pub fn filter_functions_graph(functions: impl IntoIterator<Item = Filter>) -> Option<Filter> {
    functions.into_iter().fold(None, |inner, outer| {
        Some(match inner {
            Some(inner) => Filter::compose(outer, inner),
            None => outer,
        })
    })
}

/// Whether a serialized filter can change the painted bounds of what it is applied to.
pub fn may_affect_output_bounds(bytes: &[u8]) -> bool {
    Filter::serialized_may_affect_output_bounds(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::{Color, ColorFilterType};

    #[test]
    fn device_pixel_functions_lower_the_same_way() {
        let drop_shadow = FfiFilterFunction {
            kind: FfiFilterFunctionKind::DropShadow,
            amount: 3.0,
            offset_x: 1.0,
            offset_y: 2.0,
            color: Color(0x7f00ff00),
            color_operation: ColorFilterType::Brightness,
        };
        let invert = FfiFilterFunction {
            kind: FfiFilterFunctionKind::Color,
            amount: 1.0,
            offset_x: 0.0,
            offset_y: 0.0,
            color: Color::TRANSPARENT,
            color_operation: ColorFilterType::Invert,
        };
        assert_eq!(filter_functions_graph([]), None);
        assert_eq!(
            filter_functions_graph([drop_shadow, invert].map(Filter::from)),
            Some(Filter::compose(
                Filter::color(ColorFilterType::Invert, 1.0, None),
                Filter::drop_shadow(1.0, 2.0, 3.0, Color(0x7f00ff00), None)
            ))
        );
    }

    #[test]
    fn unreadable_bytes_are_assumed_to_affect_bounds() {
        assert!(may_affect_output_bounds(&[]));
        assert!(may_affect_output_bounds(&[0xff]));
        assert!(!may_affect_output_bounds(&Filter::hue_rotate(90.0, None).serialize()));
    }
}
