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
    NumericRange, ParseContext, ParseOutcome, equals_ascii_case_insensitive, parse_length_from_stream,
    parse_length_percentage_from_stream, parse_number_from_stream, retain_fly_string,
};
use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::property_metadata::{
    longhands_for_shorthand, property_accepted_value_types, property_has_coordinating_list_multiplicity, property_id,
};
use crate::css::style_compute::px_length_unit;
use crate::css::style_value::{
    RetainedPropertyIdList, RetainedShapePoint, RetainedShapePointList, RetainedStyleValueData,
    RetainedStyleValueDataList, RetainedUtf16FlyString, StyleValueData,
};
use std::sync::Arc;

const VALUE_TYPE_BACKGROUND_POSITION: u8 = 4;
const VALUE_TYPE_ANCHOR: u8 = 0;
const VALUE_TYPE_BASIC_SHAPE: u8 = 5;
const VALUE_TYPE_CORNER_SHAPE: u8 = 7;
const VALUE_TYPE_FIT_CONTENT: u8 = 14;
const VALUE_TYPE_POSITION: u8 = 32;
const VALUE_TYPE_RECT: u8 = 34;

// NB: These are the generated C++ PositionEdge enum values.
const EDGE_CENTER: u8 = 0;
const EDGE_LEFT: u8 = 1;
const EDGE_RIGHT: u8 = 2;
const EDGE_TOP: u8 = 3;
const EDGE_BOTTOM: u8 = 4;

fn retained(value: StyleValueData) -> RetainedStyleValueData {
    RetainedStyleValueData::from_owned(value)
}

fn edge(edge: Option<u8>, offset: Option<StyleValueData>) -> StyleValueData {
    StyleValueData::Edge {
        has_edge: edge.is_some(),
        edge: edge.unwrap_or(0),
        offset: offset.map_or_else(RetainedStyleValueData::none, retained),
    }
}

fn position(edge_x: StyleValueData, edge_y: StyleValueData) -> StyleValueData {
    StyleValueData::Position {
        edge_x: retained(edge_x),
        edge_y: retained(edge_y),
    }
}

pub(crate) fn center_position() -> StyleValueData {
    position(edge(Some(EDGE_CENTER), None), edge(Some(EDGE_CENTER), None))
}

fn parse_position_edge(tokens: &mut TokenStream<'_>) -> Option<u8> {
    tokens.discard_whitespace();
    let identifier = tokens.next_token().ident()?;
    let edge = if equals_ascii_case_insensitive(identifier, b"center") {
        EDGE_CENTER
    } else if equals_ascii_case_insensitive(identifier, b"left") {
        EDGE_LEFT
    } else if equals_ascii_case_insensitive(identifier, b"right") {
        EDGE_RIGHT
    } else if equals_ascii_case_insensitive(identifier, b"top") {
        EDGE_TOP
    } else if equals_ascii_case_insensitive(identifier, b"bottom") {
        EDGE_BOTTOM
    } else {
        return None;
    };
    tokens.discard_a_token();
    Some(edge)
}

fn is_horizontal(edge: u8, accept_center: bool) -> bool {
    matches!(edge, EDGE_LEFT | EDGE_RIGHT) || (accept_center && edge == EDGE_CENTER)
}

fn is_vertical(edge: u8, accept_center: bool) -> bool {
    matches!(edge, EDGE_TOP | EDGE_BOTTOM) || (accept_center && edge == EDGE_CENTER)
}

fn parse_length_percentage(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    parse_length_percentage_from_stream(
        context,
        property,
        tokens,
        NumericRange::INFINITE,
        NumericRange::INFINITE,
    )
}

fn alternative_1(context: &ParseContext, property: u16, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    if let Some(parsed_edge) = parse_position_edge(tokens) {
        return Some(if is_horizontal(parsed_edge, false) {
            position(edge(Some(parsed_edge), None), edge(Some(EDGE_CENTER), None))
        } else if is_vertical(parsed_edge, false) {
            position(edge(Some(EDGE_CENTER), None), edge(Some(parsed_edge), None))
        } else {
            position(edge(Some(EDGE_CENTER), None), edge(Some(EDGE_CENTER), None))
        });
    }
    let offset = parse_length_percentage(context, property, tokens)?;
    Some(position(edge(None, Some(offset)), edge(Some(EDGE_CENTER), None)))
}

fn alternative_2(tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let mut first = parse_position_edge(tokens)?;
    let mut second = parse_position_edge(tokens)?;
    if is_vertical(first, false) || is_horizontal(second, false) {
        std::mem::swap(&mut first, &mut second);
    }
    if !is_horizontal(first, true) || !is_vertical(second, true) {
        return None;
    }
    Some(position(edge(Some(first), None), edge(Some(second), None)))
}

fn alternative_3(context: &ParseContext, property: u16, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let parse_position_or_length = |tokens: &mut TokenStream<'_>, horizontal: bool| {
        let mut candidate = tokens.clone();
        if let Some(parsed_edge) = parse_position_edge(&mut candidate) {
            if !(if horizontal {
                is_horizontal(parsed_edge, true)
            } else {
                is_vertical(parsed_edge, true)
            }) {
                return None;
            }
            *tokens = candidate;
            return Some(edge(Some(parsed_edge), None));
        }
        let offset = parse_length_percentage(context, property, tokens)?;
        Some(edge(None, Some(offset)))
    };
    let edge_x = parse_position_or_length(tokens, true)?;
    let edge_y = parse_position_or_length(tokens, false)?;
    Some(position(edge_x, edge_y))
}

fn alternative_4(context: &ParseContext, property: u16, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let parse_group = |tokens: &mut TokenStream<'_>| {
        let parsed_edge = parse_position_edge(tokens)?;
        let offset = parse_length_percentage(context, property, tokens)?;
        Some((parsed_edge, offset))
    };
    let first = parse_group(tokens)?;
    let second = parse_group(tokens)?;
    if is_horizontal(first.0, false) && is_vertical(second.0, false) {
        return Some(position(
            edge(Some(first.0), Some(first.1)),
            edge(Some(second.0), Some(second.1)),
        ));
    }
    if is_vertical(first.0, false) && is_horizontal(second.0, false) {
        return Some(position(
            edge(Some(second.0), Some(second.1)),
            edge(Some(first.0), Some(first.1)),
        ));
    }
    None
}

fn alternative_5(context: &ParseContext, property: u16, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    let parse_group = |tokens: &mut TokenStream<'_>| {
        let parsed_edge = parse_position_edge(tokens)?;
        let mut candidate = tokens.clone();
        let offset = parse_length_percentage(context, property, &mut candidate);
        if parsed_edge == EDGE_CENTER && offset.is_some() {
            return None;
        }
        if offset.is_some() {
            *tokens = candidate;
        }
        Some((parsed_edge, offset))
    };
    let mut first = parse_group(tokens)?;
    let mut second = parse_group(tokens)?;
    if first.1.is_some() == second.1.is_some() {
        return None;
    }
    if is_vertical(first.0, false) || is_horizontal(second.0, false) {
        std::mem::swap(&mut first, &mut second);
    }
    if !is_horizontal(first.0, true) || !is_vertical(second.0, true) {
        return None;
    }
    Some(position(edge(Some(first.0), first.1), edge(Some(second.0), second.1)))
}

pub(crate) fn parse_position_from_stream(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    background_position: bool,
) -> Option<StyleValueData> {
    let mut candidate = tokens.clone();
    if let Some(parsed) = alternative_4(context, property, &mut candidate) {
        *tokens = candidate;
        return Some(parsed);
    }
    if background_position {
        let mut candidate = tokens.clone();
        if let Some(parsed) = alternative_5(context, property, &mut candidate) {
            *tokens = candidate;
            return Some(parsed);
        }
    }
    let mut candidate = tokens.clone();
    if let Some(parsed) = alternative_3(context, property, &mut candidate) {
        *tokens = candidate;
        return Some(parsed);
    }
    let mut candidate = tokens.clone();
    if let Some(parsed) = alternative_2(&mut candidate) {
        *tokens = candidate;
        return Some(parsed);
    }
    let mut candidate = tokens.clone();
    let parsed = alternative_1(context, property, &mut candidate)?;
    *tokens = candidate;
    Some(parsed)
}

fn parse_position_value(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
    background_position: bool,
) -> Option<StyleValueData> {
    let try_full = |parser: &dyn Fn(&mut TokenStream<'_>) -> Option<StyleValueData>| {
        let mut tokens = TokenStream::new(values);
        let parsed = parser(&mut tokens)?;
        tokens.discard_whitespace();
        (!tokens.has_next_token()).then_some(parsed)
    };

    // NB: Keep the C++ alternative order. Shorter alternatives can match a prefix of longer ones.
    if let Some(parsed) = try_full(&|tokens| alternative_4(context, property, tokens)) {
        return Some(parsed);
    }
    if background_position && let Some(parsed) = try_full(&|tokens| alternative_5(context, property, tokens)) {
        return Some(parsed);
    }
    if let Some(parsed) = try_full(&|tokens| alternative_3(context, property, tokens)) {
        return Some(parsed);
    }
    if let Some(parsed) = try_full(&|tokens| alternative_2(tokens)) {
        return Some(parsed);
    }
    try_full(&|tokens| alternative_1(context, property, tokens))
}

fn value_list(values: Vec<StyleValueData>) -> StyleValueData {
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(values.into_iter().map(retained).collect()),
        separator: 1,
        collapsible: true,
    }
}

fn parse_position_list(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
    background_position: bool,
) -> Option<Vec<StyleValueData>> {
    values
        .split(ComponentValue::is_comma)
        .map(|item| parse_position_value(context, property, item, background_position))
        .collect()
}

fn parse_background_position(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let positions = parse_position_list(context, property, values, true)?;
    let mut x_values = Vec::with_capacity(positions.len());
    let mut y_values = Vec::with_capacity(positions.len());
    for position in positions {
        let StyleValueData::Position { edge_x, edge_y } = position else {
            unreachable!();
        };
        x_values.push(edge_x.data().clone());
        y_values.push(edge_y.data().clone());
    }
    Some(StyleValueData::Shorthand {
        shorthand_property: property_id::BACKGROUND_POSITION,
        sub_properties: RetainedPropertyIdList::from_property_ids(vec![
            property_id::BACKGROUND_POSITION_X,
            property_id::BACKGROUND_POSITION_Y,
        ]),
        values: RetainedStyleValueDataList::from_retained_values(vec![
            retained(value_list(x_values)),
            retained(value_list(y_values)),
        ]),
    })
}

fn parse_background_position_axis(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let horizontal = property == property_id::BACKGROUND_POSITION_X;
    let parsed = values
        .split(ComponentValue::is_comma)
        .map(|item| {
            let mut tokens = TokenStream::new(item);
            tokens.discard_whitespace();
            let mut parsed_edge = None;
            let mut offset = None;
            let mut edge_candidate = tokens.clone();
            if let Some(candidate) = parse_position_edge(&mut edge_candidate) {
                if !(if horizontal {
                    is_horizontal(candidate, true)
                } else {
                    is_vertical(candidate, true)
                }) {
                    return None;
                }
                parsed_edge = Some(candidate);
                tokens = edge_candidate;
                if candidate != EDGE_CENTER {
                    offset = parse_length_percentage(context, property, &mut tokens);
                }
            } else {
                offset = Some(parse_length_percentage(context, property, &mut tokens)?);
            }
            tokens.discard_whitespace();
            if tokens.has_next_token() {
                return None;
            }
            Some(edge(parsed_edge, offset))
        })
        .collect::<Option<Vec<_>>>()?;
    Some(value_list(parsed))
}

pub(crate) fn is_position_shape_function_name(name: &[u16]) -> bool {
    [
        "anchor",
        "anchor-size",
        "circle",
        "ellipse",
        "fit-content",
        "inset",
        "path",
        "polygon",
        "rect",
        "shape",
        "superellipse",
        "xywh",
    ]
    .iter()
    .any(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
}

fn keyword_value(value: &ComponentValue, accepted: &[u16]) -> Option<StyleValueData> {
    let parsed_keyword = keyword_from_ascii_case_insensitive(value.ident()?)?;
    accepted.contains(&parsed_keyword).then_some(StyleValueData::Keyword {
        keyword: parsed_keyword,
    })
}

fn border_radius(horizontal: StyleValueData, vertical: StyleValueData) -> StyleValueData {
    let is_elliptical = horizontal != vertical;
    StyleValueData::BorderRadius {
        is_elliptical,
        horizontal_radius: retained(horizontal),
        vertical_radius: retained(vertical),
    }
}

fn expanded_four(values: &[StyleValueData]) -> Option<[StyleValueData; 4]> {
    Some(match values {
        [first] => [first.clone(), first.clone(), first.clone(), first.clone()],
        [first, second] => [first.clone(), second.clone(), first.clone(), second.clone()],
        [first, second, third] => [first.clone(), second.clone(), third.clone(), second.clone()],
        [first, second, third, fourth] => [first.clone(), second.clone(), third.clone(), fourth.clone()],
        _ => return None,
    })
}

fn parse_border_radius_rect(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    let mut horizontal = Vec::new();
    let mut vertical = Vec::new();
    let mut reading_vertical = false;
    tokens.discard_whitespace();
    while tokens.has_next_token() {
        if tokens.next_token().is_delim(b'/') {
            if reading_vertical || horizontal.is_empty() {
                return None;
            }
            reading_vertical = true;
            tokens.discard_a_token();
            tokens.discard_whitespace();
            continue;
        }
        let radius = parse_length_percentage_from_stream(
            context,
            property,
            tokens,
            NumericRange::NON_NEGATIVE,
            NumericRange::NON_NEGATIVE,
        )?;
        if reading_vertical {
            vertical.push(radius);
        } else {
            horizontal.push(radius);
        }
        tokens.discard_whitespace();
    }
    if horizontal.is_empty() || horizontal.len() > 4 || vertical.len() > 4 || (reading_vertical && vertical.is_empty())
    {
        return None;
    }
    let horizontal = expanded_four(&horizontal)?;
    let vertical = if vertical.is_empty() {
        horizontal.clone()
    } else {
        expanded_four(&vertical)?
    };
    let corners: [StyleValueData; 4] =
        std::array::from_fn(|index| border_radius(horizontal[index].clone(), vertical[index].clone()));
    Some(StyleValueData::BorderRadiusRect {
        top_left: retained(corners[0].clone()),
        top_right: retained(corners[1].clone()),
        bottom_right: retained(corners[2].clone()),
        bottom_left: retained(corners[3].clone()),
    })
}

fn zero_border_radius_rect() -> StyleValueData {
    let zero = StyleValueData::Length {
        value: 0.0,
        unit: px_length_unit(),
    };
    let corner = border_radius(zero.clone(), zero);
    StyleValueData::BorderRadiusRect {
        top_left: retained(corner.clone()),
        top_right: retained(corner.clone()),
        bottom_right: retained(corner.clone()),
        bottom_left: retained(corner),
    }
}

fn parse_single_border_radius(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    let horizontal = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::NON_NEGATIVE,
        NumericRange::NON_NEGATIVE,
    )?;
    tokens.discard_whitespace();
    if tokens.next_token().is_delim(b'/') {
        tokens.discard_a_token();
    }
    let vertical = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::NON_NEGATIVE,
        NumericRange::NON_NEGATIVE,
    )
    .unwrap_or_else(|| horizontal.clone());
    tokens.discard_whitespace();
    (!tokens.has_next_token()).then(|| border_radius(horizontal, vertical))
}

fn parse_border_radius_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    let rect = parse_border_radius_rect(context, property, &mut tokens)?;
    let StyleValueData::BorderRadiusRect {
        top_left,
        top_right,
        bottom_right,
        bottom_left,
    } = rect
    else {
        unreachable!();
    };
    Some(StyleValueData::Shorthand {
        shorthand_property: property_id::BORDER_RADIUS,
        sub_properties: RetainedPropertyIdList::from_property_ids(vec![
            property_id::BORDER_TOP_LEFT_RADIUS,
            property_id::BORDER_TOP_RIGHT_RADIUS,
            property_id::BORDER_BOTTOM_RIGHT_RADIUS,
            property_id::BORDER_BOTTOM_LEFT_RADIUS,
        ]),
        values: RetainedStyleValueDataList::from_retained_values(vec![top_left, top_right, bottom_right, bottom_left]),
    })
}

fn parse_legacy_rect(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let (name, arguments) = values.iter().find(|value| !value.is_whitespace())?.function()?;
    if !equals_ascii_case_insensitive(name, b"rect")
        || values.iter().filter(|value| !value.is_whitespace()).count() != 1
    {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    let mut sides = Vec::with_capacity(4);
    let mut commas = None;
    for side in 0..4 {
        tokens.discard_whitespace();
        let value = if let Some(auto) = keyword_value(tokens.next_token(), &[keyword::AUTO]) {
            tokens.discard_a_token();
            auto
        } else {
            parse_length_from_stream(context, property, &mut tokens, NumericRange::INFINITE)?
        };
        sides.push(value);
        tokens.discard_whitespace();
        if side == 3 {
            break;
        }
        let next_is_comma = tokens.next_token().is_comma();
        let requires_commas = *commas.get_or_insert(next_is_comma);
        if requires_commas != next_is_comma {
            return None;
        }
        if next_is_comma {
            tokens.discard_a_token();
        }
    }
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some(StyleValueData::Rect {
        top: retained(sides[0].clone()),
        right: retained(sides[1].clone()),
        bottom: retained(sides[2].clone()),
        left: retained(sides[3].clone()),
    })
}

fn parse_corner_shape(context: &ParseContext, property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    if let Some(parsed) = keyword_value(
        value,
        &[
            keyword::ROUND,
            keyword::SCOOP,
            keyword::BEVEL,
            keyword::NOTCH,
            keyword::SQUARE,
            keyword::SQUIRCLE,
        ],
    ) {
        return Some(parsed);
    }
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"superellipse") {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    tokens.discard_whitespace();
    let parameter = if tokens
        .next_token()
        .ident()
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"infinity"))
    {
        tokens.discard_a_token();
        StyleValueData::Number { value: f64::INFINITY }
    } else if tokens
        .next_token()
        .ident()
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"-infinity"))
    {
        tokens.discard_a_token();
        StyleValueData::Number {
            value: f64::NEG_INFINITY,
        }
    } else {
        parse_number_from_stream(context, property, &mut tokens, NumericRange::INFINITE)?
    };
    tokens.discard_whitespace();
    (!tokens.has_next_token()).then(|| StyleValueData::Superellipse {
        parameter: retained(parameter),
    })
}

fn positional_shorthand(property: u16, values: Vec<StyleValueData>) -> Option<StyleValueData> {
    let longhands = longhands_for_shorthand(property);
    let expanded = match longhands.len() {
        2 => match values.as_slice() {
            [first] => vec![first.clone(), first.clone()],
            [first, second] => vec![first.clone(), second.clone()],
            _ => return None,
        },
        4 => expanded_four(&values)?.into_iter().collect(),
        _ => return None,
    };
    Some(StyleValueData::Shorthand {
        shorthand_property: property,
        sub_properties: RetainedPropertyIdList::from_property_ids(longhands.to_vec()),
        values: RetainedStyleValueDataList::from_retained_values(expanded.into_iter().map(retained).collect()),
    })
}

fn parse_corner_shape_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    let mut parsed = Vec::new();
    while tokens.has_next_token() {
        tokens.discard_whitespace();
        if !tokens.has_next_token() {
            break;
        }
        parsed.push(parse_corner_shape(context, property, tokens.consume_a_token())?);
    }
    if longhands_for_shorthand(property).is_empty() {
        match parsed.as_slice() {
            [value] => Some(value.clone()),
            _ => None,
        }
    } else {
        positional_shorthand(property, parsed)
    }
}

enum RadialComponent {
    Extent(u8),
    Value(StyleValueData),
}

fn radial_extent(value: &ComponentValue) -> Option<u8> {
    let identifier = value.ident()?;
    if equals_ascii_case_insensitive(identifier, b"closest-corner") {
        Some(0)
    } else if equals_ascii_case_insensitive(identifier, b"closest-side") {
        Some(1)
    } else if equals_ascii_case_insensitive(identifier, b"farthest-corner") {
        Some(2)
    } else if equals_ascii_case_insensitive(identifier, b"farthest-side") {
        Some(3)
    } else {
        None
    }
}

fn radial_size(components: Vec<RadialComponent>) -> Option<StyleValueData> {
    if components.is_empty() || components.len() > 2 {
        return None;
    }
    let mut is_extent = [false; 2];
    let mut extent = [0; 2];
    let mut value = [RetainedStyleValueData::none(), RetainedStyleValueData::none()];
    for (index, component) in components.into_iter().enumerate() {
        match component {
            RadialComponent::Extent(parsed) => {
                is_extent[index] = true;
                extent[index] = parsed;
            }
            RadialComponent::Value(parsed) => value[index] = retained(parsed),
        }
    }
    Some(StyleValueData::RadialSize {
        component_count: u8::from(is_extent[1] || value[1].optional_data().is_some()) + 1,
        is_extent_0: is_extent[0],
        extent_0: extent[0],
        value_0: std::mem::replace(&mut value[0], RetainedStyleValueData::none()),
        is_extent_1: is_extent[1],
        extent_1: extent[1],
        value_1: std::mem::replace(&mut value[1], RetainedStyleValueData::none()),
    })
}

pub(crate) fn parse_radial_size(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    let mut components = Vec::new();
    while components.len() < 2 {
        tokens.discard_whitespace();
        if let Some(extent) = radial_extent(tokens.next_token()) {
            tokens.discard_a_token();
            components.push(RadialComponent::Extent(extent));
            continue;
        }
        let mut candidate = tokens.clone();
        let Some(value) = parse_length_percentage_from_stream(
            context,
            property,
            &mut candidate,
            NumericRange::NON_NEGATIVE,
            NumericRange::NON_NEGATIVE,
        ) else {
            break;
        };
        *tokens = candidate;
        components.push(RadialComponent::Value(value));
    }
    radial_size(components)
}

fn basic_shape(
    kind: u8,
    values: Vec<StyleValueData>,
    fill_rule: u8,
    points: Vec<RetainedShapePoint>,
    path_string: RetainedUtf16FlyString,
) -> StyleValueData {
    let mut values = values.into_iter().map(retained).collect::<Vec<_>>();
    values.resize_with(5, RetainedStyleValueData::none);
    StyleValueData::BasicShape {
        kind,
        v0: values.remove(0),
        v1: values.remove(0),
        v2: values.remove(0),
        v3: values.remove(0),
        v4: values.remove(0),
        fill_rule,
        points: RetainedShapePointList::from_retained_points(points),
        path_string,
    }
}

fn parse_fill_rule(values: &[ComponentValue]) -> Option<u8> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    let identifier = tokens.consume_a_token().ident()?;
    let fill_rule = if equals_ascii_case_insensitive(identifier, b"nonzero") {
        0
    } else if equals_ascii_case_insensitive(identifier, b"evenodd") {
        1
    } else {
        return None;
    };
    tokens.discard_whitespace();
    (!tokens.has_next_token()).then_some(fill_rule)
}

fn parse_round_radius(context: &ParseContext, property: u16, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    if !tokens
        .next_token()
        .ident()
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"round"))
    {
        return Some(zero_border_radius_rect());
    }
    tokens.discard_a_token();
    parse_border_radius_rect(context, property, tokens)
}

fn parse_inset_shape(context: &ParseContext, property: u16, arguments: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(arguments);
    let mut offsets = Vec::new();
    while offsets.len() < 4 {
        let mut candidate = tokens.clone();
        let Some(offset) = parse_length_percentage_from_stream(
            context,
            property,
            &mut candidate,
            NumericRange::INFINITE,
            NumericRange::INFINITE,
        ) else {
            break;
        };
        tokens = candidate;
        offsets.push(offset);
    }
    let offsets = expanded_four(&offsets)?;
    let radius = parse_round_radius(context, property, &mut tokens)?;
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some(basic_shape(
        0,
        vec![
            offsets[0].clone(),
            offsets[1].clone(),
            offsets[2].clone(),
            offsets[3].clone(),
            radius,
        ],
        0,
        vec![],
        RetainedUtf16FlyString::none(),
    ))
}

fn parse_xywh_shape(context: &ParseContext, property: u16, arguments: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(arguments);
    let x = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::INFINITE,
        NumericRange::INFINITE,
    )?;
    let y = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::INFINITE,
        NumericRange::INFINITE,
    )?;
    let width = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::NON_NEGATIVE,
        NumericRange::NON_NEGATIVE,
    )?;
    let height = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::NON_NEGATIVE,
        NumericRange::NON_NEGATIVE,
    )?;
    let radius = parse_round_radius(context, property, &mut tokens)?;
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some(basic_shape(
        1,
        vec![x, y, width, height, radius],
        0,
        vec![],
        RetainedUtf16FlyString::none(),
    ))
}

fn parse_rect_shape(context: &ParseContext, property: u16, arguments: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(arguments);
    let mut sides = Vec::new();
    for _ in 0..4 {
        tokens.discard_whitespace();
        if let Some(auto) = keyword_value(tokens.next_token(), &[keyword::AUTO]) {
            tokens.discard_a_token();
            sides.push(auto);
        } else {
            sides.push(parse_length_percentage_from_stream(
                context,
                property,
                &mut tokens,
                NumericRange::INFINITE,
                NumericRange::INFINITE,
            )?);
        }
    }
    let radius = parse_round_radius(context, property, &mut tokens)?;
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    sides.push(radius);
    Some(basic_shape(2, sides, 0, vec![], RetainedUtf16FlyString::none()))
}

fn parse_circle_or_ellipse(
    context: &ParseContext,
    property: u16,
    arguments: &[ComponentValue],
    ellipse: bool,
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(arguments);
    let mut radius = parse_radial_size(context, property, &mut tokens);
    if radius.as_ref().is_some_and(|radius| {
        matches!(radius, StyleValueData::RadialSize { component_count, .. } if *component_count != if ellipse { 2 } else { 1 })
    }) {
        return None;
    }
    if radius.is_none() {
        radius = radial_size(if ellipse {
            vec![RadialComponent::Extent(1), RadialComponent::Extent(1)]
        } else {
            vec![RadialComponent::Extent(1)]
        });
    }
    tokens.discard_whitespace();
    let parsed_position = if tokens
        .next_token()
        .ident()
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"at"))
    {
        tokens.discard_a_token();
        let remaining = &tokens.values[tokens.position..];
        let parsed = parse_position_value(context, property, remaining, false)?;
        tokens.position = tokens.values.len();
        Some(parsed)
    } else {
        None
    };
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some(StyleValueData::BasicShape {
        kind: if ellipse { 4 } else { 3 },
        v0: retained(radius?),
        v1: parsed_position.map_or_else(RetainedStyleValueData::none, retained),
        v2: RetainedStyleValueData::none(),
        v3: RetainedStyleValueData::none(),
        v4: RetainedStyleValueData::none(),
        fill_rule: 0,
        points: RetainedShapePointList::from_retained_points(vec![]),
        path_string: RetainedUtf16FlyString::none(),
    })
}

fn parse_polygon(context: &ParseContext, property: u16, arguments: &[ComponentValue]) -> Option<StyleValueData> {
    let mut arguments = arguments.split(ComponentValue::is_comma).collect::<Vec<_>>();
    let fill_rule = arguments.first().and_then(|argument| parse_fill_rule(argument));
    let fill_rule = if let Some(fill_rule) = fill_rule {
        arguments.remove(0);
        fill_rule
    } else {
        0
    };
    if arguments.is_empty() {
        return None;
    }
    let points = arguments
        .into_iter()
        .map(|argument| {
            let mut tokens = TokenStream::new(argument);
            let x = parse_length_percentage_from_stream(
                context,
                property,
                &mut tokens,
                NumericRange::INFINITE,
                NumericRange::INFINITE,
            )?;
            let y = parse_length_percentage_from_stream(
                context,
                property,
                &mut tokens,
                NumericRange::INFINITE,
                NumericRange::INFINITE,
            )?;
            tokens.discard_whitespace();
            if tokens.has_next_token() {
                return None;
            }
            Some(RetainedShapePoint::from_retained_values(retained(x), retained(y)))
        })
        .collect::<Option<Vec<_>>>()?;
    Some(basic_shape(
        5,
        vec![],
        fill_rule,
        points,
        RetainedUtf16FlyString::none(),
    ))
}

fn parse_path(context: &ParseContext, arguments: &[ComponentValue]) -> Option<StyleValueData> {
    let arguments = arguments.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.is_empty() || arguments.len() > 2 {
        return None;
    }
    let fill_rule = if arguments.len() == 2 {
        parse_fill_rule(arguments[0])?
    } else {
        0
    };
    let mut tokens = TokenStream::new(arguments[arguments.len() - 1]);
    tokens.discard_whitespace();
    let path = tokens.consume_a_token().string()?.to_vec();
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    let normalize = context.normalize_svg_path_data?;
    crate::css::ffi_stats::bump_cpp_callback(crate::css::ffi_stats::FfiOp::NormalizeSvgPathDataCallback);
    let raw = unsafe { normalize(path.as_ptr(), path.len(), context.is_svg_presentation_attribute) };
    if raw == 0 {
        return None;
    }
    let path_string = unsafe { RetainedUtf16FlyString::from_leaked_raw(raw) };
    Some(basic_shape(6, vec![], fill_rule, vec![], path_string))
}

fn parse_basic_shape(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut significant = values.iter().filter(|value| !value.is_whitespace());
    let (name, arguments) = significant.next()?.function()?;
    if significant.next().is_some() {
        return None;
    }
    if equals_ascii_case_insensitive(name, b"inset") {
        parse_inset_shape(context, property, arguments)
    } else if equals_ascii_case_insensitive(name, b"xywh") {
        parse_xywh_shape(context, property, arguments)
    } else if equals_ascii_case_insensitive(name, b"rect") {
        parse_rect_shape(context, property, arguments)
    } else if equals_ascii_case_insensitive(name, b"circle") {
        parse_circle_or_ellipse(context, property, arguments, false)
    } else if equals_ascii_case_insensitive(name, b"ellipse") {
        parse_circle_or_ellipse(context, property, arguments, true)
    } else if equals_ascii_case_insensitive(name, b"polygon") {
        parse_polygon(context, property, arguments)
    } else if equals_ascii_case_insensitive(name, b"path") {
        parse_path(context, arguments)
    } else {
        None
    }
}

fn shape_box(value: &ComponentValue) -> Option<StyleValueData> {
    let parsed_keyword = keyword_from_ascii_case_insensitive(value.ident()?)?;
    ["content-box", "padding-box", "border-box", "margin-box"]
        .iter()
        .any(|accepted| equals_ascii_case_insensitive(value.ident().unwrap(), accepted.as_bytes()))
        .then_some(StyleValueData::Keyword {
            keyword: parsed_keyword,
        })
}

fn parse_shape_outside(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    if let Some(value) = values
        .iter()
        .find(|value| !value.is_whitespace())
        .and_then(|value| keyword_value(value, &[keyword::NONE]))
        && values.iter().filter(|value| !value.is_whitespace()).count() == 1
    {
        return Some(value);
    }
    let mut tokens = TokenStream::new(values);
    let mut shape = None;
    let mut box_value = None;
    while tokens.has_next_token() {
        tokens.discard_whitespace();
        if !tokens.has_next_token() {
            break;
        }
        if tokens.next_token().function().is_some() {
            if shape.is_some() {
                return None;
            }
            shape = Some(parse_basic_shape(
                context,
                property,
                std::slice::from_ref(tokens.consume_a_token()),
            )?);
        } else {
            if box_value.is_some() {
                return None;
            }
            box_value = Some(shape_box(tokens.consume_a_token())?);
        }
    }
    match (shape, box_value) {
        (Some(shape), Some(box_value)) => Some(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(vec![retained(shape), retained(box_value)]),
            separator: 0,
            collapsible: false,
        }),
        (Some(shape), None) | (None, Some(shape)) => Some(shape),
        (None, None) => None,
    }
}

fn is_border_radius_longhand(property: u16) -> bool {
    matches!(
        property,
        property_id::BORDER_TOP_LEFT_RADIUS
            | property_id::BORDER_TOP_RIGHT_RADIUS
            | property_id::BORDER_BOTTOM_RIGHT_RADIUS
            | property_id::BORDER_BOTTOM_LEFT_RADIUS
            | property_id::BORDER_END_END_RADIUS
            | property_id::BORDER_END_START_RADIUS
            | property_id::BORDER_START_END_RADIUS
            | property_id::BORDER_START_START_RADIUS
    )
}

fn dashed_ident(context: &ParseContext, value: &ComponentValue) -> Option<RetainedUtf16FlyString> {
    let identifier = value.ident()?;
    (identifier.starts_with(&[u16::from(b'-'), u16::from(b'-')])).then(|| retain_fly_string(context, identifier))?
}

fn anchor_side(value: &ComponentValue) -> Option<StyleValueData> {
    let identifier = value.ident()?;
    [
        "inside",
        "outside",
        "top",
        "left",
        "right",
        "bottom",
        "start",
        "end",
        "self-start",
        "self-end",
        "center",
    ]
    .iter()
    .any(|accepted| equals_ascii_case_insensitive(identifier, accepted.as_bytes()))
    .then(|| StyleValueData::Keyword {
        keyword: keyword_from_ascii_case_insensitive(identifier).unwrap(),
    })
}

fn property_allows_anchor(property: u16) -> bool {
    matches!(
        property,
        property_id::INSET
            | property_id::TOP
            | property_id::RIGHT
            | property_id::BOTTOM
            | property_id::LEFT
            | property_id::INSET_BLOCK
            | property_id::INSET_BLOCK_START
            | property_id::INSET_BLOCK_END
            | property_id::INSET_INLINE
            | property_id::INSET_INLINE_START
            | property_id::INSET_INLINE_END
    )
}

pub(crate) fn parse_anchor_function(
    context: &ParseContext,
    property: u16,
    value: &ComponentValue,
) -> Option<StyleValueData> {
    if !property_allows_anchor(property) {
        return None;
    }
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"anchor") {
        return None;
    }
    let arguments = arguments.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.is_empty() || arguments.len() > 2 {
        return None;
    }
    let mut tokens = TokenStream::new(arguments[0]);
    let mut anchor_name = None;
    let mut side = None;
    for _ in 0..2 {
        tokens.discard_whitespace();
        if !tokens.has_next_token() {
            break;
        }
        if let Some(parsed_name) = dashed_ident(context, tokens.next_token()) {
            if anchor_name.is_some() {
                return None;
            }
            tokens.discard_a_token();
            anchor_name = Some(parsed_name);
            continue;
        }
        if side.is_some() {
            break;
        }
        if let Some(parsed_side) = anchor_side(tokens.next_token()) {
            tokens.discard_a_token();
            side = Some(parsed_side);
            continue;
        }
        let parsed_side = parse_length_percentage_from_stream(
            context,
            property,
            &mut tokens,
            NumericRange::INFINITE,
            NumericRange::INFINITE,
        )?;
        if matches!(parsed_side, StyleValueData::Length { .. }) {
            return None;
        }
        side = Some(parsed_side);
    }
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    let fallback = if arguments.len() == 2 {
        let mut candidate = TokenStream::new(arguments[1]);
        candidate.discard_whitespace();
        let parsed = parse_anchor_function(context, property, candidate.next_token())
            .inspect(|_| {
                candidate.discard_a_token();
            })
            .or_else(|| {
                parse_length_percentage_from_stream(
                    context,
                    property,
                    &mut candidate,
                    NumericRange::INFINITE,
                    NumericRange::INFINITE,
                )
            })?;
        candidate.discard_whitespace();
        if candidate.has_next_token() {
            return None;
        }
        Some(parsed)
    } else {
        None
    };
    Some(StyleValueData::Anchor {
        has_anchor_name: anchor_name.is_some(),
        anchor_name: anchor_name.unwrap_or_else(RetainedUtf16FlyString::none),
        anchor_side: retained(side?),
        fallback_value: fallback.map_or_else(RetainedStyleValueData::none, retained),
    })
}

fn anchor_size_keyword(value: &ComponentValue) -> Option<u8> {
    let identifier = value.ident()?;
    ["block", "height", "inline", "self-block", "self-inline", "width"]
        .iter()
        .position(|accepted| equals_ascii_case_insensitive(identifier, accepted.as_bytes()))
        .and_then(|index| u8::try_from(index).ok())
}

fn property_allows_anchor_size(property: u16) -> bool {
    property_allows_anchor(property)
        || matches!(
            property,
            property_id::MARGIN
                | property_id::MARGIN_TOP
                | property_id::MARGIN_RIGHT
                | property_id::MARGIN_BOTTOM
                | property_id::MARGIN_LEFT
                | property_id::MARGIN_BLOCK
                | property_id::MARGIN_BLOCK_START
                | property_id::MARGIN_BLOCK_END
                | property_id::MARGIN_INLINE
                | property_id::MARGIN_INLINE_START
                | property_id::MARGIN_INLINE_END
                | property_id::WIDTH
                | property_id::MIN_WIDTH
                | property_id::MAX_WIDTH
                | property_id::HEIGHT
                | property_id::MIN_HEIGHT
                | property_id::MAX_HEIGHT
                | property_id::BLOCK_SIZE
                | property_id::MIN_BLOCK_SIZE
                | property_id::MAX_BLOCK_SIZE
                | property_id::INLINE_SIZE
                | property_id::MIN_INLINE_SIZE
                | property_id::MAX_INLINE_SIZE
                | property_id::ALIGN_SELF
                | property_id::JUSTIFY_SELF
                | property_id::PLACE_SELF
        )
}

fn parse_anchor_size_function(context: &ParseContext, property: u16, value: &ComponentValue) -> Option<StyleValueData> {
    if !property_allows_anchor_size(property) {
        return None;
    }
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"anchor-size") {
        return None;
    }
    let arguments = arguments.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.is_empty() || arguments.len() > 2 {
        return None;
    }
    let mut tokens = TokenStream::new(arguments[0]);
    let mut anchor_name = None;
    let mut anchor_size = None;
    for _ in 0..2 {
        tokens.discard_whitespace();
        if !tokens.has_next_token() || tokens.next_token().ident().is_none() {
            break;
        }
        if let Some(parsed_name) = dashed_ident(context, tokens.next_token()) {
            if anchor_name.is_some() {
                return None;
            }
            tokens.discard_a_token();
            anchor_name = Some(parsed_name);
            continue;
        }
        let parsed_size = anchor_size_keyword(tokens.next_token())?;
        if anchor_size.is_some() {
            return None;
        }
        tokens.discard_a_token();
        anchor_size = Some(parsed_size);
    }
    tokens.discard_whitespace();
    let has_name_or_size = anchor_name.is_some() || anchor_size.is_some();
    let comma_present = arguments.len() == 2;
    if comma_present && !has_name_or_size {
        return None;
    }
    let fallback_values = if comma_present { arguments[1] } else { arguments[0] };
    let fallback_start = if comma_present { 0 } else { tokens.current_index() };
    let mut fallback_tokens = TokenStream::new(&fallback_values[fallback_start..]);
    fallback_tokens.discard_whitespace();
    if comma_present && !fallback_tokens.has_next_token() {
        return None;
    }
    let fallback = if fallback_tokens.has_next_token() {
        let mut candidate = fallback_tokens.clone();
        let parsed = parse_anchor_size_function(context, property, candidate.next_token())
            .inspect(|_| {
                candidate.discard_a_token();
            })
            .or_else(|| {
                parse_length_percentage_from_stream(
                    context,
                    property,
                    &mut candidate,
                    NumericRange::INFINITE,
                    NumericRange::INFINITE,
                )
            })?;
        candidate.discard_whitespace();
        if candidate.has_next_token() {
            return None;
        }
        Some(parsed)
    } else {
        None
    };
    if fallback.is_some() && !comma_present && has_name_or_size {
        return None;
    }
    Some(StyleValueData::AnchorSize {
        has_anchor_name: anchor_name.is_some(),
        anchor_name: anchor_name.unwrap_or_else(RetainedUtf16FlyString::none),
        has_anchor_size: anchor_size.is_some(),
        anchor_size: anchor_size.unwrap_or(0),
        fallback_value: fallback.map_or_else(RetainedStyleValueData::none, retained),
    })
}

fn contains_anchor_in_math_function(values: &[ComponentValue], inside_math_function: bool) -> bool {
    values.iter().any(|value| match &value.kind {
        ComponentKind::Function { name, values } => {
            let is_math_function = crate::css::math_functions::math_function_from_name(name).is_some();
            (inside_math_function
                && (equals_ascii_case_insensitive(name, b"anchor")
                    || equals_ascii_case_insensitive(name, b"anchor-size")))
                || contains_anchor_in_math_function(values, inside_math_function || is_math_function)
        }
        ComponentKind::SimpleBlock { values, .. } => contains_anchor_in_math_function(values, inside_math_function),
        ComponentKind::Token(_) => false,
    })
}

fn parse_fit_content(context: &ParseContext, property: u16, values: &[ComponentValue]) -> Option<StyleValueData> {
    let value = values.iter().find(|value| !value.is_whitespace())?;
    if values.iter().filter(|value| !value.is_whitespace()).count() != 1 {
        return None;
    }
    if value
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"fit-content"))
    {
        return Some(StyleValueData::Keyword {
            keyword: keyword::FIT_CONTENT,
        });
    }
    let (name, arguments) = value.function()?;
    if !equals_ascii_case_insensitive(name, b"fit-content") {
        return None;
    }
    let mut tokens = TokenStream::new(arguments);
    let argument = parse_length_percentage_from_stream(
        context,
        property,
        &mut tokens,
        NumericRange::INFINITE,
        NumericRange::INFINITE,
    )?;
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some(StyleValueData::Function {
        name: retain_fly_string(context, &"fit-content".encode_utf16().collect::<Vec<_>>())?,
        value: retained(argument),
    })
}

fn parse_inset_leaf(context: &ParseContext, property: u16, tokens: &mut TokenStream<'_>) -> Option<StyleValueData> {
    tokens.discard_whitespace();
    if let Some(auto) = keyword_value(tokens.next_token(), &[keyword::AUTO]) {
        tokens.discard_a_token();
        return Some(auto);
    }
    if let Some(anchor) = parse_anchor_function(context, property, tokens.next_token()) {
        tokens.discard_a_token();
        return Some(anchor);
    }
    if let Some(anchor_size) = parse_anchor_size_function(context, property, tokens.next_token()) {
        tokens.discard_a_token();
        return Some(anchor_size);
    }
    parse_length_percentage_from_stream(
        context,
        property,
        tokens,
        NumericRange::INFINITE,
        NumericRange::INFINITE,
    )
}

fn parse_positional_anchor_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    let mut parsed = Vec::new();
    let maximum = longhands_for_shorthand(property).len();
    while parsed.len() < maximum {
        let mut candidate = tokens.clone();
        let Some(value) = parse_inset_leaf(context, property, &mut candidate) else {
            break;
        };
        tokens = candidate;
        parsed.push(value);
    }
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    positional_shorthand(property, parsed)
}

fn is_inset_or_margin_shorthand(property: u16) -> bool {
    matches!(
        property,
        property_id::INSET
            | property_id::INSET_BLOCK
            | property_id::INSET_INLINE
            | property_id::MARGIN
            | property_id::MARGIN_BLOCK
            | property_id::MARGIN_INLINE
    )
}

pub(crate) fn parse_anchor_fit_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let parsed = if is_inset_or_margin_shorthand(property) {
        parse_positional_anchor_shorthand(context, property, values)
    } else if property_allows_anchor(property) {
        let mut tokens = TokenStream::new(values);
        let parsed = parse_inset_leaf(context, property, &mut tokens);
        tokens.discard_whitespace();
        if parsed.is_none() && contains_anchor_in_math_function(values, false) {
            return ParseOutcome::NotHandled;
        } else if tokens.has_next_token() {
            None
        } else {
            parsed
        }
    } else if property_accepted_value_types(property).contains(&VALUE_TYPE_FIT_CONTENT) {
        let is_fit_or_anchor_size = values.iter().find(|value| !value.is_whitespace()).is_some_and(|value| {
            value
                .ident()
                .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"fit-content"))
                || value.function().is_some_and(|(name, _)| {
                    equals_ascii_case_insensitive(name, b"fit-content")
                        || equals_ascii_case_insensitive(name, b"anchor-size")
                })
        });
        if !is_fit_or_anchor_size {
            return ParseOutcome::NotHandled;
        }
        parse_fit_content(context, property, values).or_else(|| {
            let value = values.iter().find(|value| !value.is_whitespace())?;
            parse_anchor_size_function(context, property, value)
        })
    } else if property_allows_anchor_size(property) {
        let parsed = values
            .iter()
            .find(|value| !value.is_whitespace())
            .and_then(|value| parse_anchor_size_function(context, property, value));
        if parsed.is_none() {
            return ParseOutcome::NotHandled;
        }
        parsed
    } else if property_accepted_value_types(property).contains(&VALUE_TYPE_ANCHOR) {
        values
            .iter()
            .find(|value| !value.is_whitespace())
            .and_then(|value| parse_anchor_function(context, property, value))
    } else {
        return ParseOutcome::NotHandled;
    };
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

pub(crate) fn parse_geometry_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let parsed = if property == property_id::BORDER_RADIUS {
        parse_border_radius_shorthand(context, property, values)
    } else if is_border_radius_longhand(property) {
        parse_single_border_radius(context, property, values)
    } else if property_accepted_value_types(property).contains(&VALUE_TYPE_CORNER_SHAPE) {
        parse_corner_shape_property(context, property, values)
    } else if property_accepted_value_types(property).contains(&VALUE_TYPE_RECT) {
        let is_rect = values
            .iter()
            .find(|value| !value.is_whitespace())
            .and_then(ComponentValue::function)
            .is_some_and(|(name, _)| equals_ascii_case_insensitive(name, b"rect"));
        if !is_rect {
            return ParseOutcome::NotHandled;
        }
        parse_legacy_rect(context, property, values)
    } else if property == property_id::SHAPE_OUTSIDE {
        let is_shape_outside_value = values.iter().find(|value| !value.is_whitespace()).is_some_and(|value| {
            value.ident().is_some_and(|ident| {
                equals_ascii_case_insensitive(ident, b"none")
                    || ["content-box", "padding-box", "border-box", "margin-box"]
                        .iter()
                        .any(|expected| equals_ascii_case_insensitive(ident, expected.as_bytes()))
            }) || value.function().is_some_and(|(name, _)| {
                ["inset", "xywh", "rect", "circle", "ellipse", "polygon", "path", "shape"]
                    .iter()
                    .any(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
            })
        });
        if !is_shape_outside_value {
            return ParseOutcome::NotHandled;
        }
        parse_shape_outside(context, property, values)
    } else if property == property_id::D {
        if let Some(value) = values
            .iter()
            .find(|value| !value.is_whitespace())
            .and_then(|value| keyword_value(value, &[keyword::NONE]))
            .filter(|_| values.iter().filter(|value| !value.is_whitespace()).count() == 1)
        {
            Some(value)
        } else {
            parse_basic_shape(context, property, values).filter(|_| {
                values
                    .iter()
                    .find(|value| !value.is_whitespace())
                    .and_then(ComponentValue::function)
                    .is_some_and(|(name, _)| equals_ascii_case_insensitive(name, b"path"))
            })
        }
    } else if property_accepted_value_types(property).contains(&VALUE_TYPE_BASIC_SHAPE) {
        let is_known_shape = values
            .iter()
            .find(|value| !value.is_whitespace())
            .and_then(ComponentValue::function)
            .is_some_and(|(name, _)| {
                ["inset", "xywh", "rect", "circle", "ellipse", "polygon", "path", "shape"]
                    .iter()
                    .any(|expected| equals_ascii_case_insensitive(name, expected.as_bytes()))
            });
        if !is_known_shape {
            return ParseOutcome::NotHandled;
        }
        parse_basic_shape(context, property, values)
    } else {
        return ParseOutcome::NotHandled;
    };
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

pub(crate) fn parse_position_property(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    let parsed = if property == property_id::BACKGROUND_POSITION {
        parse_background_position(context, property, values)
    } else if matches!(
        property,
        property_id::BACKGROUND_POSITION_X | property_id::BACKGROUND_POSITION_Y
    ) {
        parse_background_position_axis(context, property, values)
    } else {
        let accepted_types = property_accepted_value_types(property);
        let background_position = accepted_types.contains(&VALUE_TYPE_BACKGROUND_POSITION);
        if !background_position && !accepted_types.contains(&VALUE_TYPE_POSITION) {
            return ParseOutcome::NotHandled;
        }
        parse_position_list(context, property, values, background_position).map(|mut positions| {
            if property_has_coordinating_list_multiplicity(property) {
                value_list(positions)
            } else {
                positions.remove(0)
            }
        })
    };
    parsed.map_or(ParseOutcome::Invalid, |parsed| ParseOutcome::Parsed(Arc::new(parsed)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;

    unsafe extern "C" fn discard_interned_string(_: *const u16, _: usize) -> usize {
        0
    }

    unsafe extern "C" fn retain_normalized_path(_: *const u16, _: usize, _: bool) -> usize {
        ak::utf16_short_string_raw("M0 0").unwrap()
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
            declared_namespaces: std::ptr::null(),
            declared_namespace_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            intern_utf16_fly_string: Some(discard_interned_string),
            normalize_svg_path_data: Some(retain_normalized_path),
            length_resolution_context: std::ptr::null(),
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_position_property(&context(), property, &values)
    }

    fn parse_geometry(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_geometry_property(&context(), property, &values)
    }

    fn parse_anchor_fit(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_anchor_fit_property(&context(), property, &values)
    }

    #[test]
    fn parses_position_alternatives() {
        for source in [
            "center",
            "top right",
            "25% bottom",
            "right 10px bottom calc(20% + 1px)",
            "calc(50% - 2px) 25%",
        ] {
            assert!(
                matches!(parse(property_id::OBJECT_POSITION, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        assert!(matches!(
            parse(property_id::OBJECT_POSITION, "left 10px center"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn parses_background_position_extensions_and_axes() {
        let ParseOutcome::Parsed(value) = parse(property_id::BACKGROUND_POSITION, "left 10px center, right bottom")
        else {
            panic!("background-position should parse");
        };
        let StyleValueData::Shorthand {
            sub_properties, values, ..
        } = &*value
        else {
            panic!("background-position should expand to its axes");
        };
        assert_eq!(
            sub_properties.as_slice(),
            [property_id::BACKGROUND_POSITION_X, property_id::BACKGROUND_POSITION_Y]
        );
        assert_eq!(values.as_slice().len(), 2);

        for (property, source) in [
            (property_id::BACKGROUND_POSITION_X, "left 10px, center"),
            (property_id::BACKGROUND_POSITION_Y, "bottom 20%, top"),
        ] {
            assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
        }
    }

    #[test]
    fn parses_coordinating_position_lists() {
        let ParseOutcome::Parsed(value) = parse(property_id::MASK_POSITION, "left top, 25% 75%") else {
            panic!("mask-position should parse");
        };
        let StyleValueData::ValueList { values, separator, .. } = &*value else {
            panic!("mask-position should produce a comma-separated list");
        };
        assert_eq!(*separator, 1);
        assert_eq!(values.as_slice().len(), 2);
    }

    #[test]
    fn parses_border_radius_properties() {
        for source in ["10px", "10px 20%", "10px / 20%"] {
            assert!(
                matches!(
                    parse_geometry(property_id::BORDER_TOP_LEFT_RADIUS, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        let ParseOutcome::Parsed(value) = parse_geometry(property_id::BORDER_RADIUS, "1px 2px 3px / 4px 5px") else {
            panic!("border-radius should parse");
        };
        assert!(matches!(&*value, StyleValueData::Shorthand { values, .. } if values.as_slice().len() == 4));
        assert!(matches!(
            parse_geometry(property_id::BORDER_RADIUS, "-1px"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn parses_corner_shapes() {
        for source in ["round", "squircle", "superellipse(2)", "superellipse(-infinity)"] {
            assert!(
                matches!(
                    parse_geometry(property_id::CORNER_TOP_LEFT_SHAPE, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        assert!(matches!(
            parse_geometry(property_id::CORNER_SHAPE, "round scoop bevel notch"),
            ParseOutcome::Parsed(_)
        ));
    }

    #[test]
    fn parses_rect_and_basic_shapes() {
        assert!(matches!(
            parse_geometry(property_id::CLIP, "rect(auto, 10px, 20px, 0)"),
            ParseOutcome::Parsed(_)
        ));
        for (source, expected_kind) in [
            ("inset(1px 2% round 3px / 4px)", 0),
            ("xywh(1px 2px 30% 40% round 5px)", 1),
            ("rect(auto 2px 30% auto)", 2),
            ("circle(closest-side at right 2px bottom 3px)", 3),
            ("ellipse(10px 20% at center)", 4),
            ("polygon(evenodd, 0 0, 100% 0, 50% 100%)", 5),
            ("path(evenodd, \"M 0 0 L 10 10\")", 6),
        ] {
            let ParseOutcome::Parsed(value) = parse_geometry(property_id::CLIP_PATH, source) else {
                panic!("basic shape should parse: {source}");
            };
            assert!(matches!(&*value, StyleValueData::BasicShape { kind, .. } if *kind == expected_kind));
        }
        assert!(matches!(
            parse_geometry(property_id::CLIP_PATH, "shape(from 0 0, close)"),
            ParseOutcome::Invalid
        ));
    }

    #[test]
    fn parses_shape_outside_combinations() {
        for source in ["none", "margin-box", "circle() border-box", "content-box inset(10px)"] {
            assert!(
                matches!(
                    parse_geometry(property_id::SHAPE_OUTSIDE, source),
                    ParseOutcome::Parsed(_)
                ),
                "{source}"
            );
        }
        assert!(matches!(
            parse_geometry(property_id::SHAPE_OUTSIDE, "url(shape.png)"),
            ParseOutcome::NotHandled
        ));
    }

    #[test]
    fn parses_anchor_functions_and_insets() {
        for source in [
            "anchor(top)",
            "anchor(--card 25%, 10px)",
            "anchor(left, anchor(--fallback right))",
        ] {
            assert!(
                matches!(parse_anchor_fit(property_id::TOP, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        assert!(matches!(
            parse_anchor_fit(property_id::TOP, "anchor(10px)"),
            ParseOutcome::Invalid
        ));
        assert!(matches!(
            parse_anchor_fit(property_id::TOP, "calc(anchor(top, 1px) + 2px)"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse_anchor_fit(property_id::TOP, "anchor(--foo top, calc(0.5 * anchor(--bar bottom)))"),
            ParseOutcome::Parsed(_)
        ));
        assert!(matches!(
            parse_anchor_fit(property_id::INSET, "auto anchor(right) 10px 20%"),
            ParseOutcome::Parsed(_)
        ));
    }

    #[test]
    fn parses_anchor_size_and_fit_content_functions() {
        for (property, source) in [
            (property_id::WIDTH, "anchor-size()"),
            (property_id::WIDTH, "anchor-size(--card width, 10px)"),
            (property_id::WIDTH, "anchor-size(anchor-size(20%))"),
            (property_id::WIDTH, "fit-content(20%)"),
            (property_id::WIDTH, "fit-content"),
            (property_id::MARGIN, "anchor-size(width) 10px"),
        ] {
            assert!(
                matches!(parse_anchor_fit(property, source), ParseOutcome::Parsed(_)),
                "{source}"
            );
        }
        for source in ["anchor-size(--foo width,)", "anchor-size(--foo,)"] {
            assert!(matches!(
                parse_anchor_fit(property_id::WIDTH, source),
                ParseOutcome::Invalid
            ));
        }
        assert!(matches!(
            parse_anchor_fit(property_id::WIDTH, "auto"),
            ParseOutcome::NotHandled
        ));
        assert!(matches!(
            parse_anchor_fit(property_id::COLOR, "anchor-size()"),
            ParseOutcome::NotHandled
        ));
    }
}
