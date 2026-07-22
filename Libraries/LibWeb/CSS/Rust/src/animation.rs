/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS animation value interpolation.

// Animation values use the same thread-confined shared graph as the rest of the style core.
#![allow(clippy::arc_with_non_send_sync)]

use std::sync::Arc;

use crate::property_metadata::{property_animation_type, property_numeric_ranges};
use crate::style_value::{RetainedStyleValueData, StyleValueData};

const ANIMATION_TYPE_BY_COMPUTED_VALUE: u8 = 1;
const VALUE_TYPE_ANGLE: u8 = 2;
const VALUE_TYPE_FLEX: u8 = 15;
const VALUE_TYPE_INTEGER: u8 = 24;
const VALUE_TYPE_LENGTH: u8 = 25;
const VALUE_TYPE_NUMBER: u8 = 27;
const VALUE_TYPE_PERCENTAGE: u8 = 31;
const VALUE_TYPE_RATIO: u8 = 33;

#[repr(C)]
pub struct FfiAnimationValueResult {
    pub value: *const StyleValueData,
    pub handled: bool,
}

fn accepted_range(property_id: u16, value_type: u8) -> Option<(f64, f64)> {
    property_numeric_ranges(property_id)
        .iter()
        .find(|range| range.value_type == value_type)
        .map(|range| (range.min, range.max))
}

fn clamp_to_range(value: f64, range: Option<(f64, f64)>) -> f64 {
    let Some((min, max)) = range else {
        return value;
    };
    if value < min {
        min
    } else if value > max {
        max
    } else {
        value
    }
}

fn interpolate_f64(from: f64, to: f64, delta: f32, range: Option<(f64, f64)>) -> f64 {
    clamp_to_range(from + (to - from) * f64::from(delta), range)
}

fn interpolate_i32(from: i32, to: i32, delta: f32, range: Option<(f64, f64)>) -> i32 {
    // https://drafts.csswg.org/css-values/#combine-integers
    // Interpolation of <integer> is defined as Vresult = round((1 - p) × VA + p × VB);
    // that is, interpolation happens in the real number space as for <number>s, and the result is converted to an <integer> by rounding to the nearest integer.
    let value = (from as f32 + (to as f32 - from as f32) * delta).round();
    clamp_to_range(f64::from(value), range) as i32
}

fn angle_to_degrees(value: f64, unit: u8) -> Option<f64> {
    let ratio = match unit {
        0 => 1.0,
        1 => 0.9,
        2 => 57.295_779_513_082_32,
        3 => 360.0,
        _ => return None,
    };
    Some(value * ratio)
}

fn owned(value: StyleValueData) -> FfiAnimationValueResult {
    FfiAnimationValueResult {
        value: Arc::into_raw(Arc::new(value)),
        handled: true,
    }
}

fn not_handled() -> FfiAnimationValueResult {
    FfiAnimationValueResult {
        value: std::ptr::null(),
        handled: false,
    }
}

fn interpolate_scalar(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    if property_animation_type(property_id) != ANIMATION_TYPE_BY_COMPUTED_VALUE {
        return not_handled();
    }

    match (from, to) {
        (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) => {
            owned(StyleValueData::Number {
                value: interpolate_f64(*from, *to, delta, accepted_range(property_id, VALUE_TYPE_NUMBER)),
            })
        }
        (StyleValueData::Integer { value: from }, StyleValueData::Integer { value: to }) => {
            owned(StyleValueData::Integer {
                value: interpolate_i32(*from, *to, delta, accepted_range(property_id, VALUE_TYPE_INTEGER)),
            })
        }
        (
            StyleValueData::Angle {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Angle {
                value: to,
                unit: to_unit,
            },
        ) => {
            let (Some(from), Some(to)) = (angle_to_degrees(*from, *from_unit), angle_to_degrees(*to, *to_unit)) else {
                return not_handled();
            };
            owned(StyleValueData::Angle {
                value: interpolate_f64(from, to, delta, accepted_range(property_id, VALUE_TYPE_ANGLE)),
                unit: 0,
            })
        }
        (
            StyleValueData::Flex {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Flex {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Flex {
            value: interpolate_f64(*from, *to, delta, accepted_range(property_id, VALUE_TYPE_FLEX)),
            unit: *from_unit,
        }),
        (
            StyleValueData::Length {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Length { value: to, .. },
        ) => owned(StyleValueData::Length {
            value: interpolate_f64(*from, *to, delta, accepted_range(property_id, VALUE_TYPE_LENGTH)),
            unit: *from_unit,
        }),
        (StyleValueData::Percentage { value: from }, StyleValueData::Percentage { value: to }) => {
            owned(StyleValueData::Percentage {
                value: interpolate_f64(*from, *to, delta, accepted_range(property_id, VALUE_TYPE_PERCENTAGE)),
            })
        }
        (StyleValueData::OpacityValue { value: from }, StyleValueData::OpacityValue { value: to }) => {
            let (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) =
                (from.data(), to.data())
            else {
                return not_handled();
            };
            let number = Arc::into_raw(Arc::new(StyleValueData::Number {
                value: interpolate_f64(*from, *to, delta, Some((0.0, 1.0))),
            }));
            owned(StyleValueData::OpacityValue {
                value: unsafe { RetainedStyleValueData::from_retained_pointer(number) },
            })
        }
        (
            StyleValueData::Ratio {
                numerator: from_numerator,
                denominator: from_denominator,
            },
            StyleValueData::Ratio {
                numerator: to_numerator,
                denominator: to_denominator,
            },
        ) => {
            let (
                StyleValueData::Number { value: from_numerator },
                StyleValueData::Number {
                    value: from_denominator,
                },
                StyleValueData::Number { value: to_numerator },
                StyleValueData::Number { value: to_denominator },
            ) = (
                from_numerator.data(),
                from_denominator.data(),
                to_numerator.data(),
                to_denominator.data(),
            )
            else {
                return not_handled();
            };

            // https://drafts.csswg.org/css-values/#combine-ratio
            // If either <ratio> is degenerate, the values cannot be interpolated.
            if !from_numerator.is_finite()
                || *from_numerator == 0.0
                || !from_denominator.is_finite()
                || *from_denominator == 0.0
                || !to_numerator.is_finite()
                || *to_numerator == 0.0
                || !to_denominator.is_finite()
                || *to_denominator == 0.0
            {
                return not_handled();
            }

            // The interpolation of a <ratio> is defined by converting each <ratio> to a number by dividing the first value
            // by the second (so a ratio of 3 / 2 would become 1.5), taking the logarithm of that result (so the 1.5 would
            // become approximately 0.176), then interpolating those values. The result during the interpolation is
            // converted back to a <ratio> by inverting the logarithm, then interpreting the result as a <ratio> with the
            // result as the first value and 1 as the second value.
            let from_number = (from_numerator / from_denominator).ln();
            let to_number = (to_numerator / to_denominator).ln();
            let value = interpolate_f64(
                from_number,
                to_number,
                delta,
                accepted_range(property_id, VALUE_TYPE_RATIO),
            )
            .exp();
            let numerator = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
            let denominator = Arc::into_raw(Arc::new(StyleValueData::Number { value: 1.0 }));
            owned(StyleValueData::Ratio {
                numerator: unsafe { RetainedStyleValueData::from_retained_pointer(numerator) },
                denominator: unsafe { RetainedStyleValueData::from_retained_pointer(denominator) },
            })
        }
        _ => not_handled(),
    }
}

/// Attempt scalar by-computed-value interpolation without consulting C++ or the DOM.
///
/// # Safety
/// `from` and `to` must point at live `StyleValueData` allocations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_interpolate_scalar_style_value(
    property_id: u16,
    from: *const StyleValueData,
    to: *const StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    crate::abort_on_panic(|| interpolate_scalar(property_id, unsafe { &*from }, unsafe { &*to }, delta))
}
