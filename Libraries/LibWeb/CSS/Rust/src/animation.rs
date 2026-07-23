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
use crate::style_value::{
    RetainedNumericRangeList, RetainedStyleValueData, RetainedStyleValueDataList, RetainedUtf16FlyString,
    StyleValueData,
};

const ANIMATION_TYPE_BY_COMPUTED_VALUE: u8 = 1;
const ANIMATION_TYPE_REPEATABLE_LIST: u8 = 2;
const ANIMATION_TYPE_CUSTOM: u8 = 3;
const VALUE_TYPE_ANGLE: u8 = 2;
const VALUE_TYPE_FLEX: u8 = 15;
const VALUE_TYPE_FREQUENCY: u8 = 21;
const VALUE_TYPE_INTEGER: u8 = 24;
const VALUE_TYPE_LENGTH: u8 = 25;
const VALUE_TYPE_NUMBER: u8 = 27;
const VALUE_TYPE_PERCENTAGE: u8 = 31;
const VALUE_TYPE_RATIO: u8 = 33;
const VALUE_TYPE_RESOLUTION: u8 = 35;
const VALUE_TYPE_TIME: u8 = 38;
const TRANSFORM_FUNCTION_MATRIX: u8 = 0;
const TRANSFORM_FUNCTION_MATRIX_3D: u8 = 1;
const TRANSFORM_FUNCTION_PERSPECTIVE: u8 = 2;
const TRANSFORM_FUNCTION_TRANSLATE: u8 = 3;
const TRANSFORM_FUNCTION_TRANSLATE_3D: u8 = 4;
const TRANSFORM_FUNCTION_TRANSLATE_X: u8 = 5;
const TRANSFORM_FUNCTION_TRANSLATE_Y: u8 = 6;
const TRANSFORM_FUNCTION_TRANSLATE_Z: u8 = 7;
const TRANSFORM_FUNCTION_SCALE: u8 = 8;
const TRANSFORM_FUNCTION_SCALE_3D: u8 = 9;
const TRANSFORM_FUNCTION_SCALE_X: u8 = 10;
const TRANSFORM_FUNCTION_SCALE_Y: u8 = 11;
const TRANSFORM_FUNCTION_SCALE_Z: u8 = 12;
const TRANSFORM_FUNCTION_ROTATE: u8 = 13;
const TRANSFORM_FUNCTION_ROTATE_3D: u8 = 14;
const TRANSFORM_FUNCTION_ROTATE_X: u8 = 15;
const TRANSFORM_FUNCTION_ROTATE_Y: u8 = 16;
const TRANSFORM_FUNCTION_ROTATE_Z: u8 = 17;
const TRANSFORM_FUNCTION_SKEW: u8 = 18;
const TRANSFORM_FUNCTION_SKEW_X: u8 = 19;
const TRANSFORM_FUNCTION_SKEW_Y: u8 = 20;
const OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS: u8 = 1;
const FONT_STYLE_NORMAL: u8 = 0;
const FONT_STYLE_OBLIQUE: u8 = 4;

#[derive(Clone, Copy)]
struct NumericRangeOverride {
    value_type: u8,
    min: f64,
    max: f64,
}

const BORDER_RADIUS_RECT_RANGES: &[NumericRangeOverride] = &[
    NumericRangeOverride {
        value_type: VALUE_TYPE_LENGTH,
        min: 0.0,
        max: f32::MAX as f64,
    },
    NumericRangeOverride {
        value_type: VALUE_TYPE_PERCENTAGE,
        min: 0.0,
        max: f32::MAX as f64,
    },
];

#[repr(C)]
pub struct FfiAnimationValueResult {
    pub value: *const StyleValueData,
    pub handled: bool,
}

#[repr(C)]
pub struct FfiAnimationContext {
    pub allow_discrete: bool,
    pub has_transform_reference_box: bool,
    pub transform_reference_box_width: f64,
    pub transform_reference_box_height: f64,
}

#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiCompositeOperation {
    Replace,
    Add,
    Accumulate,
}

fn accepted_range(property_id: u16, value_type: u8, range_overrides: &[NumericRangeOverride]) -> Option<(f64, f64)> {
    if let Some(range) = range_overrides.iter().find(|range| range.value_type == value_type) {
        return Some((range.min, range.max));
    }
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

// https://drafts.csswg.org/css-borders-4/#normalized-superellipse-half-corner
fn normalized_super_ellipse_half_corner(s: f64) -> f64 {
    //  To compute the normalized superellipse half corner given a superellipse parameter s, return the first matching statement, switching on s:

    // -∞ Return 0.
    if s == f64::NEG_INFINITY {
        return 0.0;
    }

    // ∞ Return 1.
    if s == f64::INFINITY {
        return 1.0;
    }

    // Otherwise
    // 1. Let k be 0.5^abs(s).
    let k = 0.5_f64.powf(s.abs());

    // 2. Let convexHalfCorner be 0.5^k.
    let convex_half_corner = 0.5_f64.powf(k);

    // 3. If s is less than 0, return 1 - convexHalfCorner.
    if s < 0.0 {
        return 1.0 - convex_half_corner;
    }

    // 4. Return convexHalfCorner.
    convex_half_corner
}

fn interpolation_value_to_super_ellipse_parameter(interpolation_value: f64) -> f64 {
    // To convert a <number [0,1]> interpolationValue back to a superellipse parameter, switch on interpolationValue:

    // 0 Return -∞.
    if interpolation_value == 0.0 {
        return f64::NEG_INFINITY;
    }

    // 0.5 Return 0.
    if interpolation_value == 0.5 {
        return 0.0;
    }

    // 1 Return ∞.
    if interpolation_value == 1.0 {
        return f64::INFINITY;
    }

    // Otherwise
    // 1. Let convexHalfCorner be interpolationValue.
    let mut convex_half_corner = interpolation_value;

    // 2. If interpolationValue is less than 0.5, set convexHalfCorner to 1 - interpolationValue.
    if interpolation_value < 0.5 {
        convex_half_corner = 1.0 - interpolation_value;
    }

    // 3. Let k be ln(0.5) / ln(convexHalfCorner).
    let k = 0.5_f64.ln() / convex_half_corner.ln();

    // 4. Let s be log2(k).
    let mut s = k.log2();

    // AD-HOC: The logs above can introduce slight inaccuracies, this can interfere with the behaviour of
    //         serializing superellipse style values as their equivalent keywords as that relies on exact
    //         equality. To mitigate this we simply round to a whole number if we are sufficiently near
    if (s.round() - s).abs() < f64::from(f32::EPSILON) {
        s = s.round();
    }

    // 5. If interpolationValue is less than 0.5, return -s.
    if interpolation_value < 0.5 {
        return -s;
    }

    // 6. Return s.
    s
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

fn handled_without_value() -> FfiAnimationValueResult {
    FfiAnimationValueResult {
        value: std::ptr::null(),
        handled: true,
    }
}

fn discrete_value(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    if !context.is_some_and(|context| context.allow_discrete) {
        return handled_without_value();
    }
    let value = if delta < 0.5 { from } else { to };
    FfiAnimationValueResult {
        value: unsafe { crate::style_value::rust_style_value_retain(value) },
        handled: true,
    }
}

fn interpolate_visibility(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let (StyleValueData::Keyword { keyword: from_keyword }, StyleValueData::Keyword { keyword: to_keyword }) =
        (from, to)
    else {
        return not_handled();
    };

    if from_keyword == to_keyword {
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/web-animations-1/#animating-visibility
    // For the visibility property, visible is interpolated as a discrete step where values of p between 0 and 1 map to visible and other values of p map to the closer endpoint.
    // If neither value is visible, then discrete animation is used.
    let visible = crate::style_compute::keyword::VISIBLE;
    if *from_keyword == visible || *to_keyword == visible {
        let value = if delta <= 0.0 {
            from
        } else if delta >= 1.0 {
            to
        } else if *from_keyword == visible {
            from
        } else {
            to
        };
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(value) },
            handled: true,
        };
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_content_visibility(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let (StyleValueData::Keyword { keyword: from_keyword }, StyleValueData::Keyword { keyword: to_keyword }) =
        (from, to)
    else {
        return not_handled();
    };

    if from_keyword == to_keyword {
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-contain/#content-visibility-animation
    // In general, the content-visibility property’s animation type is discrete.
    // However, similar to interpolation of visibility, during interpolation between hidden and any other content-visibility value,
    // p values between 0 and 1 map to the non-hidden value.
    let hidden = crate::style_compute::keyword::HIDDEN;
    if *from_keyword == hidden || *to_keyword == hidden {
        if !context.is_some_and(|context| context.allow_discrete) {
            return handled_without_value();
        }
        let value = if delta <= 0.0 {
            from
        } else if delta >= 1.0 || *from_keyword == hidden {
            to
        } else {
            from
        };
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(value) },
            handled: true,
        };
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_display(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let (StyleValueData::Display { raw: from_raw }, StyleValueData::Display { raw: to_raw }) = (from, to) else {
        return not_handled();
    };

    if from_raw == to_raw {
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-display-4/#display-animation
    // In general, the display property’s animation type is discrete. However, similar to interpolation of
    // visibility (see Web Animations §  Animation of visibility), during interpolation between none and any
    // other display value, p values between 0 and 1 map to the non-none value. Additionally, the element is
    // inert as long as its display value would compute to none when ignoring the Transitions and Animations
    // cascade origins.
    // FIXME: Implement the inertness portion of this.
    let from_is_none = crate::style_compute::display_is_none(*from_raw);
    let to_is_none = crate::style_compute::display_is_none(*to_raw);
    if from_is_none || to_is_none {
        if !context.is_some_and(|context| context.allow_discrete) {
            return handled_without_value();
        }
        let value = if delta <= 0.0 {
            from
        } else if delta >= 1.0 || from_is_none {
            to
        } else {
            from
        };
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(value) },
            handled: true,
        };
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_scale(from: &StyleValueData, to: &StyleValueData, delta: f32) -> FfiAnimationValueResult {
    let none = crate::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-transforms-2/#propdef-scale
    // Animation type: by computed value, but see below for none
    // The scale property accepts 1-3 values, each specifying a scale along one axis, in order X, Y, then Z.
    // If the Y value is not given, then it defaults to being the same as the X value.
    // If the Z value is not given, then it defaults to 1.
    // A <percentage> is equivalent to a <number>, for example scale: 100% is equivalent to scale: 1. Numbers are used
    // during serialization of specified and computed values.
    // When translate, rotate or scale are animating or transitioning, and the from value or to value (but not both) is
    // none, the value none is replaced by the equivalent identity value (0px for translate, 0deg for rotate, 1 for scale).
    let decode = |value: &StyleValueData| {
        if matches!(value, StyleValueData::Keyword { keyword } if *keyword == none) {
            return Some(vec![1.0, 1.0]);
        }
        let StyleValueData::Transformation { values, .. } = value else {
            return None;
        };
        if !matches!(values.as_slice().len(), 2 | 3) {
            return None;
        }
        values
            .as_slice()
            .iter()
            .map(|value| match value.data() {
                StyleValueData::Number { value } => Some(*value),
                StyleValueData::Percentage { value } => Some(*value / 100.0),
                calculated @ StyleValueData::Calculated { .. } => {
                    crate::calc::resolve_calculated_number_without_context(calculated).or_else(|| {
                        crate::calc::resolve_calculated_percentage_without_context(calculated)
                            .map(|value| value / 100.0)
                    })
                }
                _ => None,
            })
            .collect::<Option<Vec<_>>>()
    };
    let (Some(mut from), Some(mut to)) = (decode(from), decode(to)) else {
        return not_handled();
    };
    let is_3d = from.len() == 3 || to.len() == 3;
    if is_3d {
        from.resize(3, 1.0);
        to.resize(3, 1.0);
    }
    let values = from
        .into_iter()
        .zip(to)
        .map(|(from, to)| retained_number(interpolate_f64(from, to, delta, None)))
        .collect();
    owned(StyleValueData::Transformation {
        property: crate::property_metadata::property_id::SCALE,
        transform_function: if is_3d {
            TRANSFORM_FUNCTION_SCALE_3D
        } else {
            TRANSFORM_FUNCTION_SCALE
        },
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn length_percentage_calculation_node(value: &StyleValueData) -> Option<Arc<crate::calc::CalcNode>> {
    match value {
        StyleValueData::Length { value, unit } => Some(Arc::new(crate::calc::CalcNode::Numeric(
            crate::calc::CalcNumericValue::Length {
                value: *value,
                unit: *unit,
            },
        ))),
        StyleValueData::Percentage { value } => Some(Arc::new(crate::calc::CalcNode::Numeric(
            crate::calc::CalcNumericValue::Percentage(*value),
        ))),
        StyleValueData::Calculated { rust_calculation, .. } => Some(rust_calculation.node_arc()),
        _ => None,
    }
}

fn interpolate_translate_component(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<RetainedStyleValueData> {
    let direct = interpolate_scalar_value(property_id, from, to, delta, &[]);
    if direct.handled && !direct.value.is_null() {
        return Some(unsafe { RetainedStyleValueData::from_retained_pointer(direct.value) });
    }

    // https://drafts.csswg.org/css-values-4/#combine-mixed
    // The computed value of a percentage-dimension mix is defined as
    // a computed percentage if the dimension component is zero
    let dimension_component_is_zero = match (from, to) {
        (StyleValueData::Length { value, .. }, StyleValueData::Percentage { .. }) => {
            *value * (1.0 - delta as f64) == 0.0
        }
        (StyleValueData::Percentage { .. }, StyleValueData::Length { value, .. }) => *value * delta as f64 == 0.0,
        _ => false,
    };
    if dimension_component_is_zero {
        let percentage = match (from, to) {
            (StyleValueData::Length { .. }, StyleValueData::Percentage { value }) => *value * delta as f64,
            (StyleValueData::Percentage { value }, StyleValueData::Length { .. }) => *value * (1.0 - delta as f64),
            _ => unreachable!(),
        };
        let value = Arc::into_raw(Arc::new(StyleValueData::Percentage { value: percentage }));
        return Some(unsafe { RetainedStyleValueData::from_retained_pointer(value) });
    }

    let from = length_percentage_calculation_node(from)?;
    let to = length_percentage_calculation_node(to)?;
    let (calculation, resolved_type) = crate::calc::interpolate_length_percentage_calculations(from, to, delta)?;
    let value = match &*calculation {
        crate::calc::CalcNode::Numeric(crate::calc::CalcNumericValue::Length { value, unit }) => {
            StyleValueData::Length {
                value: *value,
                unit: *unit,
            }
        }
        crate::calc::CalcNode::Numeric(crate::calc::CalcNumericValue::Percentage(value)) => {
            StyleValueData::Percentage { value: *value }
        }
        _ => StyleValueData::Calculated {
            rust_calculation: crate::calc::CalcNodeHandle::from_arc(calculation),
            resolve_as_is_number: false,
            resolve_as_base: 0,
            resolved_type,
            has_percentages_resolve_as: true,
            percentages_resolve_as: VALUE_TYPE_LENGTH,
            resolve_numbers_as_integers: false,
            accepted_ranges: RetainedNumericRangeList::empty(),
        },
    };
    let value = Arc::into_raw(Arc::new(value));
    Some(unsafe { RetainedStyleValueData::from_retained_pointer(value) })
}

fn interpolate_translate(from: &StyleValueData, to: &StyleValueData, delta: f32) -> FfiAnimationValueResult {
    let none = crate::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-transforms-2/#propdef-translate
    // Animation type: by computed value, but see below for none
    // The translate property accepts 1-3 values, each specifying a translation against one axis, in the order X, Y,
    // then Z. When the second or third values are missing, they default to 0px.
    // If the third value is omitted or zero, this specifies a 2d translation, equivalent to the translate() function.
    // Otherwise, this specifies a 3d translation, equivalent to the translate3d() function.
    // When translate, rotate or scale are animating or transitioning, and the from value or to value (but not both) is
    // none, the value none is replaced by the equivalent identity value (0px for translate, 0deg for rotate, 1 for scale).
    let decode = |value: &StyleValueData| {
        let zero = || {
            let zero = Arc::into_raw(Arc::new(StyleValueData::Length {
                value: 0.0,
                unit: crate::calc::canonical_pixel_unit(),
            }));
            unsafe { RetainedStyleValueData::from_retained_pointer(zero) }
        };
        if matches!(value, StyleValueData::Keyword { keyword } if *keyword == none) {
            return Some(vec![zero(), zero()]);
        }
        let StyleValueData::Transformation { values, .. } = value else {
            return None;
        };
        if !matches!(values.as_slice().len(), 2 | 3) {
            return None;
        }
        Some(
            values
                .as_slice()
                .iter()
                .map(|value| value.clone_retained())
                .collect::<Vec<_>>(),
        )
    };
    let (Some(mut from), Some(mut to)) = (decode(from), decode(to)) else {
        return not_handled();
    };
    let is_3d = from.len() == 3 || to.len() == 3;
    let zero = || {
        let zero = Arc::into_raw(Arc::new(StyleValueData::Length {
            value: 0.0,
            unit: crate::calc::canonical_pixel_unit(),
        }));
        unsafe { RetainedStyleValueData::from_retained_pointer(zero) }
    };
    if is_3d {
        from.resize_with(3, zero);
        to.resize_with(3, zero);
    }
    let values = from
        .iter()
        .zip(to.iter())
        .map(|(from, to)| {
            interpolate_translate_component(
                crate::property_metadata::property_id::TRANSLATE,
                from.data(),
                to.data(),
                delta,
            )
        })
        .collect::<Option<Vec<_>>>();
    let Some(values) = values else {
        return not_handled();
    };
    owned(StyleValueData::Transformation {
        property: crate::property_metadata::property_id::TRANSLATE,
        transform_function: if is_3d {
            TRANSFORM_FUNCTION_TRANSLATE_3D
        } else {
            TRANSFORM_FUNCTION_TRANSLATE
        },
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn interpolate_individual_rotate(from: &StyleValueData, to: &StyleValueData, delta: f32) -> FfiAnimationValueResult {
    let none = crate::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return FfiAnimationValueResult {
            value: unsafe { crate::style_value::rust_style_value_retain(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-transforms-2/#propdef-rotate
    // Animation type: as SLERP, but see below for none
    // The rotate property accepts an angle to rotate an element, and optionally an axis to rotate it around.
    // When translate, rotate or scale are animating or transitioning, and the from value or to value (but not both) is
    // none, the value none is replaced by the equivalent identity value (0px for translate, 0deg for rotate, 1 for scale).
    let is_none = |value: &StyleValueData| matches!(value, StyleValueData::Keyword { keyword } if *keyword == none);
    let is_2d = |value: &StyleValueData| {
        is_none(value)
            || matches!(value, StyleValueData::Transformation { transform_function, values, .. }
                if *transform_function == TRANSFORM_FUNCTION_ROTATE && values.as_slice().len() == 1)
    };
    if is_2d(from) && is_2d(to) {
        let angle = |value: &StyleValueData| {
            if is_none(value) {
                return Some(0.0);
            }
            let StyleValueData::Transformation { values, .. } = value else {
                return None;
            };
            let [angle] = values.as_slice() else {
                return None;
            };
            match angle.data() {
                StyleValueData::Angle { value, unit } => angle_to_degrees(*value, *unit),
                _ => None,
            }
        };
        let (Some(from), Some(to)) = (angle(from), angle(to)) else {
            return not_handled();
        };
        let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
            value: interpolate_f64(from, to, delta, None),
            unit: 0,
        }));
        return owned(StyleValueData::Transformation {
            property: crate::property_metadata::property_id::ROTATE,
            transform_function: TRANSFORM_FUNCTION_ROTATE,
            values: RetainedStyleValueDataList::from_retained_values(vec![unsafe {
                RetainedStyleValueData::from_retained_pointer(angle)
            }]),
        });
    }

    let normalize = |value: &StyleValueData| {
        if is_none(value) {
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            return Some(RetainedStyleValueDataList::from_retained_values(vec![
                retained_number(0.0),
                retained_number(0.0),
                retained_number(1.0),
                unsafe { RetainedStyleValueData::from_retained_pointer(angle) },
            ]));
        }
        let StyleValueData::Transformation {
            transform_function,
            values,
            ..
        } = value
        else {
            return None;
        };
        match (*transform_function, values.as_slice()) {
            (TRANSFORM_FUNCTION_ROTATE, [angle]) => Some(RetainedStyleValueDataList::from_retained_values(vec![
                retained_number(0.0),
                retained_number(0.0),
                retained_number(1.0),
                angle.clone_retained(),
            ])),
            (TRANSFORM_FUNCTION_ROTATE_3D, [..]) if values.as_slice().len() == 4 => {
                Some(RetainedStyleValueDataList::from_retained_values(
                    values
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                ))
            }
            _ => None,
        }
    };
    let (Some(from), Some(to)) = (normalize(from), normalize(to)) else {
        return not_handled();
    };
    interpolate_rotate_3d(
        crate::property_metadata::property_id::ROTATE,
        TRANSFORM_FUNCTION_ROTATE_3D,
        &from,
        &to,
        delta,
    )
    .map_or_else(not_handled, owned)
}

fn interpolate_font_variation_settings(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    // https://drafts.csswg.org/css-fonts/#font-variation-settings-def
    // Two declarations of font-feature-settings can be animated between if they are "like". "Like" declarations
    // are ones where the same set of properties appear (in any order). Because successive duplicate properties
    // are applied instead of prior duplicate properties, two declarations can be "like" even if they have
    // differing number of properties. If two declarations are "like" then animation occurs pairwise between
    // corresponding values in the declarations. Otherwise, animation is not possible.
    if !matches!(from, StyleValueData::ValueList { .. }) || !matches!(to, StyleValueData::ValueList { .. }) {
        return discrete_value(context, from, to, delta);
    }

    // NB: The values in these lists have already been deduplicated and sorted at this point, so we can
    //     interpolate them pairwise.
    let result = interpolate_scalar_value(property_id, from, to, delta, &[]);
    if !result.handled || result.value.is_null() {
        return discrete_value(context, from, to, delta);
    }
    result
}

fn radius_components_equal(first: &StyleValueData, second: &StyleValueData) -> bool {
    match (first, second) {
        (
            StyleValueData::Length {
                value: first_value,
                unit: first_unit,
            },
            StyleValueData::Length {
                value: second_value,
                unit: second_unit,
            },
        ) => first_value == second_value && first_unit == second_unit,
        (StyleValueData::Percentage { value: first }, StyleValueData::Percentage { value: second }) => first == second,
        _ => false,
    }
}

fn composite_scalar_value(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> FfiAnimationValueResult {
    if matches!(operation, FfiCompositeOperation::Replace) {
        return handled_without_value();
    }

    match (underlying, animated) {
        (StyleValueData::Number { value: underlying }, StyleValueData::Number { value: animated }) => {
            // https://drafts.csswg.org/css-values-4/#combine-numbers
            // Addition of <number> is defined as Vresult = VA + VB.
            owned(StyleValueData::Number {
                value: underlying + animated,
            })
        }
        (StyleValueData::Integer { value: underlying }, StyleValueData::Integer { value: animated }) => {
            // https://drafts.csswg.org/css-values-4/#combine-integers
            // Addition of <integer> is defined as Vresult = VA + VB.
            owned(StyleValueData::Integer {
                value: underlying.saturating_add(*animated),
            })
        }
        (
            StyleValueData::Angle {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Angle {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => {
            // https://drafts.csswg.org/css-values-4/#combine-dimensions
            // Addition of compatible dimensions is defined as Vresult = VA + VB.
            owned(StyleValueData::Angle {
                value: underlying + animated,
                unit: *underlying_unit,
            })
        }
        (
            StyleValueData::Flex {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Flex {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Flex {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (
            StyleValueData::Frequency {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Frequency {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Frequency {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (
            StyleValueData::Length {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Length {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Length {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (StyleValueData::Percentage { value: underlying }, StyleValueData::Percentage { value: animated }) => {
            // https://drafts.csswg.org/css-values-4/#combine-mixed
            // Addition of <percentage> is defined the same as interpolation except by adding each component rather than interpolating it.
            owned(StyleValueData::Percentage {
                value: underlying + animated,
            })
        }
        (
            StyleValueData::Resolution {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Resolution {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Resolution {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (
            StyleValueData::Time {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Time {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Time {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (StyleValueData::OpacityValue { value: underlying }, StyleValueData::OpacityValue { value: animated }) => {
            let (StyleValueData::Number { value: underlying }, StyleValueData::Number { value: animated }) =
                (underlying.data(), animated.data())
            else {
                return not_handled();
            };

            // https://drafts.csswg.org/css-color-4/#propdef-opacity
            // Computed value: specified number, clamped to the range [0,1]
            let number = Arc::into_raw(Arc::new(StyleValueData::Number {
                value: (underlying + animated).clamp(0.0, 1.0),
            }));
            owned(StyleValueData::OpacityValue {
                value: unsafe { RetainedStyleValueData::from_retained_pointer(number) },
            })
        }
        (
            StyleValueData::BackgroundSize {
                size_x: underlying_x,
                size_y: underlying_y,
            },
            StyleValueData::BackgroundSize {
                size_x: animated_x,
                size_y: animated_y,
            },
        ) => {
            let x = composite_scalar_value(underlying_x.data(), animated_x.data(), operation);
            let y = composite_scalar_value(underlying_y.data(), animated_y.data(), operation);
            if !x.handled || !y.handled {
                return not_handled();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::BackgroundSize {
                size_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                size_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (StyleValueData::Edge { offset: underlying, .. }, StyleValueData::Edge { offset: animated, .. }) => {
            let (Some(underlying), Some(animated)) = (underlying.optional_data(), animated.optional_data()) else {
                return not_handled();
            };
            let result = composite_scalar_value(underlying, animated, operation);
            if !result.handled {
                return not_handled();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Edge {
                has_edge: false,
                edge: 0,
                offset: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
            })
        }
        (
            StyleValueData::Position {
                edge_x: underlying_x,
                edge_y: underlying_y,
            },
            StyleValueData::Position {
                edge_x: animated_x,
                edge_y: animated_y,
            },
        ) => {
            let x = composite_scalar_value(underlying_x.data(), animated_x.data(), operation);
            let y = composite_scalar_value(underlying_y.data(), animated_y.data(), operation);
            if !x.handled || !y.handled {
                return not_handled();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Position {
                edge_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                edge_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (
            StyleValueData::Rect {
                top: underlying_top,
                right: underlying_right,
                bottom: underlying_bottom,
                left: underlying_left,
            },
            StyleValueData::Rect {
                top: animated_top,
                right: animated_right,
                bottom: animated_bottom,
                left: animated_left,
            },
        ) => {
            let combine = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(underlying_top, animated_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(underlying_right, animated_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(underlying_bottom, animated_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(underlying_left, animated_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::Rect {
                    top,
                    right,
                    bottom,
                    left,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderRadius {
                horizontal_radius: underlying_horizontal,
                vertical_radius: underlying_vertical,
                ..
            },
            StyleValueData::BorderRadius {
                horizontal_radius: animated_horizontal,
                vertical_radius: animated_vertical,
                ..
            },
        ) => {
            let horizontal =
                composite_scalar_value(underlying_horizontal.data(), animated_horizontal.data(), operation);
            if !horizontal.handled {
                return not_handled();
            }
            if horizontal.value.is_null() {
                return handled_without_value();
            }
            let horizontal = unsafe { RetainedStyleValueData::from_retained_pointer(horizontal.value) };
            let vertical = composite_scalar_value(underlying_vertical.data(), animated_vertical.data(), operation);
            if !vertical.handled {
                return not_handled();
            }
            if vertical.value.is_null() {
                return handled_without_value();
            }
            let vertical = unsafe { RetainedStyleValueData::from_retained_pointer(vertical.value) };
            owned(StyleValueData::BorderRadius {
                is_elliptical: !radius_components_equal(horizontal.data(), vertical.data()),
                horizontal_radius: horizontal,
                vertical_radius: vertical,
            })
        }
        (
            StyleValueData::BorderRadiusRect {
                top_left: underlying_top_left,
                top_right: underlying_top_right,
                bottom_right: underlying_bottom_right,
                bottom_left: underlying_bottom_left,
            },
            StyleValueData::BorderRadiusRect {
                top_left: animated_top_left,
                top_right: animated_top_right,
                bottom_right: animated_bottom_right,
                bottom_left: animated_bottom_left,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            let combine = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top_left) = combine(underlying_top_left, animated_top_left)? else {
                    return Ok(None);
                };
                let Some(top_right) = combine(underlying_top_right, animated_top_right)? else {
                    return Ok(None);
                };
                let Some(bottom_right) = combine(underlying_bottom_right, animated_bottom_right)? else {
                    return Ok(None);
                };
                let Some(bottom_left) = combine(underlying_bottom_left, animated_bottom_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderRadiusRect {
                    top_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderImageSlice {
                top: underlying_top,
                right: underlying_right,
                bottom: underlying_bottom,
                left: underlying_left,
                fill: underlying_fill,
            },
            StyleValueData::BorderImageSlice {
                top: animated_top,
                right: animated_right,
                bottom: animated_bottom,
                left: animated_left,
                fill: animated_fill,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_fill != animated_fill {
                return handled_without_value();
            }
            let combine = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(underlying_top, animated_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(underlying_right, animated_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(underlying_bottom, animated_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(underlying_left, animated_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderImageSlice {
                    top,
                    right,
                    bottom,
                    left,
                    fill: *underlying_fill,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::OpenTypeTagged {
                tag: underlying_tag,
                value: underlying_value,
                ..
            },
            StyleValueData::OpenTypeTagged {
                tag: animated_tag,
                value: animated_value,
                ..
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_tag.raw() != animated_tag.raw() {
                return handled_without_value();
            }
            let value = composite_scalar_value(underlying_value.data(), animated_value.data(), operation);
            if !value.handled {
                return not_handled();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::OpenTypeTagged {
                mode: OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS,
                tag: unsafe { RetainedUtf16FlyString::from_borrowed_raw(underlying_tag.raw()) },
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::Function {
                name: underlying_name,
                value: underlying_value,
            },
            StyleValueData::Function {
                name: animated_name,
                value: animated_value,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_name.raw() != animated_name.raw() {
                return handled_without_value();
            }
            let value = composite_scalar_value(underlying_value.data(), animated_value.data(), operation);
            if !value.handled {
                return not_handled();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Function {
                name: unsafe { RetainedUtf16FlyString::from_borrowed_raw(underlying_name.raw()) },
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::TextIndent {
                length_percentage: underlying,
                hanging: underlying_hanging,
                each_line: underlying_each_line,
            },
            StyleValueData::TextIndent {
                length_percentage: animated,
                hanging: animated_hanging,
                each_line: animated_each_line,
            },
        ) => {
            if underlying_hanging != animated_hanging || underlying_each_line != animated_each_line {
                return handled_without_value();
            }
            let result = composite_scalar_value(underlying.data(), animated.data(), operation);
            if !result.handled {
                return not_handled();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::TextIndent {
                length_percentage: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
                hanging: *underlying_hanging,
                each_line: *underlying_each_line,
            })
        }
        (StyleValueData::Ratio { .. }, StyleValueData::Ratio { .. }) => {
            // https://drafts.csswg.org/css-values-4/#combine-ratio
            // Addition of <ratio>s is not possible.
            handled_without_value()
        }
        (
            StyleValueData::ValueList {
                values: underlying_values,
                separator: underlying_separator,
                ..
            },
            StyleValueData::ValueList {
                values: animated_values,
                separator: animated_separator,
                collapsible,
            },
        ) => {
            if underlying_values.as_slice().len() != animated_values.as_slice().len()
                || underlying_separator != animated_separator
            {
                return not_handled();
            }

            let mut values = Vec::with_capacity(underlying_values.as_slice().len());
            for (underlying, animated) in underlying_values.as_slice().iter().zip(animated_values.as_slice()) {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return not_handled();
                }
                if result.value.is_null() {
                    return handled_without_value();
                }
                values.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
            }
            owned(StyleValueData::ValueList {
                values: RetainedStyleValueDataList::from_retained_values(values),
                separator: *underlying_separator,
                collapsible: *collapsible,
            })
        }
        _ => not_handled(),
    }
}

fn interpolate_scalar_value(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
    range_overrides: &[NumericRangeOverride],
) -> FfiAnimationValueResult {
    match (from, to) {
        (from_value @ StyleValueData::Keyword { keyword: from }, StyleValueData::Keyword { keyword: to })
            if from == to =>
        {
            FfiAnimationValueResult {
                value: unsafe { crate::style_value::rust_style_value_retain(from_value) },
                handled: true,
            }
        }
        (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) => {
            owned(StyleValueData::Number {
                value: interpolate_f64(
                    *from,
                    *to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_NUMBER, range_overrides),
                ),
            })
        }
        (StyleValueData::Integer { value: from }, StyleValueData::Integer { value: to }) => {
            owned(StyleValueData::Integer {
                value: interpolate_i32(
                    *from,
                    *to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_INTEGER, range_overrides),
                ),
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
                value: interpolate_f64(
                    from,
                    to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_ANGLE, range_overrides),
                ),
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
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_FLEX, range_overrides),
            ),
            unit: *from_unit,
        }),
        (
            StyleValueData::Frequency {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Frequency {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Frequency {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_FREQUENCY, range_overrides),
            ),
            unit: *from_unit,
        }),
        (
            StyleValueData::Length {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Length {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Length {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_LENGTH, range_overrides),
            ),
            unit: *from_unit,
        }),
        (StyleValueData::Percentage { value: from }, StyleValueData::Percentage { value: to }) => {
            owned(StyleValueData::Percentage {
                value: interpolate_f64(
                    *from,
                    *to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_PERCENTAGE, range_overrides),
                ),
            })
        }
        (
            StyleValueData::Resolution {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Resolution {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Resolution {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_RESOLUTION, range_overrides),
            ),
            unit: *from_unit,
        }),
        (
            StyleValueData::Time {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Time {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Time {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_TIME, range_overrides),
            ),
            unit: *from_unit,
        }),
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
        (StyleValueData::Superellipse { parameter: from }, StyleValueData::Superellipse { parameter: to }) => {
            let (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) =
                (from.data(), to.data())
            else {
                return not_handled();
            };

            // https://drafts.csswg.org/css-borders-4/#corner-shape-interpolation
            let from_normalized_value = normalized_super_ellipse_half_corner(*from);
            let to_normalized_value = normalized_super_ellipse_half_corner(*to);
            let interpolated_value =
                interpolate_f64(from_normalized_value, to_normalized_value, delta, Some((0.0, 1.0)));
            let parameter = Arc::into_raw(Arc::new(StyleValueData::Number {
                value: interpolation_value_to_super_ellipse_parameter(interpolated_value),
            }));
            owned(StyleValueData::Superellipse {
                parameter: unsafe { RetainedStyleValueData::from_retained_pointer(parameter) },
            })
        }
        (
            StyleValueData::BackgroundSize {
                size_x: from_x,
                size_y: from_y,
            },
            StyleValueData::BackgroundSize {
                size_x: to_x,
                size_y: to_y,
            },
        ) => {
            let x = interpolate_scalar_value(property_id, from_x.data(), to_x.data(), delta, range_overrides);
            let y = interpolate_scalar_value(property_id, from_y.data(), to_y.data(), delta, range_overrides);
            if !x.handled || !y.handled {
                return not_handled();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::BackgroundSize {
                size_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                size_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (StyleValueData::Edge { offset: from, .. }, StyleValueData::Edge { offset: to, .. }) => {
            let (Some(from), Some(to)) = (from.optional_data(), to.optional_data()) else {
                return not_handled();
            };
            let result = interpolate_scalar_value(property_id, from, to, delta, range_overrides);
            if !result.handled {
                return not_handled();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Edge {
                has_edge: false,
                edge: 0,
                offset: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
            })
        }
        (
            StyleValueData::Position {
                edge_x: from_x,
                edge_y: from_y,
            },
            StyleValueData::Position {
                edge_x: to_x,
                edge_y: to_y,
            },
        ) => {
            // https://www.w3.org/TR/css-values-4/#combine-positions
            // FIXME: Interpolation of <position> is defined as the independent interpolation of each component (x, y) normalized as an offset from the top left corner as a <length-percentage>.
            let x = interpolate_scalar_value(property_id, from_x.data(), to_x.data(), delta, range_overrides);
            let y = interpolate_scalar_value(property_id, from_y.data(), to_y.data(), delta, range_overrides);
            if !x.handled || !y.handled {
                return not_handled();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Position {
                edge_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                edge_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (
            StyleValueData::Rect {
                top: from_top,
                right: from_right,
                bottom: from_bottom,
                left: from_left,
            },
            StyleValueData::Rect {
                top: to_top,
                right: to_right,
                bottom: to_bottom,
                left: to_left,
            },
        ) => {
            let combine = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
                let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(from_top, to_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(from_right, to_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(from_bottom, to_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(from_left, to_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::Rect {
                    top,
                    right,
                    bottom,
                    left,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderRadius {
                horizontal_radius: from_horizontal,
                vertical_radius: from_vertical,
                ..
            },
            StyleValueData::BorderRadius {
                horizontal_radius: to_horizontal,
                vertical_radius: to_vertical,
                ..
            },
        ) => {
            let horizontal = interpolate_scalar_value(
                property_id,
                from_horizontal.data(),
                to_horizontal.data(),
                delta,
                range_overrides,
            );
            if !horizontal.handled {
                return not_handled();
            }
            if horizontal.value.is_null() {
                return handled_without_value();
            }
            let horizontal = unsafe { RetainedStyleValueData::from_retained_pointer(horizontal.value) };
            let vertical = interpolate_scalar_value(
                property_id,
                from_vertical.data(),
                to_vertical.data(),
                delta,
                range_overrides,
            );
            if !vertical.handled {
                return not_handled();
            }
            if vertical.value.is_null() {
                return handled_without_value();
            }
            let vertical = unsafe { RetainedStyleValueData::from_retained_pointer(vertical.value) };
            owned(StyleValueData::BorderRadius {
                is_elliptical: !radius_components_equal(horizontal.data(), vertical.data()),
                horizontal_radius: horizontal,
                vertical_radius: vertical,
            })
        }
        (
            StyleValueData::BorderRadiusRect {
                top_left: from_top_left,
                top_right: from_top_right,
                bottom_right: from_bottom_right,
                bottom_left: from_bottom_left,
            },
            StyleValueData::BorderRadiusRect {
                top_left: to_top_left,
                top_right: to_top_right,
                bottom_right: to_bottom_right,
                bottom_left: to_bottom_left,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            let combine = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
                let result =
                    interpolate_scalar_value(property_id, from.data(), to.data(), delta, BORDER_RADIUS_RECT_RANGES);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top_left) = combine(from_top_left, to_top_left)? else {
                    return Ok(None);
                };
                let Some(top_right) = combine(from_top_right, to_top_right)? else {
                    return Ok(None);
                };
                let Some(bottom_right) = combine(from_bottom_right, to_bottom_right)? else {
                    return Ok(None);
                };
                let Some(bottom_left) = combine(from_bottom_left, to_bottom_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderRadiusRect {
                    top_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderImageSlice {
                top: from_top,
                right: from_right,
                bottom: from_bottom,
                left: from_left,
                fill: from_fill,
            },
            StyleValueData::BorderImageSlice {
                top: to_top,
                right: to_right,
                bottom: to_bottom,
                left: to_left,
                fill: to_fill,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_fill != to_fill {
                return handled_without_value();
            }
            let combine = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
                let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(from_top, to_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(from_right, to_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(from_bottom, to_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(from_left, to_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderImageSlice {
                    top,
                    right,
                    bottom,
                    left,
                    fill: *from_fill,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::OpenTypeTagged {
                tag: from_tag,
                value: from_value,
                ..
            },
            StyleValueData::OpenTypeTagged {
                tag: to_tag,
                value: to_value,
                ..
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_tag.raw() != to_tag.raw() {
                return handled_without_value();
            }
            let value =
                interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, range_overrides);
            if !value.handled {
                return not_handled();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::OpenTypeTagged {
                mode: OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS,
                tag: unsafe { RetainedUtf16FlyString::from_borrowed_raw(from_tag.raw()) },
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::Function {
                name: from_name,
                value: from_value,
            },
            StyleValueData::Function {
                name: to_name,
                value: to_value,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_name.raw() != to_name.raw() {
                return handled_without_value();
            }
            let value =
                interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, range_overrides);
            if !value.handled {
                return not_handled();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Function {
                name: unsafe { RetainedUtf16FlyString::from_borrowed_raw(from_name.raw()) },
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::TextIndent {
                length_percentage: from,
                hanging: from_hanging,
                each_line: from_each_line,
            },
            StyleValueData::TextIndent {
                length_percentage: to,
                hanging: to_hanging,
                each_line: to_each_line,
            },
        ) => {
            if from_hanging != to_hanging || from_each_line != to_each_line {
                return handled_without_value();
            }
            let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
            if !result.handled {
                return not_handled();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::TextIndent {
                length_percentage: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
                hanging: *from_hanging,
                each_line: *from_each_line,
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
                return handled_without_value();
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
                accepted_range(property_id, VALUE_TYPE_RATIO, range_overrides),
            )
            .exp();
            let numerator = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
            let denominator = Arc::into_raw(Arc::new(StyleValueData::Number { value: 1.0 }));
            owned(StyleValueData::Ratio {
                numerator: unsafe { RetainedStyleValueData::from_retained_pointer(numerator) },
                denominator: unsafe { RetainedStyleValueData::from_retained_pointer(denominator) },
            })
        }
        (
            StyleValueData::ValueList {
                values: from_values,
                separator,
                collapsible,
            },
            StyleValueData::ValueList { values: to_values, .. },
        ) => {
            // https://www.w3.org/TR/web-animations/#by-computed-value
            // If the number of components or the types of corresponding components do not match,
            // or if any component value uses discrete animation and the two corresponding values do not match,
            // then the property values combine as discrete.
            if from_values.as_slice().len() != to_values.as_slice().len() {
                return not_handled();
            }

            let mut values = Vec::with_capacity(from_values.as_slice().len());
            for (from, to) in from_values.as_slice().iter().zip(to_values.as_slice()) {
                let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
                if !result.handled {
                    return not_handled();
                }
                if result.value.is_null() {
                    return handled_without_value();
                }
                values.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
            }
            owned(StyleValueData::ValueList {
                values: RetainedStyleValueDataList::from_retained_values(values),
                separator: *separator,
                collapsible: *collapsible,
            })
        }
        _ => not_handled(),
    }
}

fn interpolate_rotate_3d(
    property: u16,
    transform_function: u8,
    from_arguments: &RetainedStyleValueDataList,
    to_arguments: &RetainedStyleValueDataList,
    delta: f32,
) -> Option<StyleValueData> {
    let ([from_x, from_y, from_z, from_angle], [to_x, to_y, to_z, to_angle]) =
        (from_arguments.as_slice(), to_arguments.as_slice())
    else {
        return None;
    };
    let (
        StyleValueData::Number { value: from_x },
        StyleValueData::Number { value: from_y },
        StyleValueData::Number { value: from_z },
        StyleValueData::Angle {
            value: from_angle,
            unit: from_angle_unit,
        },
        StyleValueData::Number { value: to_x },
        StyleValueData::Number { value: to_y },
        StyleValueData::Number { value: to_z },
        StyleValueData::Angle {
            value: to_angle,
            unit: to_angle_unit,
        },
    ) = (
        from_x.data(),
        from_y.data(),
        from_z.data(),
        from_angle.data(),
        to_x.data(),
        to_y.data(),
        to_z.data(),
        to_angle.data(),
    )
    else {
        return None;
    };
    let from_angle = angle_to_degrees(*from_angle, *from_angle_unit)?.to_radians();
    let to_angle = angle_to_degrees(*to_angle, *to_angle_unit)?.to_radians();
    let from_axis = [*from_x, *from_y, *from_z];
    let to_axis = [*to_x, *to_y, *to_z];

    let length = |vector: [f64; 3]| vector.iter().map(|component| component * component).sum::<f64>().sqrt();
    let normalize = |vector: [f64; 3]| {
        let length = length(vector);
        [vector[0] / length, vector[1] / length, vector[2] / length]
    };
    let epsilon = 1e-5;
    let from_axis_normalized = if length(from_axis) > epsilon {
        normalize(from_axis)
    } else {
        [0.0, 0.0, 1.0]
    };
    let to_axis_normalized = if length(to_axis) > epsilon {
        normalize(to_axis)
    } else {
        [0.0, 0.0, 1.0]
    };
    let axis_difference = [
        from_axis_normalized[0] - to_axis_normalized[0],
        from_axis_normalized[1] - to_axis_normalized[1],
        from_axis_normalized[2] - to_axis_normalized[2],
    ];

    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
    // For interpolations with the primitive rotate3d(), the direction vectors of the transform functions get
    // normalized first. If the normalized vectors are not equal and both rotation angles are non-zero the
    // transform functions get converted into 4x4 matrices first and interpolated as defined in section
    // Interpolation of Matrices afterwards. Otherwise the rotation angle gets interpolated numerically and the
    // rotation vector of the non-zero angle is used or (0, 0, 1) if both angles are zero.
    let (result_axis, result_angle) = if length(axis_difference) < epsilon || from_angle == 0.0 || to_angle == 0.0 {
        let result_axis = if to_angle != 0.0 {
            to_axis_normalized
        } else if from_angle != 0.0 {
            from_axis_normalized
        } else {
            [0.0, 0.0, 1.0]
        };
        (result_axis, interpolate_f64(from_angle, to_angle, delta, None))
    } else {
        let to_quaternion = |axis: [f64; 3], angle: f64| {
            let half_angle = angle / 2.0;
            let sin_half_angle = half_angle.sin();
            [
                axis[0] * sin_half_angle,
                axis[1] * sin_half_angle,
                axis[2] * sin_half_angle,
                half_angle.cos(),
            ]
        };
        let from_quaternion = to_quaternion(from_axis_normalized, from_angle);
        let to_quaternion = to_quaternion(to_axis_normalized, to_angle);

        // https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
        let product = from_quaternion
            .iter()
            .zip(to_quaternion)
            .map(|(from, to)| from * to)
            .sum::<f64>()
            .clamp(-1.0, 1.0);
        let interpolated_quaternion = if product.abs() >= 1.0 {
            from_quaternion
        } else {
            let theta = product.acos();
            let weight = (f64::from(delta) * theta).sin() / (1.0 - product * product).sqrt();
            let from_multiplier = (f64::from(delta) * theta).cos() - product * weight;
            if weight.abs() < f64::from(f32::EPSILON) {
                from_quaternion.map(|component| component * from_multiplier)
            } else if from_multiplier.abs() < f64::from(f32::EPSILON) {
                to_quaternion.map(|component| component * weight)
            } else {
                std::array::from_fn(|index| from_quaternion[index] * from_multiplier + to_quaternion[index] * weight)
            }
        };

        let mut axis = [
            interpolated_quaternion[0],
            interpolated_quaternion[1],
            interpolated_quaternion[2],
        ];
        let sin_half_angle = (1.0 - interpolated_quaternion[3] * interpolated_quaternion[3])
            .max(0.0)
            .sqrt();
        let angle = 2.0 * interpolated_quaternion[3].clamp(-1.0, 1.0).acos();
        if sin_half_angle >= epsilon {
            axis = axis.map(|component| component / sin_half_angle);
        }
        (axis, angle)
    };

    let mut arguments = Vec::with_capacity(4);
    for value in result_axis {
        let argument = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
        arguments.push(unsafe { RetainedStyleValueData::from_retained_pointer(argument) });
    }
    let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
        value: result_angle.to_degrees(),
        unit: 0,
    }));
    arguments.push(unsafe { RetainedStyleValueData::from_retained_pointer(angle) });

    Some(StyleValueData::Transformation {
        property,
        transform_function,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    })
}

fn retained_number(value: f64) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_zero_px() -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Length {
        value: 0.0,
        unit: crate::style_compute::px_length_unit(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_none_keyword() -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Keyword {
        keyword: crate::style_compute::none_keyword(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn is_2d_transform(function: u8) -> bool {
    matches!(
        function,
        TRANSFORM_FUNCTION_ROTATE
            | TRANSFORM_FUNCTION_SCALE
            | TRANSFORM_FUNCTION_SCALE_X
            | TRANSFORM_FUNCTION_SCALE_Y
            | TRANSFORM_FUNCTION_TRANSLATE
            | TRANSFORM_FUNCTION_TRANSLATE_X
            | TRANSFORM_FUNCTION_TRANSLATE_Y
    )
}

fn is_3d_primitive(function: u8) -> bool {
    matches!(
        function,
        TRANSFORM_FUNCTION_ROTATE_3D | TRANSFORM_FUNCTION_SCALE_3D | TRANSFORM_FUNCTION_TRANSLATE_3D
    )
}

fn is_3d_transform(function: u8) -> bool {
    is_2d_transform(function)
        || is_3d_primitive(function)
        || matches!(
            function,
            TRANSFORM_FUNCTION_ROTATE_X
                | TRANSFORM_FUNCTION_ROTATE_Y
                | TRANSFORM_FUNCTION_ROTATE_Z
                | TRANSFORM_FUNCTION_SCALE_Z
                | TRANSFORM_FUNCTION_TRANSLATE_Z
        )
}

fn convert_2d_transform_to_primitive(
    function: u8,
    arguments: &RetainedStyleValueDataList,
) -> Option<(u8, Vec<RetainedStyleValueData>)> {
    let arguments = arguments.as_slice();
    match (function, arguments) {
        (TRANSFORM_FUNCTION_SCALE, [x]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![x.clone_retained(), x.clone_retained()]))
        }
        (TRANSFORM_FUNCTION_SCALE, [x, y]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![x.clone_retained(), y.clone_retained()]))
        }
        (TRANSFORM_FUNCTION_SCALE_X, [x]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![x.clone_retained(), retained_number(1.0)]))
        }
        (TRANSFORM_FUNCTION_SCALE_Y, [y]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![retained_number(1.0), y.clone_retained()]))
        }
        (TRANSFORM_FUNCTION_ROTATE, [angle]) => Some((TRANSFORM_FUNCTION_ROTATE, vec![angle.clone_retained()])),
        (TRANSFORM_FUNCTION_TRANSLATE, [x]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![x.clone_retained(), retained_zero_px()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE, [x, y]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![x.clone_retained(), y.clone_retained()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_X, [x]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![x.clone_retained(), retained_zero_px()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Y, [y]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![retained_zero_px(), y.clone_retained()],
        )),
        _ => None,
    }
}

// https://drafts.csswg.org/css-transforms-1/#transform-primitives
// https://drafts.csswg.org/css-transforms-2/#transform-primitives
fn convert_3d_transform_to_primitive(
    function: u8,
    arguments: &RetainedStyleValueDataList,
) -> Option<(u8, Vec<RetainedStyleValueData>)> {
    let converted_2d;
    let (function, arguments) = if is_2d_transform(function) {
        converted_2d = convert_2d_transform_to_primitive(function, arguments)?;
        (converted_2d.0, converted_2d.1.as_slice())
    } else {
        (function, arguments.as_slice())
    };

    match (function, arguments) {
        (TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z, [angle]) => Some((
            TRANSFORM_FUNCTION_ROTATE_3D,
            vec![
                retained_number(0.0),
                retained_number(0.0),
                retained_number(1.0),
                angle.clone_retained(),
            ],
        )),
        (TRANSFORM_FUNCTION_ROTATE_X, [angle]) => Some((
            TRANSFORM_FUNCTION_ROTATE_3D,
            vec![
                retained_number(1.0),
                retained_number(0.0),
                retained_number(0.0),
                angle.clone_retained(),
            ],
        )),
        (TRANSFORM_FUNCTION_ROTATE_Y, [angle]) => Some((
            TRANSFORM_FUNCTION_ROTATE_3D,
            vec![
                retained_number(0.0),
                retained_number(1.0),
                retained_number(0.0),
                angle.clone_retained(),
            ],
        )),
        (TRANSFORM_FUNCTION_SCALE, [x, y]) => Some((
            TRANSFORM_FUNCTION_SCALE_3D,
            vec![x.clone_retained(), y.clone_retained(), retained_number(1.0)],
        )),
        (TRANSFORM_FUNCTION_SCALE_Z, [z]) => Some((
            TRANSFORM_FUNCTION_SCALE_3D,
            vec![retained_number(1.0), retained_number(1.0), z.clone_retained()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE, [x, y]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE_3D,
            vec![x.clone_retained(), y.clone_retained(), retained_zero_px()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Z, [z]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE_3D,
            vec![retained_zero_px(), retained_zero_px(), z.clone_retained()],
        )),
        _ => None,
    }
}

fn convert_transform_pair_to_common_primitive(
    from_function: u8,
    from_arguments: &RetainedStyleValueDataList,
    to_function: u8,
    to_arguments: &RetainedStyleValueDataList,
) -> Option<(u8, Vec<RetainedStyleValueData>, Vec<RetainedStyleValueData>)> {
    if matches!(
        (from_function, to_function),
        (
            TRANSFORM_FUNCTION_MATRIX | TRANSFORM_FUNCTION_MATRIX_3D | TRANSFORM_FUNCTION_PERSPECTIVE,
            _,
        ) | (
            _,
            TRANSFORM_FUNCTION_MATRIX | TRANSFORM_FUNCTION_MATRIX_3D | TRANSFORM_FUNCTION_PERSPECTIVE,
        )
    ) {
        return None;
    }
    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
    // If both transform functions share a primitive in the two-dimensional space, both transform functions get
    // converted to the two-dimensional primitive. If one or both transform functions are three-dimensional
    // transform functions, the common three-dimensional primitive is used.
    let (from_function, from_arguments, to_function, to_arguments) =
        if is_2d_transform(from_function) && is_2d_transform(to_function) {
            let (from_function, from_arguments) = convert_2d_transform_to_primitive(from_function, from_arguments)?;
            let (to_function, to_arguments) = convert_2d_transform_to_primitive(to_function, to_arguments)?;
            (from_function, from_arguments, to_function, to_arguments)
        } else if is_3d_transform(from_function) || is_3d_transform(to_function) {
            let (from_function, from_arguments) = if is_3d_primitive(from_function) {
                (
                    from_function,
                    from_arguments
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                )
            } else {
                convert_3d_transform_to_primitive(from_function, from_arguments)?
            };
            let (to_function, to_arguments) = if is_3d_primitive(to_function) {
                (
                    to_function,
                    to_arguments
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                )
            } else {
                convert_3d_transform_to_primitive(to_function, to_arguments)?
            };
            (from_function, from_arguments, to_function, to_arguments)
        } else {
            (
                from_function,
                from_arguments
                    .as_slice()
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
                to_function,
                to_arguments
                    .as_slice()
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
            )
        };
    (from_function == to_function && from_arguments.len() == to_arguments.len()).then_some((
        from_function,
        from_arguments,
        to_arguments,
    ))
}

fn identity_transformation(property: u16, function: u8) -> Option<RetainedStyleValueData> {
    // https://drafts.csswg.org/css-transforms-1/#identity-transform-function
    // A transform function that is equivalent to a identity 4x4 matrix (see Mathematical Description of Transform
    // Functions). Examples for identity transform functions are translate(0), translateX(0), translateY(0), scale(1),
    // scaleX(1), scaleY(1), rotate(0), skew(0, 0), skewX(0), skewY(0) and matrix(1, 0, 0, 1, 0, 0).

    // https://drafts.csswg.org/css-transforms-2/#identity-transform-function
    // In addition to the identity transform function in CSS Transforms, examples for identity transform functions
    // include translate3d(0, 0, 0), translateZ(0), scaleZ(1), rotate3d(1, 1, 1, 0), rotateX(0), rotateY(0), rotateZ(0)
    // and matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1). A special case is perspective: perspective(none).
    // The value of m34 becomes infinitesimal small and the transform function is therefore assumed to be equal to the
    // identity matrix.
    let arguments = match function {
        TRANSFORM_FUNCTION_MATRIX => vec![
            retained_number(1.0),
            retained_number(0.0),
            retained_number(0.0),
            retained_number(1.0),
            retained_number(0.0),
            retained_number(0.0),
        ],
        TRANSFORM_FUNCTION_MATRIX_3D => (0..16)
            .map(|index| retained_number(if index % 5 == 0 { 1.0 } else { 0.0 }))
            .collect(),
        TRANSFORM_FUNCTION_PERSPECTIVE => vec![retained_none_keyword()],
        TRANSFORM_FUNCTION_ROTATE
        | TRANSFORM_FUNCTION_ROTATE_X
        | TRANSFORM_FUNCTION_ROTATE_Y
        | TRANSFORM_FUNCTION_ROTATE_Z
        | TRANSFORM_FUNCTION_SKEW
        | TRANSFORM_FUNCTION_SKEW_X
        | TRANSFORM_FUNCTION_SKEW_Y => {
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            vec![unsafe { RetainedStyleValueData::from_retained_pointer(angle) }]
        }
        TRANSFORM_FUNCTION_ROTATE_3D => vec![
            retained_number(1.0),
            retained_number(1.0),
            retained_number(1.0),
            unsafe {
                RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(StyleValueData::Angle {
                    value: 0.0,
                    unit: 0,
                })))
            },
        ],
        TRANSFORM_FUNCTION_TRANSLATE
        | TRANSFORM_FUNCTION_TRANSLATE_X
        | TRANSFORM_FUNCTION_TRANSLATE_Y
        | TRANSFORM_FUNCTION_TRANSLATE_Z => vec![retained_zero_px()],
        TRANSFORM_FUNCTION_TRANSLATE_3D => vec![retained_zero_px(), retained_zero_px(), retained_zero_px()],
        TRANSFORM_FUNCTION_SCALE
        | TRANSFORM_FUNCTION_SCALE_X
        | TRANSFORM_FUNCTION_SCALE_Y
        | TRANSFORM_FUNCTION_SCALE_Z => vec![retained_number(1.0)],
        TRANSFORM_FUNCTION_SCALE_3D => vec![retained_number(1.0), retained_number(1.0), retained_number(1.0)],
        _ => return None,
    };
    let transformation = Arc::into_raw(Arc::new(StyleValueData::Transformation {
        property,
        transform_function: function,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    }));
    Some(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) })
}

type Matrix4 = [[f64; 4]; 4];

struct DecomposedMatrix {
    translation: [f64; 3],
    scale: [f64; 3],
    skew: [f64; 3],
    rotation: [f64; 4],
    perspective: [f64; 4],
}

fn identity_matrix() -> Matrix4 {
    std::array::from_fn(|row| std::array::from_fn(|column| if row == column { 1.0 } else { 0.0 }))
}

fn multiply_matrices(left: Matrix4, right: Matrix4) -> Matrix4 {
    std::array::from_fn(|row| {
        std::array::from_fn(|column| (0..4).map(|index| left[row][index] * right[index][column]).sum())
    })
}

fn invert_matrix(matrix: Matrix4) -> Option<Matrix4> {
    let mut augmented = [[0.0; 8]; 4];
    for row in 0..4 {
        augmented[row][..4].copy_from_slice(&matrix[row]);
        augmented[row][row + 4] = 1.0;
    }
    for column in 0..4 {
        let pivot_row = (column..4).max_by(|left, right| {
            augmented[*left][column]
                .abs()
                .total_cmp(&augmented[*right][column].abs())
        })?;
        if augmented[pivot_row][column] == 0.0 {
            return None;
        }
        augmented.swap(column, pivot_row);
        let pivot = augmented[column][column];
        for value in &mut augmented[column] {
            *value /= pivot;
        }
        let pivot_values = augmented[column];
        for (row, values) in augmented.iter_mut().enumerate() {
            if row == column {
                continue;
            }
            let factor = values[column];
            for index in 0..8 {
                values[index] -= factor * pivot_values[index];
            }
        }
    }
    Some(std::array::from_fn(|row| {
        std::array::from_fn(|column| augmented[row][column + 4])
    }))
}

fn vector_length(vector: [f64; 3]) -> f64 {
    vector.iter().map(|component| component * component).sum::<f64>().sqrt()
}

fn vector_dot(left: [f64; 3], right: [f64; 3]) -> f64 {
    left.iter().zip(right).map(|(left, right)| left * right).sum()
}

fn vector_cross(left: [f64; 3], right: [f64; 3]) -> [f64; 3] {
    [
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    ]
}

// https://drafts.csswg.org/css-transforms-1/#supporting-functions
fn combine_vectors(left: [f64; 3], right: [f64; 3], left_scale: f64, right_scale: f64) -> [f64; 3] {
    std::array::from_fn(|index| left_scale * left[index] + right_scale * right[index])
}

// https://drafts.csswg.org/css-transforms-2/#decomposing-a-3d-matrix
fn decompose_matrix(mut matrix: Matrix4) -> Option<DecomposedMatrix> {
    // Normalize the matrix.
    if matrix[3][3] == 0.0 {
        return None;
    }
    let normalization = matrix[3][3];
    for row in &mut matrix {
        for value in row {
            *value /= normalization;
        }
    }

    // perspectiveMatrix is used to solve for perspective, but it also provides
    // an easy way to test for singularity of the upper 3x3 component.
    let mut perspective_matrix = matrix;
    perspective_matrix[3][..3].fill(0.0);
    perspective_matrix[3][3] = 1.0;
    // Solve the equation by inverting perspectiveMatrix and multiplying
    // rightHandSide by the inverse.
    let inverse_perspective_matrix = invert_matrix(perspective_matrix)?;

    // First, isolate perspective.
    let perspective = if matrix[3][0] != 0.0 || matrix[3][1] != 0.0 || matrix[3][2] != 0.0 {
        // rightHandSide is the right hand side of the equation.
        // Note: It is the bottom side in a row-major matrix
        let bottom_side = matrix[3];
        std::array::from_fn(|row| {
            (0..4)
                .map(|column| inverse_perspective_matrix[column][row] * bottom_side[column])
                .sum()
        })
    } else {
        // No perspective.
        [0.0, 0.0, 0.0, 1.0]
    };

    // Next take care of translation
    let translation = [matrix[0][3], matrix[1][3], matrix[2][3]];

    // Now get scale and shear. 'row' is a 3 element array of 3 component vectors
    let mut row: [[f64; 3]; 3] =
        std::array::from_fn(|column| [matrix[0][column], matrix[1][column], matrix[2][column]]);

    // Compute X scale factor and normalize first row.
    let mut scale = [0.0; 3];
    scale[0] = vector_length(row[0]);
    row[0] = row[0].map(|value| value / scale[0]);

    // Compute XY shear factor and make 2nd row orthogonal to 1st.
    let mut skew = [0.0; 3];
    skew[0] = vector_dot(row[0], row[1]);
    row[1] = combine_vectors(row[1], row[0], 1.0, -skew[0]);

    // Now, compute Y scale and normalize 2nd row.
    scale[1] = vector_length(row[1]);
    row[1] = row[1].map(|value| value / scale[1]);
    skew[0] /= scale[1];

    // Compute XZ and YZ shears, orthogonalize 3rd row
    skew[1] = vector_dot(row[0], row[2]);
    row[2] = combine_vectors(row[2], row[0], 1.0, -skew[1]);
    skew[2] = vector_dot(row[1], row[2]);
    row[2] = combine_vectors(row[2], row[1], 1.0, -skew[2]);

    // Next, get Z scale and normalize 3rd row.
    scale[2] = vector_length(row[2]);
    row[2] = row[2].map(|value| value / scale[2]);
    skew[1] /= scale[2];
    skew[2] /= scale[2];

    // At this point, the matrix (in rows) is orthonormal.
    // Check for a coordinate system flip.  If the determinant
    // is -1, then negate the matrix and the scaling factors.
    let pdum3 = vector_cross(row[1], row[2]);
    if vector_dot(row[0], pdum3) < 0.0 {
        for index in 0..3 {
            scale[index] *= -1.0;
            row[index] = row[index].map(|value| -value);
        }
    }

    // Now, get the rotations out
    let mut rotation = [
        0.5 * (1.0 + row[0][0] - row[1][1] - row[2][2]).max(0.0).sqrt(),
        0.5 * (1.0 - row[0][0] + row[1][1] - row[2][2]).max(0.0).sqrt(),
        0.5 * (1.0 - row[0][0] - row[1][1] + row[2][2]).max(0.0).sqrt(),
        0.5 * (1.0 + row[0][0] + row[1][1] + row[2][2]).max(0.0).sqrt(),
    ];
    if row[2][1] > row[1][2] {
        rotation[0] = -rotation[0];
    }
    if row[0][2] > row[2][0] {
        rotation[1] = -rotation[1];
    }
    if row[1][0] > row[0][1] {
        rotation[2] = -rotation[2];
    }

    // FIXME: This accounts for the fact that the browser coordinate system is left-handed instead of right-handed.
    //        The reason for this is that the positive Y-axis direction points down instead of up. To fix this, we
    //        invert the Y axis. However, it feels like the spec pseudo-code above should have taken something like
    //        this into account, so we're probably doing something else wrong.
    rotation[2] *= -1.0;

    Some(DecomposedMatrix {
        translation,
        scale,
        skew,
        rotation,
        perspective,
    })
}

// https://drafts.csswg.org/css-transforms-2/#recomposing-to-a-3d-matrix
fn recompose_matrix(values: DecomposedMatrix) -> Matrix4 {
    let mut matrix = identity_matrix();

    // apply perspective
    matrix[3] = values.perspective;

    // apply translation
    for row in &mut matrix {
        for column in 0..3 {
            row[3] += values.translation[column] * row[column];
        }
    }

    // apply rotation
    let [x, y, z, w] = values.rotation;
    // Construct a composite rotation matrix from the quaternion values
    // rotationMatrix is a identity 4x4 matrix initially
    let mut rotation_matrix = identity_matrix();
    rotation_matrix[0][0] = 1.0 - 2.0 * (y * y + z * z);
    rotation_matrix[1][0] = 2.0 * (x * y - z * w);
    rotation_matrix[2][0] = 2.0 * (x * z + y * w);
    rotation_matrix[0][1] = 2.0 * (x * y + z * w);
    rotation_matrix[1][1] = 1.0 - 2.0 * (x * x + z * z);
    rotation_matrix[2][1] = 2.0 * (y * z - x * w);
    rotation_matrix[0][2] = 2.0 * (x * z - y * w);
    rotation_matrix[1][2] = 2.0 * (y * z + x * w);
    rotation_matrix[2][2] = 1.0 - 2.0 * (x * x + y * y);
    matrix = multiply_matrices(matrix, rotation_matrix);

    // apply skew
    // temp is a identity 4x4 matrix initially
    let mut temp = identity_matrix();
    if values.skew[2] != 0.0 {
        temp[1][2] = values.skew[2];
        matrix = multiply_matrices(matrix, temp);
    }
    if values.skew[1] != 0.0 {
        temp[1][2] = 0.0;
        temp[0][2] = values.skew[1];
        matrix = multiply_matrices(matrix, temp);
    }
    if values.skew[0] != 0.0 {
        temp[0][2] = 0.0;
        temp[0][1] = values.skew[0];
        matrix = multiply_matrices(matrix, temp);
    }

    // apply scale
    for index in 0..3 {
        for row in &mut matrix {
            row[index] *= values.scale[index];
        }
    }
    matrix
}

// https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
fn slerp_quaternions(from: [f64; 4], to: [f64; 4], delta: f32) -> [f64; 4] {
    let product = from
        .iter()
        .zip(to)
        .map(|(from, to)| from * to)
        .sum::<f64>()
        .clamp(-1.0, 1.0);
    if product.abs() >= 1.0 {
        return from;
    }
    let theta = product.acos();
    let weight = (f64::from(delta) * theta).sin() / (1.0 - product * product).sqrt();
    let from_multiplier = (f64::from(delta) * theta).cos() - product * weight;
    if weight.abs() < f64::from(f32::EPSILON) {
        return from.map(|component| component * from_multiplier);
    }
    if from_multiplier.abs() < f64::from(f32::EPSILON) {
        return to.map(|component| component * weight);
    }
    std::array::from_fn(|index| from[index] * from_multiplier + to[index] * weight)
}

fn interpolate_matrices(from: Matrix4, to: Matrix4, delta: f32) -> Option<Matrix4> {
    let from = decompose_matrix(from)?;
    let to = decompose_matrix(to)?;
    let interpolate_array = |from: [f64; 3], to: [f64; 3]| {
        std::array::from_fn(|index| interpolate_f64(from[index], to[index], delta, None))
    };
    let perspective =
        std::array::from_fn(|index| interpolate_f64(from.perspective[index], to.perspective[index], delta, None));
    Some(recompose_matrix(DecomposedMatrix {
        translation: interpolate_array(from.translation, to.translation),
        scale: interpolate_array(from.scale, to.scale),
        skew: interpolate_array(from.skew, to.skew),
        rotation: slerp_quaternions(from.rotation, to.rotation, delta),
        perspective,
    }))
}

fn transformation_to_matrix(
    context: Option<&FfiAnimationContext>,
    function: u8,
    arguments: &[RetainedStyleValueData],
) -> Option<Matrix4> {
    let number = |argument: &RetainedStyleValueData| match argument.data() {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Percentage { value } => Some(*value / 100.0),
        _ => None,
    };
    let length = |argument: &RetainedStyleValueData, reference_length: Option<f64>| match argument.data() {
        StyleValueData::Length { value, unit } => crate::style_compute::absolute_length_to_px(*value, *unit),
        StyleValueData::Percentage { value } => {
            reference_length.map(|reference_length| value / 100.0 * reference_length)
        }
        _ => None,
    };
    let angle = |argument: &RetainedStyleValueData| match argument.data() {
        StyleValueData::Angle { value, unit } => angle_to_degrees(*value, *unit).map(f64::to_radians),
        _ => None,
    };
    let numbers = || arguments.iter().map(number).collect::<Option<Vec<_>>>();
    let reference_box = context
        .filter(|context| context.has_transform_reference_box)
        .map(|context| {
            (
                context.transform_reference_box_width,
                context.transform_reference_box_height,
            )
        });
    let translation_matrix = |x: f64, y: f64, z: f64| {
        let mut matrix = identity_matrix();
        matrix[0][3] = x;
        matrix[1][3] = y;
        matrix[2][3] = z;
        matrix
    };
    let scale_matrix = |x: f64, y: f64, z: f64| {
        let mut matrix = identity_matrix();
        matrix[0][0] = x;
        matrix[1][1] = y;
        matrix[2][2] = z;
        matrix
    };
    let rotation_matrix = |axis: [f64; 3], angle: f64| {
        let axis_length = vector_length(axis);
        if axis_length < 1e-5 {
            return identity_matrix();
        }
        let [x, y, z] = axis.map(|component| component / axis_length);
        let cosine = angle.cos();
        let sine = angle.sin();
        let one_minus_cosine = 1.0 - cosine;
        [
            [
                cosine + x * x * one_minus_cosine,
                x * y * one_minus_cosine - z * sine,
                x * z * one_minus_cosine + y * sine,
                0.0,
            ],
            [
                y * x * one_minus_cosine + z * sine,
                cosine + y * y * one_minus_cosine,
                y * z * one_minus_cosine - x * sine,
                0.0,
            ],
            [
                z * x * one_minus_cosine - y * sine,
                z * y * one_minus_cosine + x * sine,
                cosine + z * z * one_minus_cosine,
                0.0,
            ],
            [0.0, 0.0, 0.0, 1.0],
        ]
    };

    match (function, arguments) {
        (TRANSFORM_FUNCTION_MATRIX, [a, b, c, d, e, f]) => Some([
            [number(a)?, number(c)?, 0.0, number(e)?],
            [number(b)?, number(d)?, 0.0, number(f)?],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ]),
        (TRANSFORM_FUNCTION_MATRIX_3D, values) if values.len() == 16 => {
            let values = numbers()?;
            Some(std::array::from_fn(|row| {
                std::array::from_fn(|column| values[column * 4 + row])
            }))
        }
        (TRANSFORM_FUNCTION_PERSPECTIVE, [argument]) => match argument.data() {
            StyleValueData::Keyword { keyword } if *keyword == crate::style_compute::none_keyword() => {
                Some(identity_matrix())
            }
            _ => {
                let depth = length(argument, None)?.max(1.0);
                let mut matrix = identity_matrix();
                matrix[3][2] = -1.0 / depth;
                Some(matrix)
            }
        },
        (TRANSFORM_FUNCTION_TRANSLATE | TRANSFORM_FUNCTION_TRANSLATE_X, [x]) => Some(translation_matrix(
            length(x, reference_box.map(|(width, _)| width))?,
            0.0,
            0.0,
        )),
        (TRANSFORM_FUNCTION_TRANSLATE, [x, y]) => Some(translation_matrix(
            length(x, reference_box.map(|(width, _)| width))?,
            length(y, reference_box.map(|(_, height)| height))?,
            0.0,
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Y, [y]) => Some(translation_matrix(
            0.0,
            length(y, reference_box.map(|(_, height)| height))?,
            0.0,
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Z, [z]) => Some(translation_matrix(0.0, 0.0, length(z, None)?)),
        (TRANSFORM_FUNCTION_TRANSLATE_3D, [x, y, z]) => Some(translation_matrix(
            length(x, reference_box.map(|(width, _)| width))?,
            length(y, reference_box.map(|(_, height)| height))?,
            length(z, None)?,
        )),
        (TRANSFORM_FUNCTION_SCALE, [value]) => {
            let value = number(value)?;
            Some(scale_matrix(value, value, 1.0))
        }
        (TRANSFORM_FUNCTION_SCALE, [x, y]) => Some(scale_matrix(number(x)?, number(y)?, 1.0)),
        (TRANSFORM_FUNCTION_SCALE_X, [x]) => Some(scale_matrix(number(x)?, 1.0, 1.0)),
        (TRANSFORM_FUNCTION_SCALE_Y, [y]) => Some(scale_matrix(1.0, number(y)?, 1.0)),
        (TRANSFORM_FUNCTION_SCALE_Z, [z]) => Some(scale_matrix(1.0, 1.0, number(z)?)),
        (TRANSFORM_FUNCTION_SCALE_3D, [x, y, z]) => Some(scale_matrix(number(x)?, number(y)?, number(z)?)),
        (TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z, [value]) => {
            Some(rotation_matrix([0.0, 0.0, 1.0], angle(value)?))
        }
        (TRANSFORM_FUNCTION_ROTATE_X, [value]) => Some(rotation_matrix([1.0, 0.0, 0.0], angle(value)?)),
        (TRANSFORM_FUNCTION_ROTATE_Y, [value]) => Some(rotation_matrix([0.0, 1.0, 0.0], angle(value)?)),
        (TRANSFORM_FUNCTION_ROTATE_3D, [x, y, z, value]) => {
            Some(rotation_matrix([number(x)?, number(y)?, number(z)?], angle(value)?))
        }
        (TRANSFORM_FUNCTION_SKEW | TRANSFORM_FUNCTION_SKEW_X, [x]) => {
            let mut matrix = identity_matrix();
            matrix[0][1] = angle(x)?.tan();
            Some(matrix)
        }
        (TRANSFORM_FUNCTION_SKEW, [x, y]) => {
            let mut matrix = identity_matrix();
            matrix[0][1] = angle(x)?.tan();
            matrix[1][0] = angle(y)?.tan();
            Some(matrix)
        }
        (TRANSFORM_FUNCTION_SKEW_Y, [y]) => {
            let mut matrix = identity_matrix();
            matrix[1][0] = angle(y)?.tan();
            Some(matrix)
        }
        _ => None,
    }
}

fn matrix_transformation(property: u16, matrix: Matrix4) -> StyleValueData {
    let arguments = (0..16)
        .map(|index| retained_number(matrix[index % 4][index / 4]))
        .collect();
    StyleValueData::Transformation {
        property,
        transform_function: TRANSFORM_FUNCTION_MATRIX_3D,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    }
}

enum TransformMatrixInterpolationError {
    NotConvertible,
    NonInvertible,
}

fn interpolate_transform_matrix_suffix(
    context: Option<&FfiAnimationContext>,
    property: u16,
    from: &[RetainedStyleValueData],
    to: &[RetainedStyleValueData],
    delta: f32,
) -> Result<RetainedStyleValueData, TransformMatrixInterpolationError> {
    let post_multiply = |transformations: &[RetainedStyleValueData]| {
        let mut result = identity_matrix();
        for transformation in transformations {
            let StyleValueData::Transformation {
                transform_function,
                values,
                ..
            } = transformation.data()
            else {
                return None;
            };
            result = multiply_matrices(
                result,
                transformation_to_matrix(context, *transform_function, values.as_slice())?,
            );
        }
        Some(result)
    };
    let from = post_multiply(from).ok_or(TransformMatrixInterpolationError::NotConvertible)?;
    let to = post_multiply(to).ok_or(TransformMatrixInterpolationError::NotConvertible)?;
    let matrix = interpolate_matrices(from, to, delta).ok_or(TransformMatrixInterpolationError::NonInvertible)?;
    let transformation = Arc::into_raw(Arc::new(matrix_transformation(property, matrix)));
    Ok(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) })
}

fn interpolate_transform_list(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<Option<StyleValueData>> {
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == crate::style_compute::none_keyword())
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == crate::style_compute::none_keyword())
    {
        // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
        // * If both Va and Vb are none:
        //   * Vresult is none.
        return Some(Some(StyleValueData::Keyword {
            keyword: crate::style_compute::none_keyword(),
        }));
    }

    let decode_transform_list = |value: &StyleValueData| match value {
        StyleValueData::ValueList {
            values,
            separator,
            collapsible,
        } => Some((
            values
                .as_slice()
                .iter()
                .map(RetainedStyleValueData::clone_retained)
                .collect::<Vec<_>>(),
            *separator,
            *collapsible,
        )),
        StyleValueData::Keyword { keyword } if *keyword == crate::style_compute::none_keyword() => {
            Some((Vec::new(), 0, false))
        }
        _ => None,
    };
    let (mut from_values, from_separator, from_collapsible) = decode_transform_list(from)?;
    let (mut to_values, to_separator, to_collapsible) = decode_transform_list(to)?;
    let (separator, collapsible) = if from_values.is_empty() {
        (to_separator, to_collapsible)
    } else {
        (from_separator, from_collapsible)
    };
    if !from_values.is_empty()
        && !to_values.is_empty()
        && (from_separator != to_separator || from_collapsible != to_collapsible)
    {
        return None;
    }
    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
    // * Treating none as a list of zero length, if Va or Vb differ in length:
    //   * extend the shorter list to the length of the longer list, setting the function at each additional
    //     position to the identity transform function matching the function at the corresponding position in the
    //     longer list. Both transform function lists are then interpolated following the next rule.
    if from_values.len() != to_values.len() {
        let (shorter, longer) = if from_values.len() < to_values.len() {
            (&mut from_values, &to_values)
        } else {
            (&mut to_values, &from_values)
        };
        for transformation in &longer[shorter.len()..] {
            let StyleValueData::Transformation {
                property,
                transform_function,
                ..
            } = transformation.data()
            else {
                return None;
            };
            shorter.push(identity_transformation(*property, *transform_function)?);
        }
    }

    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
    // *  Let Vresult be an empty list. Beginning at the start of Va and Vb, compare the corresponding functions at each
    //    position:
    //   * While the functions have either the same name, or are derivatives of the same primitive transform
    //     function, interpolate the corresponding pair of functions as described in § 10 Interpolation of
    //     primitives and derived transform functions and append the result to Vresult.
    let mut transformations = Vec::with_capacity(from_values.len());
    for (index, (from, to)) in from_values.iter().zip(&to_values).enumerate() {
        let (
            StyleValueData::Transformation {
                property: from_property,
                transform_function: from_function,
                values: from_arguments,
            },
            StyleValueData::Transformation {
                transform_function: to_function,
                values: to_arguments,
                ..
            },
        ) = (from.data(), to.data())
        else {
            return None;
        };
        if from_function == to_function
            && *from_function == TRANSFORM_FUNCTION_PERSPECTIVE
            && index + 1 == from_values.len()
        {
            let ([from_argument], [to_argument]) = (from_arguments.as_slice(), to_arguments.as_slice()) else {
                return None;
            };
            let reciprocal_depth = |argument: &RetainedStyleValueData| match argument.data() {
                StyleValueData::Length { value, unit } => Some((1.0 / value.max(1.0), Some(*unit))),
                StyleValueData::Keyword { keyword } if *keyword == crate::style_compute::none_keyword() => {
                    Some((0.0, None))
                }
                _ => None,
            };
            let (Some((from_reciprocal_depth, from_unit)), Some((to_reciprocal_depth, to_unit))) =
                (reciprocal_depth(from_argument), reciprocal_depth(to_argument))
            else {
                return None;
            };
            if from_unit.is_some() && to_unit.is_some() && from_unit != to_unit {
                return None;
            }

            // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
            // The transform functions <matrix()>, matrix3d() and perspective() get converted into 4x4 matrices first and
            // interpolated as defined in section Interpolation of Matrices afterwards.
            // OPTIMIZATION: A perspective matrix's only varying component is the negative reciprocal of its depth, so
            //               interpolating that component and inverting it produces the same result without materializing
            //               and decomposing two matrices.
            let reciprocal_depth = interpolate_f64(from_reciprocal_depth, to_reciprocal_depth, delta, None);
            let argument = if reciprocal_depth == 0.0 {
                retained_none_keyword()
            } else {
                let value = Arc::into_raw(Arc::new(StyleValueData::Length {
                    value: 1.0 / reciprocal_depth,
                    unit: from_unit
                        .or(to_unit)
                        .unwrap_or_else(crate::style_compute::px_length_unit),
                }));
                unsafe { RetainedStyleValueData::from_retained_pointer(value) }
            };
            let transformation = Arc::into_raw(Arc::new(StyleValueData::Transformation {
                property: *from_property,
                transform_function: *from_function,
                values: RetainedStyleValueDataList::from_retained_values(vec![argument]),
            }));
            transformations.push(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) });
            continue;
        }
        let Some((transform_function, from_arguments, to_arguments)) =
            convert_transform_pair_to_common_primitive(*from_function, from_arguments, *to_function, to_arguments)
        else {
            // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
            //   * If the pair do not have a common name or primitive transform function, post-multiply the remaining
            //     transform functions in each of Va and Vb respectively to produce two 4x4 matrices. Interpolate these two
            //     matrices as described in § 11 Interpolation of Matrices, append the result to Vresult, and cease
            //     iterating over Va and Vb.
            let transformation = interpolate_transform_matrix_suffix(
                context,
                *from_property,
                &from_values[index..],
                &to_values[index..],
                delta,
            );
            match transformation {
                Ok(transformation) => transformations.push(transformation),
                Err(TransformMatrixInterpolationError::NotConvertible) => return None,
                Err(TransformMatrixInterpolationError::NonInvertible) => {
                    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
                    // In some cases, an animation might cause a transformation matrix to be singular or non-invertible.
                    // For example, an animation in which scale moves from 1 to -1. At the time when the matrix is in
                    // such a state, the transformed element is not rendered.
                    // If one of the matrices for interpolation is non-invertible, the used animation function must
                    // fall-back to a discrete animation according to the rules of the respective animation specification.
                    return Some(None);
                }
            }
            break;
        };

        // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
        // Two different types of transform functions that share the same primitive, or transform functions of the same
        // type with different number of arguments can be interpolated. Both transform functions need a former
        // conversion to the common primitive first and get interpolated numerically afterwards. The computed value will
        // be the primitive with the resulting interpolated arguments.
        if transform_function == TRANSFORM_FUNCTION_ROTATE_3D {
            let from_arguments = RetainedStyleValueDataList::from_retained_values(
                from_arguments
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
            );
            let to_arguments = RetainedStyleValueDataList::from_retained_values(
                to_arguments
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
            );
            let transformation = interpolate_rotate_3d(
                *from_property,
                transform_function,
                &from_arguments,
                &to_arguments,
                delta,
            )?;
            let transformation = Arc::into_raw(Arc::new(transformation));
            transformations.push(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) });
            continue;
        }

        let mut arguments = Vec::with_capacity(from_arguments.len());
        for (from, to) in from_arguments.iter().zip(to_arguments) {
            if matches!(
                transform_function,
                TRANSFORM_FUNCTION_TRANSLATE | TRANSFORM_FUNCTION_TRANSLATE_3D
            ) {
                arguments.push(interpolate_translate_component(
                    property_id,
                    from.data(),
                    to.data(),
                    delta,
                )?);
                continue;
            }
            let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, &[]);
            if !result.handled || result.value.is_null() {
                return None;
            };
            arguments.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
        }
        let transformation = Arc::into_raw(Arc::new(StyleValueData::Transformation {
            property: *from_property,
            transform_function,
            values: RetainedStyleValueDataList::from_retained_values(arguments),
        }));
        transformations.push(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) });
    }

    Some(Some(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(transformations),
        separator,
        collapsible,
    }))
}

fn interpolate_value(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let animation_type = property_animation_type(property_id);
    let is_stroke_dasharray = animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::property_metadata::property_id::STROKE_DASHARRAY;
    if is_stroke_dasharray
        && (!matches!(from, StyleValueData::ValueList { .. }) || !matches!(to, StyleValueData::ValueList { .. }))
    {
        // https://svgwg.org/svg2-draft/painting.html#StrokeDashing
        // If either start or end compute to none or are invalid, start or end are combined using the discrete animation type.
        return discrete_value(context, from, to, delta);
    }
    if (animation_type == ANIMATION_TYPE_REPEATABLE_LIST || is_stroke_dasharray)
        && let (
            StyleValueData::ValueList {
                values: from_values,
                separator,
                collapsible,
            },
            StyleValueData::ValueList { values: to_values, .. },
        ) = (from, to)
    {
        // https://svgwg.org/svg2-draft/painting.html#StrokeDashing
        // Otherwise, repeat both dash patterns of start and end value list until the length of elements in
        // both value lists match. Each item is then combined by computed value.
        // https://drafts.csswg.org/web-animations-1/#repeatable-list
        // Same as by computed value except that if the two lists have differing numbers of items, they are first repeated to the least common multiple number of items.
        // Each item is then combined by computed value.
        // If a pair of values cannot be combined or if any component value uses discrete animation, then the property values combine as discrete.
        let from_length = from_values.as_slice().len();
        let to_length = to_values.as_slice().len();
        if from_length == 0 || to_length == 0 {
            return not_handled();
        }
        let mut a = from_length;
        let mut b = to_length;
        while b != 0 {
            (a, b) = (b, a % b);
        }
        let Some(list_size) = (from_length / a).checked_mul(to_length) else {
            return not_handled();
        };

        let mut values = Vec::with_capacity(list_size);
        for index in 0..list_size {
            let result = interpolate_scalar_value(
                property_id,
                from_values.as_slice()[index % from_length].data(),
                to_values.as_slice()[index % to_length].data(),
                delta,
                &[],
            );
            if !result.handled {
                return not_handled();
            }
            if result.value.is_null() {
                return discrete_value(context, from, to, delta);
            }
            values.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
        }
        return owned(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(values),
            separator: *separator,
            collapsible: *collapsible,
        });
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::property_metadata::property_id::FONT_STYLE
        && let (
            StyleValueData::FontStyle {
                font_style: from_font_style,
                angle_value: from_angle,
            },
            StyleValueData::FontStyle {
                font_style: to_font_style,
                angle_value: to_angle,
            },
        ) = (from, to)
    {
        // https://drafts.csswg.org/css-fonts-4/#font-style-prop
        // Animation type: by computed value type; normal animates as oblique 0deg
        let normalize = |font_style: u8, angle: &RetainedStyleValueData| {
            if font_style == FONT_STYLE_NORMAL {
                return Some((FONT_STYLE_OBLIQUE, Some((0.0, 0))));
            }
            match angle.optional_data() {
                Some(StyleValueData::Angle { value, unit }) => Some((font_style, Some((*value, *unit)))),
                Some(_) => None,
                None => Some((font_style, None)),
            }
        };
        let (Some((from_font_style, from_angle)), Some((to_font_style, to_angle))) = (
            normalize(*from_font_style, from_angle),
            normalize(*to_font_style, to_angle),
        ) else {
            return not_handled();
        };
        let font_style = if from_font_style == to_font_style {
            from_font_style
        } else if !context.is_some_and(|context| context.allow_discrete) {
            return handled_without_value();
        } else if delta < 0.5 {
            from_font_style
        } else {
            to_font_style
        };
        let angle_value = match (from_angle, to_angle) {
            (Some((from_value, from_unit)), Some((to_value, to_unit))) => {
                let (Some(from_value), Some(to_value)) = (
                    angle_to_degrees(from_value, from_unit),
                    angle_to_degrees(to_value, to_unit),
                ) else {
                    return not_handled();
                };
                let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
                    value: interpolate_f64(from_value, to_value, delta, Some((-90.0, 90.0))),
                    unit: 0,
                }));
                unsafe { RetainedStyleValueData::from_retained_pointer(angle) }
            }
            _ => unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) },
        };
        return owned(StyleValueData::FontStyle {
            font_style,
            angle_value,
        });
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::property_metadata::property_id::VISIBILITY {
        return interpolate_visibility(context, from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::property_metadata::property_id::CONTENT_VISIBILITY
    {
        return interpolate_content_visibility(context, from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::property_metadata::property_id::DISPLAY {
        return interpolate_display(context, from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::property_metadata::property_id::SCALE {
        return interpolate_scale(from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::property_metadata::property_id::TRANSLATE {
        return interpolate_translate(from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::property_metadata::property_id::ROTATE {
        return interpolate_individual_rotate(from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::property_metadata::property_id::FONT_VARIATION_SETTINGS
    {
        return interpolate_font_variation_settings(context, property_id, from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::property_metadata::property_id::TRANSFORM
        && let Some(value) = interpolate_transform_list(context, property_id, from, to, delta)
    {
        return value.map_or_else(
            || {
                if context.is_some_and(|context| context.allow_discrete) {
                    discrete_value(context, from, to, delta)
                } else {
                    handled_without_value()
                }
            },
            owned,
        );
    }
    if animation_type != ANIMATION_TYPE_BY_COMPUTED_VALUE {
        return not_handled();
    }
    let result = interpolate_scalar_value(property_id, from, to, delta, &[]);
    if result.handled && result.value.is_null() && context.is_some_and(|context| context.allow_discrete) {
        return discrete_value(context, from, to, delta);
    }
    result
}

/// Attempt Rust-owned style value interpolation without consulting C++ or the DOM.
///
/// # Safety
/// `context` must be null or point at a live `FfiAnimationContext`. `from` and `to` must point at live
/// `StyleValueData` allocations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_interpolate_scalar_style_value(
    context: *const FfiAnimationContext,
    property_id: u16,
    from: *const StyleValueData,
    to: *const StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    crate::abort_on_panic(|| {
        interpolate_value(
            unsafe { context.as_ref() },
            property_id,
            unsafe { &*from },
            unsafe { &*to },
            delta,
        )
    })
}

/// Attempt Rust-owned style value composition without consulting C++ or the DOM.
///
/// # Safety
/// `underlying` and `animated` must point at live `StyleValueData` allocations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_composite_scalar_style_value(
    underlying: *const StyleValueData,
    animated: *const StyleValueData,
    operation: FfiCompositeOperation,
) -> FfiAnimationValueResult {
    crate::abort_on_panic(|| composite_scalar_value(unsafe { &*underlying }, unsafe { &*animated }, operation))
}
