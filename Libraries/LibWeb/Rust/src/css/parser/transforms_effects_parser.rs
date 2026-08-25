/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use super::component_value::{ComponentKind, ComponentValue};
use super::token_stream::TokenStream;
use super::value_parser::{
    NumericRange, ParseContext, ParseOutcome, VALUE_TYPE_ANGLE, VALUE_TYPE_INTEGER, VALUE_TYPE_NUMBER,
    VALUE_TYPE_PERCENTAGE, equals_ascii_case_insensitive, parse_angle_value,
    parse_calculated_numeric_value_with_ranges, parse_integer_value, parse_length_from_stream,
    parse_length_percentage_from_stream, parse_number_from_stream, parse_number_percentage_value, parse_number_value,
    parse_percentage_value, parse_tree_counting_value, parse_url_value,
};
use crate::css::css_enums::{keyword_from_ascii_case_insensitive, keyword_to_step_position, step_position};
use crate::css::css_tokenizer::ParserTokenKind;
use crate::css::math_functions::math_function_from_name;
use crate::css::parser::color_parser::parse_color_value;
use crate::css::property_metadata::{property_accepted_keywords, property_id};
use crate::css::style_compute::px_length_unit;
use crate::css::style_value::{
    RetainedLinearEasingStop, RetainedLinearEasingStopList, RetainedStyleValueData, RetainedStyleValueDataList,
    StyleValueData,
};
use std::sync::Arc;

include!(concat!(env!("OUT_DIR"), "/transform_functions_generated.rs"));

const PARAMETER_ANGLE: u8 = 0;
const PARAMETER_LENGTH: u8 = 1;
const PARAMETER_LENGTH_NONE: u8 = 2;
const PARAMETER_LENGTH_PERCENTAGE: u8 = 3;
const PARAMETER_NUMBER: u8 = 4;
const PARAMETER_NUMBER_PERCENTAGE: u8 = 5;

fn retained(value: StyleValueData) -> RetainedStyleValueData {
    RetainedStyleValueData::from_owned(value)
}

fn transformation(property: u16, transform_function: u8, values: Vec<StyleValueData>) -> StyleValueData {
    StyleValueData::Transformation {
        property,
        transform_function,
        values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
    }
}

fn value_list(values: Vec<StyleValueData>, separator: u8, collapsible: bool) -> StyleValueData {
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
        separator,
        collapsible,
    }
}

fn function_index(name: &[u16]) -> Option<usize> {
    TRANSFORM_FUNCTION_NAMES
        .iter()
        .position(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
}

pub(crate) fn is_transform_effect_function_name(name: &[u16]) -> bool {
    function_index(name).is_some()
        || [
            "linear",
            "cubic-bezier",
            "steps",
            "blur",
            "brightness",
            "contrast",
            "drop-shadow",
            "grayscale",
            "hue-rotate",
            "invert",
            "opacity",
            "saturate",
            "sepia",
            "url",
            "src",
        ]
        .iter()
        .any(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
}

fn parse_calculated_argument(
    context: &ParseContext,
    property: u16,
    value_type: u8,
    percentages_resolve_as: Option<u8>,
    name: &[u16],
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    parse_calculated_numeric_value_with_ranges(
        context,
        property,
        value_type,
        percentages_resolve_as,
        NumericRange::INFINITE,
        name,
        values,
    )
}

fn parse_angle_argument(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    allow_unitless_zero: bool,
) -> Option<StyleValueData> {
    if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        return parse_calculated_argument(context, property, VALUE_TYPE_ANGLE, None, name, values);
    }
    parse_angle_value(context, value, NumericRange::INFINITE).or_else(|| {
        (allow_unitless_zero
            && matches!(
                &value.kind,
                ComponentKind::Token(ParserTokenKind::Number { value, .. }) if *value == 0.0
            ))
        .then_some(StyleValueData::Angle { value: 0.0, unit: 0 })
    })
}

fn parse_number_percentage_argument(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        return parse_calculated_argument(context, property, VALUE_TYPE_NUMBER, None, name, values)
            .or_else(|| parse_calculated_argument(context, property, VALUE_TYPE_PERCENTAGE, None, name, values));
    }
    parse_number_percentage_value(value, NumericRange::INFINITE, NumericRange::INFINITE)
}

fn parse_transform_argument(
    context: &ParseContext,
    property: u16,
    parameter_type: u8,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    let parsed = match parameter_type {
        PARAMETER_ANGLE => {
            let value = parse_angle_argument(context, property, stream.next_token(), true)?;
            stream.discard_a_token();
            value
        }
        PARAMETER_LENGTH | PARAMETER_LENGTH_NONE => {
            if parameter_type == PARAMETER_LENGTH_NONE
                && stream
                    .next_token()
                    .ident()
                    .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"none"))
            {
                stream.discard_a_token();
                StyleValueData::Keyword {
                    keyword: crate::css::css_enums::keyword::NONE,
                }
            } else {
                parse_length_from_stream(context, property, &mut stream, NumericRange::INFINITE)?
            }
        }
        PARAMETER_LENGTH_PERCENTAGE => parse_length_percentage_from_stream(
            context,
            property,
            &mut stream,
            NumericRange::INFINITE,
            NumericRange::INFINITE,
        )?,
        PARAMETER_NUMBER => parse_number_from_stream(context, property, &mut stream, NumericRange::INFINITE)?,
        PARAMETER_NUMBER_PERCENTAGE => {
            let value = parse_number_percentage_argument(context, property, stream.next_token())?;
            stream.discard_a_token();
            value
        }
        _ => return None,
    };
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(parsed)
}

pub(crate) fn parse_transform_function(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    let (name, values) = value.function()?;
    let function = function_index(name)?;
    let arguments = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    let parameter_types = TRANSFORM_FUNCTION_PARAMETER_TYPES[function];
    let required = TRANSFORM_FUNCTION_PARAMETER_REQUIRED[function];
    if arguments.len() > parameter_types.len()
        || (arguments.len() < parameter_types.len() && required[arguments.len()])
        || arguments.iter().any(|argument| argument.is_empty())
    {
        return None;
    }
    let values = arguments
        .into_iter()
        .zip(parameter_types)
        .map(|(argument, parameter_type)| parse_transform_argument(context, property, *parameter_type, argument))
        .collect::<Option<Vec<_>>>()?;
    Some(transformation(property_id::TRANSFORM, function as u8, values))
}

fn non_whitespace(values: &[ComponentValue]) -> Vec<&ComponentValue> {
    values.iter().filter(|value| !value.is_whitespace()).collect()
}

pub(crate) fn parse_transform(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let transformations = non_whitespace(values)
        .into_iter()
        .map(|value| parse_transform_function(context, property, value))
        .collect::<Option<Vec<_>>>()?;
    (!transformations.is_empty()).then(|| value_list(transformations, 0, true))
}

fn parse_rotate(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let values = non_whitespace(values);
    if values.len() == 1 {
        return Some(transformation(
            property,
            transform_function::ROTATE,
            vec![parse_angle_argument(context, property, values[0], false)?],
        ));
    }
    if values.len() == 2 {
        let (axis, angle) = if values[0].ident().is_some() {
            (values[0], values[1])
        } else {
            (values[1], values[0])
        };
        let function = if axis
            .ident()
            .is_some_and(|axis| equals_ascii_case_insensitive(axis, b"x"))
        {
            transform_function::ROTATE_X
        } else if axis
            .ident()
            .is_some_and(|axis| equals_ascii_case_insensitive(axis, b"y"))
        {
            transform_function::ROTATE_Y
        } else if axis
            .ident()
            .is_some_and(|axis| equals_ascii_case_insensitive(axis, b"z"))
        {
            transform_function::ROTATE_Z
        } else {
            return None;
        };
        return Some(transformation(
            property,
            function,
            vec![parse_angle_argument(context, property, angle, false)?],
        ));
    }
    if values.len() == 4 {
        let angle_index = if parse_angle_argument(context, property, values[0], false).is_some() {
            0
        } else {
            3
        };
        let angle = parse_angle_argument(context, property, values[angle_index], false)?;
        let numbers = values
            .iter()
            .enumerate()
            .filter(|(index, _)| *index != angle_index)
            .map(|(_, value)| {
                let mut stream = TokenStream::new(std::slice::from_ref(*value));
                parse_number_from_stream(context, property, &mut stream, NumericRange::INFINITE)
            })
            .collect::<Option<Vec<_>>>()?;
        let mut arguments = numbers;
        arguments.push(angle);
        return Some(transformation(property, transform_function::ROTATE3D, arguments));
    }
    None
}

fn parse_translate(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let values = non_whitespace(values);
    if !(1..=3).contains(&values.len()) {
        return None;
    }
    let parse_length_percentage = |value: &ComponentValue| {
        let mut stream = TokenStream::new(std::slice::from_ref(value));
        parse_length_percentage_from_stream(
            context,
            property,
            &mut stream,
            NumericRange::INFINITE,
            NumericRange::INFINITE,
        )
    };
    let x = parse_length_percentage(values[0])?;
    let y = if let Some(value) = values.get(1) {
        parse_length_percentage(value)?
    } else {
        StyleValueData::Length {
            value: 0.0,
            unit: px_length_unit(),
        }
    };
    if let Some(z) = values.get(2) {
        let mut stream = TokenStream::new(std::slice::from_ref(*z));
        let z = parse_length_from_stream(context, property, &mut stream, NumericRange::INFINITE)?;
        Some(transformation(property, transform_function::TRANSLATE3D, vec![x, y, z]))
    } else {
        Some(transformation(property, transform_function::TRANSLATE, vec![x, y]))
    }
}

fn parse_scale(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let values = non_whitespace(values);
    if !(1..=3).contains(&values.len()) {
        return None;
    }
    let mut parsed = values
        .into_iter()
        .map(|value| parse_number_percentage_argument(context, property, value))
        .collect::<Option<Vec<_>>>()?;
    if parsed.len() == 1 {
        parsed.push(parsed[0].clone());
    }
    let function = if parsed.len() == 3 {
        transform_function::SCALE3D
    } else {
        transform_function::SCALE
    };
    Some(transformation(property, function, parsed))
}

fn keyword_none(values: &[ComponentValue]) -> Option<StyleValueData> {
    let values = non_whitespace(values);
    (values.len() == 1
        && values[0]
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"none")))
    .then_some(StyleValueData::Keyword {
        keyword: crate::css::css_enums::keyword::NONE,
    })
}

fn easing(
    kind: u8,
    linear_stops: Vec<RetainedLinearEasingStop>,
    coordinates: [Option<StyleValueData>; 4],
    number_of_intervals: Option<StyleValueData>,
    step_position: u8,
) -> StyleValueData {
    let [x1, y1, x2, y2] = coordinates.map(|value| value.map_or_else(RetainedStyleValueData::none, retained));
    StyleValueData::Easing {
        kind,
        linear_stops: RetainedLinearEasingStopList::from_retained_elements(linear_stops),
        x1,
        y1,
        x2,
        y2,
        number_of_intervals: number_of_intervals.map_or_else(RetainedStyleValueData::none, retained),
        step_position,
    }
}

fn parse_numeric_component(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    value_type: u8,
    range: NumericRange,
) -> Option<StyleValueData> {
    if matches!(value_type, VALUE_TYPE_INTEGER | VALUE_TYPE_NUMBER)
        && let Some(value) = parse_tree_counting_value(context, value, u8::from(value_type == VALUE_TYPE_INTEGER))
    {
        return Some(value);
    }
    if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        return parse_calculated_numeric_value_with_ranges(context, property, value_type, None, range, name, values);
    }
    match value_type {
        VALUE_TYPE_INTEGER => parse_integer_value(value, range),
        VALUE_TYPE_NUMBER => parse_number_value(value, range),
        VALUE_TYPE_PERCENTAGE => parse_percentage_value(value, range),
        _ => None,
    }
}

fn parse_linear_easing(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut stops = Vec::new();
    for argument in values.split(ComponentValue::is_comma) {
        let argument = non_whitespace(argument);
        if argument.is_empty() {
            return None;
        }
        let mut position = 0;
        let mut output = argument.get(position).and_then(|value| {
            parse_numeric_component(context, property, value, VALUE_TYPE_NUMBER, NumericRange::INFINITE)
        });
        if output.is_some() {
            position += 1;
        }
        let mut inputs = Vec::new();
        while inputs.len() < 2 {
            let Some(input) = argument.get(position).and_then(|value| {
                parse_numeric_component(context, property, value, VALUE_TYPE_PERCENTAGE, NumericRange::INFINITE)
            }) else {
                break;
            };
            inputs.push(input);
            position += 1;
        }
        if output.is_none()
            && let Some(value) = argument.get(position).and_then(|value| {
                parse_numeric_component(context, property, value, VALUE_TYPE_NUMBER, NumericRange::INFINITE)
            })
        {
            output = Some(value);
            position += 1;
        }
        if position != argument.len() {
            return None;
        }
        let output = output?;
        if inputs.is_empty() {
            stops.push(RetainedLinearEasingStop::from_retained_values(
                retained(output),
                RetainedStyleValueData::none(),
            ));
        } else {
            for input in inputs {
                stops.push(RetainedLinearEasingStop::from_retained_values(
                    retained(output.clone()),
                    retained(input),
                ));
            }
        }
    }
    (!stops.is_empty()).then(|| easing(0, stops, [None, None, None, None], None, 0))
}

fn parse_cubic_bezier_easing(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let arguments = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.len() != 4 {
        return None;
    }
    let mut coordinates = Vec::new();
    for (index, argument) in arguments.into_iter().enumerate() {
        let argument = non_whitespace(argument);
        if argument.len() != 1 {
            return None;
        }
        let range = if matches!(index, 0 | 2) {
            NumericRange::new(0.0, 1.0)
        } else {
            NumericRange::INFINITE
        };
        coordinates.push(parse_numeric_component(
            context,
            property,
            argument[0],
            VALUE_TYPE_NUMBER,
            range,
        )?);
    }
    Some(easing(
        1,
        Vec::new(),
        [
            Some(coordinates.remove(0)),
            Some(coordinates.remove(0)),
            Some(coordinates.remove(0)),
            Some(coordinates.remove(0)),
        ],
        None,
        0,
    ))
}

fn parse_steps_easing(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let arguments = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.is_empty() || arguments.len() > 2 {
        return None;
    }
    let interval_argument = non_whitespace(arguments[0]);
    if interval_argument.len() != 1 {
        return None;
    }
    let position = if let Some(position_argument) = arguments.get(1) {
        let position_argument = non_whitespace(position_argument);
        if position_argument.len() != 1 {
            return None;
        }
        keyword_to_step_position(keyword_from_ascii_case_insensitive(position_argument[0].ident()?)?)?
    } else {
        step_position::END
    };
    let minimum = if position == step_position::JUMP_NONE { 2.0 } else { 1.0 };
    let intervals = parse_numeric_component(
        context,
        property,
        interval_argument[0],
        VALUE_TYPE_INTEGER,
        NumericRange::new(minimum, f64::from(i32::MAX)),
    )?;
    Some(easing(
        2,
        Vec::new(),
        [None, None, None, None],
        Some(intervals),
        position,
    ))
}

fn parse_easing_value(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let value = non_whitespace(values);
    if value.len() != 1 {
        return None;
    }
    if let Some(identifier) = value[0].ident() {
        if let Some(keyword) = keyword_from_ascii_case_insensitive(identifier)
            && property_accepted_keywords(property).binary_search(&keyword).is_ok()
        {
            return Some(StyleValueData::Keyword { keyword });
        }
        let position = if equals_ascii_case_insensitive(identifier, b"step-start") {
            step_position::START
        } else if equals_ascii_case_insensitive(identifier, b"step-end") {
            step_position::END
        } else {
            return None;
        };
        return Some(easing(
            2,
            Vec::new(),
            [None, None, None, None],
            Some(StyleValueData::Integer { value: 1 }),
            position,
        ));
    }
    let (name, arguments) = value[0].function()?;
    if equals_ascii_case_insensitive(name, b"linear") {
        parse_linear_easing(context, property, arguments)
    } else if equals_ascii_case_insensitive(name, b"cubic-bezier") {
        parse_cubic_bezier_easing(context, property, arguments)
    } else if equals_ascii_case_insensitive(name, b"steps") {
        parse_steps_easing(context, property, arguments)
    } else {
        None
    }
}

fn parse_easing_list(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let values = values
        .split(ComponentValue::is_comma)
        .map(|value| parse_easing_value(context, property, value))
        .collect::<Option<Vec<_>>>()?;
    (!values.is_empty()).then(|| value_list(values, 1, true))
}

fn filter(kind: u8, color_operation: u8, value: StyleValueData) -> StyleValueData {
    StyleValueData::Filter {
        kind,
        color_operation,
        value: retained(value),
    }
}

fn parse_number_percentage_with_range(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
    range: NumericRange,
) -> Option<StyleValueData> {
    if let Some(value) = parse_tree_counting_value(context, value, 0) {
        return Some(value);
    }
    if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        return parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_NUMBER,
            None,
            range,
            name,
            values,
        )
        .or_else(|| {
            parse_calculated_numeric_value_with_ranges(
                context,
                property,
                VALUE_TYPE_PERCENTAGE,
                None,
                range,
                name,
                values,
            )
        });
    }
    parse_number_percentage_value(value, range, range)
}

fn parse_drop_shadow(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }

    let mut color = parse_color_value(context, property, &mut tokens, false);
    tokens.discard_whitespace();
    let offset_x = parse_length_from_stream(context, property, &mut tokens, NumericRange::INFINITE)?;
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        return None;
    }
    let offset_y = parse_length_from_stream(context, property, &mut tokens, NumericRange::INFINITE)?;
    tokens.discard_whitespace();

    let mut blur_radius = None;
    if tokens.has_next_token() {
        blur_radius = parse_length_from_stream(context, property, &mut tokens, NumericRange::INFINITE);
        tokens.discard_whitespace();
        if color.is_none() && (blur_radius.is_none() || tokens.has_next_token()) {
            color = parse_color_value(context, property, &mut tokens, false);
            color.as_ref()?;
        } else if blur_radius.is_none() {
            return None;
        }
    }
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }

    Some(filter(
        1,
        0,
        StyleValueData::Shadow {
            shadow_type: 1,
            color: color.map_or_else(RetainedStyleValueData::none, retained),
            offset_x: retained(offset_x),
            offset_y: retained(offset_y),
            blur_radius: blur_radius.map_or_else(RetainedStyleValueData::none, retained),
            spread_distance: RetainedStyleValueData::none(),
            placement: 0,
        },
    ))
}

fn contains_random_function(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| match &value.kind {
        ComponentKind::Function { name, values } => {
            equals_ascii_case_insensitive(name, b"random") || contains_random_function(values)
        }
        ComponentKind::SimpleBlock { values, .. } => contains_random_function(values),
        ComponentKind::Token(_) => false,
    })
}

fn parse_filter_function(context: &ParseContext, property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    let (name, values) = value.function()?;
    let values_without_whitespace = non_whitespace(values);
    if equals_ascii_case_insensitive(name, b"blur") {
        if contains_random_function(values) {
            return None;
        }
        let radius = if values_without_whitespace.is_empty() {
            StyleValueData::Length {
                value: 0.0,
                unit: px_length_unit(),
            }
        } else {
            let mut tokens = TokenStream::new(values);
            let radius = parse_length_from_stream(context, property, &mut tokens, NumericRange::NON_NEGATIVE)?;
            tokens.discard_whitespace();
            if tokens.has_next_token() {
                return None;
            }
            radius
        };
        return Some(filter(0, 0, radius));
    }
    if equals_ascii_case_insensitive(name, b"drop-shadow") {
        return parse_drop_shadow(context, property, values);
    }
    if equals_ascii_case_insensitive(name, b"hue-rotate") {
        let angle = if values_without_whitespace.is_empty() {
            StyleValueData::Angle { value: 0.0, unit: 0 }
        } else if values_without_whitespace.len() == 1 {
            parse_angle_argument(context, property, values_without_whitespace[0], true)?
        } else {
            return None;
        };
        return Some(filter(2, 0, angle));
    }

    let color_operation = if equals_ascii_case_insensitive(name, b"brightness") {
        0
    } else if equals_ascii_case_insensitive(name, b"contrast") {
        1
    } else if equals_ascii_case_insensitive(name, b"grayscale") {
        2
    } else if equals_ascii_case_insensitive(name, b"invert") {
        3
    } else if equals_ascii_case_insensitive(name, b"opacity") {
        4
    } else if equals_ascii_case_insensitive(name, b"saturate") {
        5
    } else if equals_ascii_case_insensitive(name, b"sepia") {
        6
    } else {
        return None;
    };
    let mut amount = if values_without_whitespace.is_empty() {
        StyleValueData::Number { value: 1.0 }
    } else if values_without_whitespace.len() == 1 {
        parse_number_percentage_with_range(
            context,
            property,
            values_without_whitespace[0],
            NumericRange::NON_NEGATIVE,
        )?
    } else {
        return None;
    };
    if matches!(color_operation, 2 | 3 | 4 | 6) {
        match &mut amount {
            StyleValueData::Number { value } if *value > 1.0 => *value = 1.0,
            StyleValueData::Percentage { value } if *value > 100.0 => *value = 100.0,
            _ => {}
        }
    }
    Some(filter(3, color_operation, amount))
}

fn parse_filter_list(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    if let Some(none) = keyword_none(values) {
        return Some(none);
    }
    let values = non_whitespace(values)
        .into_iter()
        .map(|value| parse_url_value(context, value).or_else(|| parse_filter_function(context, property, value)))
        .collect::<Option<Vec<_>>>()?;
    (!values.is_empty()).then(|| value_list(values, 0, false))
}

fn contains_math_function(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| match &value.kind {
        ComponentKind::Function { name, values } => {
            math_function_from_name(name).is_some() || contains_math_function(values)
        }
        ComponentKind::SimpleBlock { values, .. } => contains_math_function(values),
        ComponentKind::Token(_) => false,
    })
}

pub(crate) fn parse_transform_effect_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if property == property_id::TRANSFORM
        && keyword_none(values).is_none()
        && non_whitespace(values).iter().any(|value| value.function().is_none())
    {
        return ParseOutcome::NotHandled;
    }
    let parsed = match property {
        property_id::TRANSFORM => keyword_none(values).or_else(|| parse_transform(context, property, values)),
        property_id::ROTATE => keyword_none(values).or_else(|| parse_rotate(context, property, values)),
        property_id::TRANSLATE => keyword_none(values).or_else(|| parse_translate(context, property, values)),
        property_id::SCALE => keyword_none(values).or_else(|| parse_scale(context, property, values)),
        property_id::ANIMATION_TIMING_FUNCTION | property_id::TRANSITION_TIMING_FUNCTION => {
            parse_easing_list(context, property, values)
        }
        property_id::FILTER | property_id::BACKDROP_FILTER => parse_filter_list(context, property, values),
        _ => return ParseOutcome::NotHandled,
    };
    if parsed.is_none() && contains_math_function(values) {
        // NB: Some calculation forms retain parser context which the calculation tree cannot
        //     represent yet.
        return ParseOutcome::NotHandled;
    }
    if parsed.is_none()
        && matches!(
            property,
            property_id::ANIMATION_TIMING_FUNCTION | property_id::TRANSITION_TIMING_FUNCTION
        )
        && values.iter().any(ComponentValue::is_comma)
    {
        // NB: Substitution can present the arguments of steps() without the function wrapper, but
        //     this parser does not have the context needed to parse it.
        return ParseOutcome::NotHandled;
    }
    parsed.map_or(ParseOutcome::Invalid, |value| ParseOutcome::Parsed(Arc::new(value)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;

    unsafe extern "C" fn discard_interned_string(_: *const u16, _: usize) -> usize {
        0
    }

    fn context() -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            is_ua_style_sheet: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            intern_utf16_fly_string: Some(discard_interned_string),
            normalize_svg_path_data: None,
            precomputed_svg_paths: std::ptr::null(),
            precomputed_svg_path_count: 0,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            descriptor_integer_resolution_context: std::ptr::null(),
            resolve_descriptor_integer: None,
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_transform_effect_property(&context(), property, &values)
    }

    #[test]
    fn parses_transform_functions_and_lists() {
        for source in [
            "matrix(1, 0, 0, 1, 10, 20)",
            "translate(10px) rotate(0) scale(2, 50%)",
            "perspective(none)",
            "translate3d(calc(10px + 5%), 20px, 0)",
        ] {
            assert!(
                matches!(parse(property_id::TRANSFORM, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        for source in ["matrix(1, 0)", "translate()", "rotate(1)"] {
            assert!(
                matches!(parse(property_id::TRANSFORM, source), ParseOutcome::Invalid),
                "{source}"
            );
        }
        assert!(matches!(
            parse(property_id::TRANSFORM, "-100px * -1"),
            ParseOutcome::NotHandled
        ));
    }

    #[test]
    fn parses_individual_transform_properties() {
        for (property, source) in [
            (property_id::ROTATE, "x 45deg"),
            (property_id::ROTATE, "1 2 3 45deg"),
            (property_id::SCALE, "2 50% 1"),
            (property_id::TRANSLATE, "10% 20px 30px"),
            (property_id::TRANSLATE, "calc(10px + 5%)"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
        assert!(matches!(parse(property_id::ROTATE, "x"), ParseOutcome::Invalid));
        assert!(matches!(parse(property_id::SCALE, "1 2 3 4"), ParseOutcome::Invalid));
        assert!(matches!(
            parse(
                property_id::TRANSLATE,
                "random(fixed random(-2, -1), 0%, 100%) random(0%, 100%)"
            ),
            ParseOutcome::NotHandled
        ));
    }

    #[test]
    fn parses_easing_lists() {
        for source in [
            "ease, linear, step-start",
            "linear(0, 0.5 25% 75%, 1)",
            "linear(0 0%, 100% 1)",
            "cubic-bezier(0.25, -2, 0.75, 3)",
            "steps(4, jump-both)",
            "steps(2, jump-none)",
        ] {
            assert!(
                matches!(
                    parse(property_id::TRANSITION_TIMING_FUNCTION, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        for source in [
            "linear()",
            "cubic-bezier(-0.1, 0, 1, 1)",
            "steps(1, jump-none)",
            "steps(0)",
        ] {
            assert!(
                matches!(
                    parse(property_id::ANIMATION_TIMING_FUNCTION, source),
                    ParseOutcome::Invalid
                ),
                "{source}"
            );
        }
        assert!(matches!(
            parse(property_id::ANIMATION_TIMING_FUNCTION, "2, start"),
            ParseOutcome::NotHandled
        ));
    }

    #[test]
    fn parses_filter_lists() {
        for source in [
            "none",
            "blur() brightness(150%) contrast(.5)",
            "drop-shadow(red 1px 2px 3px)",
            "drop-shadow(1px 2px blue)",
            "grayscale(200%) hue-rotate(0) opacity() saturate(2) sepia(.5)",
            "url(filters.svg#blur) invert(calc(50% + 10%))",
        ] {
            assert!(
                matches!(parse(property_id::FILTER, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        for source in [
            "blur(-1px)",
            "brightness(-1)",
            "drop-shadow()",
            "drop-shadow(1px)",
            "hue-rotate(1)",
        ] {
            assert!(
                matches!(parse(property_id::BACKDROP_FILTER, source), ParseOutcome::Invalid),
                "{source}"
            );
        }
    }
}
