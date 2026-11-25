/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Parsing for the shorthands of CSS Gap Decorations: https://drafts.csswg.org/css-gaps-1/

use super::value_parser::{
    NumericRange, ParseContext, ParseOutcome, equals_ascii_case_insensitive, is_css_wide_keyword, parse_css_value,
    parse_initial_longhand, parse_integer_from_stream, shorthand_value, value_list,
};
use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::css_string::CssString;
use crate::css::parser::component_value::ComponentValue;
use crate::css::parser::token_stream::TokenStream;
use crate::css::property_metadata::{longhands_for_shorthand, property_id};
use crate::css::style_value::{
    FunctionPayload, RetainedStyleValueData, StyleValueBox, StyleValueData, shared_style_value,
};

const VALUE_LIST_SEPARATOR_COMMA: u8 = 1;

fn is_line_value_list_property(property: u16) -> bool {
    matches!(
        property,
        property_id::COLUMN_RULE_COLOR
            | property_id::COLUMN_RULE_STYLE
            | property_id::COLUMN_RULE_WIDTH
            | property_id::ROW_RULE_COLOR
            | property_id::ROW_RULE_STYLE
            | property_id::ROW_RULE_WIDTH
    )
}

fn is_repeat_function(value: &ComponentValue) -> bool {
    value
        .function()
        .is_some_and(|(name, _)| equals_ascii_case_insensitive(name, b"repeat"))
}

/// A comma-separated list of values, as the values of a repeat() and the lists of gap decoration properties hold them.
pub(crate) fn comma_list(values: Vec<StyleValueData>) -> StyleValueData {
    value_list(values, VALUE_LIST_SEPARATOR_COMMA, false)
}

/// A repeat() of `values`, `count` times or auto-repeated for a count of `auto`.
pub(crate) fn repeat_function(count: StyleValueData, values: Vec<StyleValueData>) -> StyleValueData {
    StyleValueData::Function(StyleValueBox::new(FunctionPayload {
        name: CssString::from_utf16(&"repeat".encode_utf16().collect::<Vec<_>>()),
        value: RetainedStyleValueData::from_owned(comma_list(std::iter::once(count).chain(values).collect())),
    }))
}

/// The items of a comma-separated list, each a list of the non-whitespace component values between the commas, or
/// None when an item is empty.
fn comma_separated_items(values: &[ComponentValue]) -> Option<Vec<Vec<&ComponentValue>>> {
    values
        .split(ComponentValue::is_comma)
        .map(|item| {
            let item: Vec<&ComponentValue> = item.iter().filter(|value| !value.is_whitespace()).collect();
            (!item.is_empty()).then_some(item)
        })
        .collect()
}

/// A repeat() of gap decoration values whose arguments are `arguments`, along with whether it is an auto repeater.
/// `parse_values` parses the values that follow the repeat count.
pub(crate) fn parse_repeat<T>(
    context: &ParseContext,
    property: u16,
    arguments: &[ComponentValue],
    mut parse_values: impl FnMut(&[Vec<&ComponentValue>]) -> Option<T>,
) -> Option<(StyleValueData, T)> {
    let first_comma = arguments.iter().position(ComponentValue::is_comma)?;
    let mut count_tokens = TokenStream::new(&arguments[..first_comma]);
    count_tokens.discard_whitespace();
    let count = if count_tokens
        .next_token()
        .ident()
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"auto"))
    {
        count_tokens.discard_a_token();
        StyleValueData::Keyword { keyword: keyword::AUTO }
    } else {
        parse_integer_from_stream(
            context,
            property,
            &mut count_tokens,
            NumericRange::new(1.0, i32::MAX.into()),
        )?
    };
    count_tokens.discard_whitespace();
    if count_tokens.has_next_token() {
        return None;
    }
    let values = parse_values(&comma_separated_items(&arguments[first_comma + 1..])?)?;
    Some((count, values))
}

fn is_css_wide_keyword_value(value: &ComponentValue) -> bool {
    value
        .ident()
        .and_then(keyword_from_ascii_case_insensitive)
        .is_some_and(is_css_wide_keyword)
}

pub(crate) fn is_auto_repeat_count(count: &StyleValueData) -> bool {
    matches!(count, StyleValueData::Keyword { keyword: value } if *value == keyword::AUTO)
}

/// https://drafts.csswg.org/css-gaps-1/#typedef-line-color-list and the corresponding width and style lists, along with
/// their auto variants. A single value without a repeat() is left to the property's ordinary parsing.
pub(crate) fn parse_gap_decorations_longhand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if !is_line_value_list_property(property) {
        return ParseOutcome::NotHandled;
    }
    if !values.iter().any(|value| value.is_comma() || is_repeat_function(value)) {
        return ParseOutcome::NotHandled;
    }
    let parse_single = |value: &ComponentValue| {
        if is_css_wide_keyword_value(value) || is_repeat_function(value) {
            return None;
        }
        match parse_css_value(context, property, std::slice::from_ref(value)) {
            ParseOutcome::Parsed(value) => Some((*value).clone()),
            ParseOutcome::Invalid | ParseOutcome::NotHandled => None,
        }
    };
    let parse_values = |items: &[Vec<&ComponentValue>]| {
        items
            .iter()
            .map(|item| match item.as_slice() {
                [value] => parse_single(value),
                _ => None,
            })
            .collect::<Option<Vec<_>>>()
    };

    let Some(items) = comma_separated_items(values) else {
        return ParseOutcome::Invalid;
    };
    let mut has_auto_repeat = false;
    let mut parsed = Vec::with_capacity(items.len());
    for item in items {
        let [value] = item.as_slice() else {
            return ParseOutcome::Invalid;
        };
        let parsed_item = match value.function() {
            Some((name, arguments)) if equals_ascii_case_insensitive(name, b"repeat") => {
                let Some((count, values)) = parse_repeat(context, property, arguments, parse_values) else {
                    return ParseOutcome::Invalid;
                };
                if is_auto_repeat_count(&count) {
                    if has_auto_repeat {
                        return ParseOutcome::Invalid;
                    }
                    has_auto_repeat = true;
                }
                repeat_function(count, values)
            }
            _ => match parse_single(value) {
                Some(value) => value,
                None => return ParseOutcome::Invalid,
            },
        };
        parsed.push(parsed_item);
    }
    ParseOutcome::Parsed(shared_style_value(comma_list(parsed)))
}

/// How a gap decoration shorthand maps the values it is given onto its longhands.
enum ShorthandForm {
    /// One value that every longhand takes.
    Single,
    /// A value of the longhands, which may span several component values, that every longhand takes.
    Whole,
    /// `<inset-value> <inset-value>?`, the start and end of each axis the longhands cover.
    StartEnd,
    /// `<inset-value>{1,2} [ / <inset-value>{1,2} ]?`, the cap start and end, then the junction start and end of each
    /// axis the longhands cover.
    CapJunction,
}

fn shorthand_form(property: u16) -> Option<ShorthandForm> {
    use property_id::*;
    Some(match property {
        RULE_BREAK
        | RULE_VISIBILITY_ITEMS
        | COLUMN_RULE_INSET_START
        | COLUMN_RULE_INSET_END
        | ROW_RULE_INSET_START
        | ROW_RULE_INSET_END
        | RULE_INSET_START
        | RULE_INSET_END => ShorthandForm::Single,
        COLUMN_RULE_INSET_CAP
        | COLUMN_RULE_INSET_JUNCTION
        | ROW_RULE_INSET_CAP
        | ROW_RULE_INSET_JUNCTION
        | RULE_INSET_CAP
        | RULE_INSET_JUNCTION => ShorthandForm::StartEnd,
        COLUMN_RULE_INSET | ROW_RULE_INSET | RULE_INSET => ShorthandForm::CapJunction,
        RULE_COLOR | RULE_STYLE | RULE_WIDTH => ShorthandForm::Whole,
        _ => return None,
    })
}

/// How many longhands of each axis the gap decoration shorthand `property` covers, or None for other properties.
pub(crate) fn shorthand_values_per_axis(property: u16) -> Option<usize> {
    Some(match shorthand_form(property)? {
        ShorthandForm::Single | ShorthandForm::Whole => 1,
        ShorthandForm::StartEnd => 2,
        ShorthandForm::CapJunction => 4,
    })
}

/// The width, style and color of a <gap-rule>, the longhands in the order the rule shorthands list them.
type GapRule = [StyleValueData; 3];

/// https://drafts.csswg.org/css-gaps-1/#typedef-gap-rule
/// <gap-rule> = [ <line-width> || <line-style> || <line-color> ]
fn parse_gap_rule(context: &ParseContext, longhands: &[u16], values: &[&ComponentValue]) -> Option<GapRule> {
    if values.is_empty() {
        return None;
    }
    let mut parsed: [Option<StyleValueData>; 3] = Default::default();
    for value in values {
        if is_css_wide_keyword_value(value) || is_repeat_function(value) {
            return None;
        }
        let slot = (0..3).find(|&index| {
            parsed[index].is_none()
                && match parse_css_value(context, longhands[index], std::slice::from_ref(*value)) {
                    ParseOutcome::Parsed(value) => {
                        parsed[index] = Some((*value).clone());
                        true
                    }
                    ParseOutcome::Invalid | ParseOutcome::NotHandled => false,
                }
        });
        slot?;
    }
    let [width, style, color] = parsed;
    let initial = |index: usize| parse_initial_longhand(context, longhands[index]);
    Some([
        width.or_else(|| initial(0))?,
        style.or_else(|| initial(1))?,
        color.or_else(|| initial(2))?,
    ])
}

/// https://drafts.csswg.org/css-gaps-1/#propdef-column-rule
/// <gap-rule-list> | <gap-auto-rule-list>
fn parse_gap_rule_shorthand(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let longhands = longhands_for_shorthand(property);
    let Some(items) = comma_separated_items(values) else {
        return ParseOutcome::Invalid;
    };
    let single_item = items.len() == 1;
    let mut has_auto_repeat = false;
    let mut lists: [Vec<StyleValueData>; 3] = Default::default();
    for item in items {
        match item.as_slice() {
            [value] if is_repeat_function(value) => {
                let (_, arguments) = value.function().expect("repeat() is a function");
                let parse_rules = |items: &[Vec<&ComponentValue>]| {
                    items
                        .iter()
                        .map(|item| parse_gap_rule(context, longhands, item))
                        .collect::<Option<Vec<_>>>()
                };
                let Some((count, rules)) = parse_repeat(context, longhands[0], arguments, parse_rules) else {
                    return ParseOutcome::Invalid;
                };
                if is_auto_repeat_count(&count) {
                    if has_auto_repeat {
                        return ParseOutcome::Invalid;
                    }
                    has_auto_repeat = true;
                }
                for (index, list) in lists.iter_mut().enumerate() {
                    let values = rules.iter().map(|rule| rule[index].clone()).collect();
                    list.push(repeat_function(count.clone(), values));
                }
            }
            item => {
                let Some(rule) = parse_gap_rule(context, longhands, item) else {
                    return ParseOutcome::Invalid;
                };
                for (list, value) in lists.iter_mut().zip(rule) {
                    list.push(value);
                }
            }
        }
    }
    let per_axis: Vec<StyleValueData> = lists
        .into_iter()
        .map(|mut list| {
            if single_item && !matches!(list[0], StyleValueData::Function(_)) {
                list.pop().expect("a gap rule list has an item")
            } else {
                comma_list(list)
            }
        })
        .collect();
    let expanded = per_axis.iter().cycle().take(longhands.len()).cloned().collect();
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property,
        longhands.to_vec(),
        expanded,
    )))
}

pub(crate) fn parse_gap_decorations_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if matches!(
        property,
        property_id::COLUMN_RULE | property_id::ROW_RULE | property_id::RULE
    ) {
        return parse_gap_rule_shorthand(context, property, values);
    }
    let Some(form) = shorthand_form(property) else {
        return ParseOutcome::NotHandled;
    };
    let longhands = longhands_for_shorthand(property);
    let representative_longhand = longhands[0];
    let parse_value = |value: &ComponentValue| {
        if is_css_wide_keyword_value(value) {
            return None;
        }
        match parse_css_value(context, representative_longhand, std::slice::from_ref(value)) {
            ParseOutcome::Parsed(value) => Some((*value).clone()),
            ParseOutcome::Invalid | ParseOutcome::NotHandled => None,
        }
    };
    let parse_pair = |values: &[&ComponentValue]| -> Option<[StyleValueData; 2]> {
        match values {
            [value] => {
                let value = parse_value(value)?;
                Some([value.clone(), value])
            }
            [start, end] => Some([parse_value(start)?, parse_value(end)?]),
            _ => None,
        }
    };

    let component_values = values;
    let values: Vec<&ComponentValue> = values.iter().filter(|value| !value.is_whitespace()).collect();
    // Each axis's values, in the order the longhands list them.
    let per_axis = match form {
        ShorthandForm::Single => {
            let [value] = values.as_slice() else {
                return ParseOutcome::Invalid;
            };
            let Some(value) = parse_value(value) else {
                return ParseOutcome::Invalid;
            };
            vec![value; longhands.len()]
        }
        ShorthandForm::Whole => {
            let value = match parse_css_value(context, representative_longhand, component_values) {
                ParseOutcome::Parsed(value) => (*value).clone(),
                ParseOutcome::Invalid | ParseOutcome::NotHandled => return ParseOutcome::Invalid,
            };
            vec![value; longhands.len()]
        }
        ShorthandForm::StartEnd => {
            let Some(pair) = parse_pair(&values) else {
                return ParseOutcome::Invalid;
            };
            pair.to_vec()
        }
        ShorthandForm::CapJunction => {
            let mut groups = values.split(|value| value.is_delim(b'/'));
            let Some(cap) = groups.next().and_then(parse_pair) else {
                return ParseOutcome::Invalid;
            };
            let junction = match groups.next() {
                Some(junction) => match parse_pair(junction) {
                    Some(junction) => junction,
                    None => return ParseOutcome::Invalid,
                },
                None => cap.clone(),
            };
            if groups.next().is_some() {
                return ParseOutcome::Invalid;
            }
            cap.into_iter().chain(junction).collect()
        }
    };
    let expanded = per_axis.iter().cycle().take(longhands.len()).cloned().collect();
    ParseOutcome::Parsed(shared_style_value(shorthand_value(
        property,
        longhands.to_vec(),
        expanded,
    )))
}
