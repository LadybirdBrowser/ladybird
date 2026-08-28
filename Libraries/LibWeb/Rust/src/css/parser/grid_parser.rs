/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use std::collections::HashMap;
use std::sync::Arc;

use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::math_functions::math_function_from_name;
use crate::css::parser::component_value::ComponentValue;
use crate::css::parser::token_stream::TokenStream;
use crate::css::parser::value_parser::{
    NumericRange, ParseContext, ParseOutcome, VALUE_TYPE_FLEX, equals_ascii_case_insensitive,
    is_arbitrary_substitution_function, is_valid_custom_ident, parse_calculated_numeric_value_with_ranges,
    parse_flex_value, parse_integer_from_stream, parse_length_percentage_from_stream, parse_tree_counting_value,
    retain_fly_string,
};
use crate::css::property_metadata::property_id;
use crate::css::style_value::{
    RetainedGridArea, RetainedGridAreaList, RetainedGridTrackEntry, RetainedGridTrackEntryList, RetainedPropertyIdList,
    RetainedStyleValueData, RetainedStyleValueDataList, StyleValueData,
};

const GRID_REPEAT_AUTO_FIT: u8 = 0;
const GRID_REPEAT_AUTO_FILL: u8 = 1;
const GRID_REPEAT_FIXED: u8 = 2;

fn grid_track_list(
    is_subgrid: bool,
    preserve_line_name_sets: bool,
    entries: Vec<RetainedGridTrackEntry>,
) -> StyleValueData {
    StyleValueData::GridTrackSizeList {
        is_subgrid,
        preserve_line_name_sets,
        entries: RetainedGridTrackEntryList::from_retained_entries(entries),
    }
}

fn parse_keyword(value: &ComponentValue, expected: u16) -> Option<StyleValueData> {
    (keyword_from_ascii_case_insensitive(value.ident()?) == Some(expected))
        .then_some(StyleValueData::Keyword { keyword: expected })
}

fn parse_grid_fixed_breadth(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    parse_length_percentage_from_stream(
        context,
        property,
        tokens,
        NumericRange::NON_NEGATIVE,
        NumericRange::NON_NEGATIVE,
    )
}

// https://drafts.csswg.org/css-grid-2/#typedef-inflexible-breadth
// <inflexible-breadth> = <length-percentage [0,∞]> | min-content | max-content | auto
fn parse_grid_inflexible_breadth(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    let start = tokens.position;
    if let Some(value) = parse_grid_fixed_breadth(context, property, tokens) {
        return Some(value);
    }
    tokens.position = start;
    tokens.discard_whitespace();
    let value = parse_keyword(tokens.next_token(), keyword::MAX_CONTENT)
        .or_else(|| parse_keyword(tokens.next_token(), keyword::MIN_CONTENT))
        .or_else(|| parse_keyword(tokens.next_token(), keyword::AUTO))?;
    tokens.discard_a_token();
    Some(value)
}

// https://drafts.csswg.org/css-grid-2/#typedef-track-breadth
// <track-breadth> = <length-percentage [0,∞]> | <flex [0,∞]> | min-content | max-content | auto
fn parse_grid_track_breadth(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<StyleValueData> {
    let start = tokens.position;
    if let Some(value) = parse_grid_inflexible_breadth(context, property, tokens) {
        return Some(value);
    }
    tokens.position = start;
    tokens.discard_whitespace();
    let value = tokens.next_token();
    let parsed = if let Some((name, values)) = value.function()
        && math_function_from_name(name).is_some()
    {
        parse_calculated_numeric_value_with_ranges(
            context,
            property,
            VALUE_TYPE_FLEX,
            None,
            NumericRange::NON_NEGATIVE,
            name,
            values,
        )
    } else {
        parse_flex_value(value, NumericRange::NON_NEGATIVE)
    }?;
    tokens.discard_a_token();
    Some(parsed)
}

// https://drafts.csswg.org/css-grid-2/#typedef-line-names
// <line-names> = '[' <custom-ident>* ']'
fn parse_grid_line_names(
    context: &ParseContext,
    tokens: &mut TokenStream<'_>,
) -> Option<Vec<crate::css::retained_fly_string::RetainedUtf16FlyString>> {
    tokens.discard_whitespace();
    let Some(block) = tokens.next_token().square_block() else {
        return Some(Vec::new());
    };
    let mut block_tokens = TokenStream::new(block);
    let mut names = Vec::new();
    block_tokens.discard_whitespace();
    while block_tokens.has_next_token() {
        let identifier = block_tokens.next_token().ident()?;
        if !is_valid_custom_ident(identifier, &["span", "auto"]) {
            return None;
        }
        names.push(retain_fly_string(context, identifier)?);
        block_tokens.discard_a_token();
        block_tokens.discard_whitespace();
    }
    tokens.discard_a_token();
    Some(names)
}

#[derive(Clone, Copy)]
enum TrackGrammar {
    TrackSize,
    FixedSize,
    TrackSizeOrRepeat,
    FixedSizeOrRepeat,
}

#[derive(Clone, Copy)]
enum BreadthGrammar {
    Fixed,
    Inflexible,
    Track,
}

fn parse_grid_minmax(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    min_grammar: BreadthGrammar,
    max_grammar: BreadthGrammar,
) -> Option<RetainedGridTrackEntry> {
    tokens.discard_whitespace();
    let (name, values) = tokens.next_token().function()?;
    if !equals_ascii_case_insensitive(name, b"minmax") {
        return None;
    }
    let arguments = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.len() != 2 {
        return None;
    }
    let parse_parameter = |values: &[ComponentValue], grammar: BreadthGrammar| {
        let mut stream = TokenStream::new(values);
        let value = match grammar {
            BreadthGrammar::Fixed => parse_grid_fixed_breadth(context, property, &mut stream),
            BreadthGrammar::Inflexible => parse_grid_inflexible_breadth(context, property, &mut stream),
            BreadthGrammar::Track => parse_grid_track_breadth(context, property, &mut stream),
        }?;
        stream.discard_whitespace();
        stream.is_empty().then_some(value)
    };
    let min = parse_parameter(arguments[0], min_grammar)?;
    let max = parse_parameter(arguments[1], max_grammar)?;
    tokens.discard_a_token();
    Some(RetainedGridTrackEntry::minmax(min, max))
}

// https://drafts.csswg.org/css-grid-2/#typedef-track-size
// <track-size> = <track-breadth> | minmax( <inflexible-breadth> , <track-breadth> ) |
//                fit-content( <length-percentage [0,∞]> )
fn parse_grid_track_size(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<RetainedGridTrackEntry> {
    let start = tokens.position;
    tokens.discard_whitespace();
    if let Some((name, values)) = tokens.next_token().function() {
        if equals_ascii_case_insensitive(name, b"minmax") {
            if let Some(value) = parse_grid_minmax(
                context,
                property,
                tokens,
                BreadthGrammar::Inflexible,
                BreadthGrammar::Track,
            ) {
                return Some(value);
            }
            tokens.position = start;
            return None;
        }
        if equals_ascii_case_insensitive(name, b"fit-content") {
            let mut function_tokens = TokenStream::new(values);
            let value = parse_grid_fixed_breadth(context, property, &mut function_tokens)?;
            function_tokens.discard_whitespace();
            if function_tokens.has_next_token() {
                tokens.position = start;
                return None;
            }
            let function = StyleValueData::Function {
                name: retain_fly_string(context, name)?,
                value: RetainedStyleValueData::from_owned(value),
            };
            tokens.discard_a_token();
            return Some(RetainedGridTrackEntry::size(function));
        }
    }
    tokens.position = start;
    let value = parse_grid_track_breadth(context, property, tokens)?;
    Some(RetainedGridTrackEntry::size(value))
}

// https://drafts.csswg.org/css-grid-2/#typedef-fixed-size
// <fixed-size> = <fixed-breadth> | minmax( <fixed-breadth> , <track-breadth> ) |
//                minmax( <inflexible-breadth> , <fixed-breadth> )
fn parse_grid_fixed_size(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<RetainedGridTrackEntry> {
    let start = tokens.position;
    tokens.discard_whitespace();
    if tokens
        .next_token()
        .function()
        .is_some_and(|(name, _)| equals_ascii_case_insensitive(name, b"minmax"))
    {
        if let Some(value) = parse_grid_minmax(context, property, tokens, BreadthGrammar::Fixed, BreadthGrammar::Track)
        {
            return Some(value);
        }
        tokens.position = start;
        if let Some(value) = parse_grid_minmax(
            context,
            property,
            tokens,
            BreadthGrammar::Inflexible,
            BreadthGrammar::Fixed,
        ) {
            return Some(value);
        }
        tokens.position = start;
        return None;
    }
    tokens.position = start;
    let value = parse_grid_fixed_breadth(context, property, tokens)?;
    Some(RetainedGridTrackEntry::size(value))
}

fn parse_track_list_impl(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    output: &mut Vec<RetainedGridTrackEntry>,
    grammar: TrackGrammar,
    allow_trailing_line_names_for_each_track: bool,
) -> usize {
    let mut parsed_track_count = 0;
    tokens.discard_whitespace();
    while tokens.has_next_token() {
        let start = tokens.position;
        let Some(line_names) = parse_grid_line_names(context, tokens) else {
            tokens.position = start;
            break;
        };
        tokens.discard_whitespace();
        let Some(track) = parse_track(context, property, tokens, grammar) else {
            tokens.position = start;
            break;
        };
        tokens.discard_whitespace();
        if !line_names.is_empty() {
            output.push(RetainedGridTrackEntry::line_names(line_names));
        }
        output.push(track);
        if allow_trailing_line_names_for_each_track
            && let Some(line_names) = parse_grid_line_names(context, tokens)
            && !line_names.is_empty()
        {
            output.push(RetainedGridTrackEntry::line_names(line_names));
        }
        parsed_track_count += 1;
        tokens.discard_whitespace();
    }
    if !allow_trailing_line_names_for_each_track {
        let start = tokens.position;
        match parse_grid_line_names(context, tokens) {
            Some(line_names) if !line_names.is_empty() => {
                output.push(RetainedGridTrackEntry::line_names(line_names));
            }
            Some(_) => {}
            None => tokens.position = start,
        }
    }
    parsed_track_count
}

fn parse_track(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    grammar: TrackGrammar,
) -> Option<RetainedGridTrackEntry> {
    let start = tokens.position;
    match grammar {
        TrackGrammar::TrackSize => parse_grid_track_size(context, property, tokens),
        TrackGrammar::FixedSize => parse_grid_fixed_size(context, property, tokens),
        TrackGrammar::TrackSizeOrRepeat => parse_grid_track_repeat(context, property, tokens).or_else(|| {
            tokens.position = start;
            parse_grid_track_size(context, property, tokens)
        }),
        TrackGrammar::FixedSizeOrRepeat => parse_grid_fixed_repeat(context, property, tokens).or_else(|| {
            tokens.position = start;
            parse_grid_fixed_size(context, property, tokens)
        }),
    }
}

fn parse_grid_track_repeat_impl(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
    allow_auto: bool,
    track_grammar: TrackGrammar,
) -> Option<RetainedGridTrackEntry> {
    tokens.discard_whitespace();
    let (name, values) = tokens.next_token().function()?;
    if !equals_ascii_case_insensitive(name, b"repeat") {
        return None;
    }
    let arguments = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.len() != 2 {
        return None;
    }
    let mut first = TokenStream::new(arguments[0]);
    first.discard_whitespace();
    let (repeat_type, repeat_count) = if allow_auto {
        let identifier = first.next_token().ident()?;
        let repeat_type = if equals_ascii_case_insensitive(identifier, b"auto-fill") {
            GRID_REPEAT_AUTO_FILL
        } else if equals_ascii_case_insensitive(identifier, b"auto-fit") {
            GRID_REPEAT_AUTO_FIT
        } else {
            return None;
        };
        first.discard_a_token();
        (repeat_type, None)
    } else {
        let count = parse_integer_from_stream(context, property, &mut first, NumericRange::new(1.0, i32::MAX.into()))?;
        (GRID_REPEAT_FIXED, Some(count))
    };
    first.discard_whitespace();
    if first.has_next_token() {
        return None;
    }
    let mut second = TokenStream::new(arguments[1]);
    let mut entries = Vec::new();
    if parse_track_list_impl(context, property, &mut second, &mut entries, track_grammar, false) == 0 {
        return None;
    }
    second.discard_whitespace();
    if second.has_next_token() {
        return None;
    }
    tokens.discard_a_token();
    Some(RetainedGridTrackEntry::repeat(
        repeat_type,
        repeat_count,
        false,
        false,
        entries,
    ))
}

// https://drafts.csswg.org/css-grid-2/#typedef-track-repeat
// <track-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <track-size> ]+ <line-names>? )
fn parse_grid_track_repeat(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<RetainedGridTrackEntry> {
    parse_grid_track_repeat_impl(context, property, tokens, false, TrackGrammar::TrackSize)
}

// https://drafts.csswg.org/css-grid-2/#typedef-auto-repeat
// <auto-repeat> = repeat( [ auto-fill | auto-fit ] , [ <line-names>? <fixed-size> ]+ <line-names>? )
fn parse_grid_auto_repeat(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<RetainedGridTrackEntry> {
    parse_grid_track_repeat_impl(context, property, tokens, true, TrackGrammar::FixedSize)
}

fn parse_grid_fixed_repeat(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<RetainedGridTrackEntry> {
    parse_grid_track_repeat_impl(context, property, tokens, false, TrackGrammar::FixedSize)
}

// https://drafts.csswg.org/css-grid-2/#typedef-name-repeat
// <name-repeat> = repeat( [ <integer [1,∞]> | auto-fill ] , <line-names>+ )
fn parse_grid_name_repeat(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<RetainedGridTrackEntry> {
    tokens.discard_whitespace();
    let (name, values) = tokens.next_token().function()?;
    if !equals_ascii_case_insensitive(name, b"repeat") {
        return None;
    }
    let arguments = values.split(ComponentValue::is_comma).collect::<Vec<_>>();
    if arguments.len() != 2 {
        return None;
    }
    let mut first = TokenStream::new(arguments[0]);
    first.discard_whitespace();
    let (repeat_type, repeat_count) = if first
        .next_token()
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"auto-fill"))
    {
        first.discard_a_token();
        (GRID_REPEAT_AUTO_FILL, None)
    } else {
        let count = parse_integer_from_stream(context, property, &mut first, NumericRange::new(1.0, i32::MAX.into()))?;
        (GRID_REPEAT_FIXED, Some(count))
    };
    first.discard_whitespace();
    if first.has_next_token() {
        return None;
    }
    let mut second = TokenStream::new(arguments[1]);
    let mut entries = Vec::new();
    second.discard_whitespace();
    while second.has_next_token() {
        second.next_token().square_block()?;
        entries.push(RetainedGridTrackEntry::line_names(parse_grid_line_names(
            context,
            &mut second,
        )?));
        second.discard_whitespace();
    }
    if entries.is_empty() {
        return None;
    }
    tokens.discard_a_token();
    Some(RetainedGridTrackEntry::repeat(
        repeat_type,
        repeat_count,
        false,
        true,
        entries,
    ))
}

// https://drafts.csswg.org/css-grid-2/#typedef-track-list
// <track-list> = [ <line-names>? [ <track-size> | <track-repeat> ] ]+ <line-names>?
fn parse_grid_track_list(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<Vec<RetainedGridTrackEntry>> {
    let start = tokens.position;
    let mut entries = Vec::new();
    if parse_track_list_impl(
        context,
        property,
        tokens,
        &mut entries,
        TrackGrammar::TrackSizeOrRepeat,
        false,
    ) == 0
    {
        tokens.position = start;
        return None;
    }
    Some(entries)
}

// https://drafts.csswg.org/css-grid-2/#typedef-auto-track-list
fn parse_grid_auto_track_list(
    context: &ParseContext,
    property: u16,
    tokens: &mut TokenStream<'_>,
) -> Option<Vec<RetainedGridTrackEntry>> {
    let start = tokens.position;
    let mut entries = Vec::new();
    let parsed_track_count = parse_track_list_impl(
        context,
        property,
        tokens,
        &mut entries,
        TrackGrammar::FixedSizeOrRepeat,
        false,
    );
    tokens.discard_whitespace();
    if !tokens.has_next_token() {
        if parsed_track_count == 0 {
            tokens.position = start;
            return None;
        }
        return Some(entries);
    }
    let Some(auto_repeat) = parse_grid_auto_repeat(context, property, tokens) else {
        tokens.position = start;
        return None;
    };
    entries.push(auto_repeat);
    parse_track_list_impl(
        context,
        property,
        tokens,
        &mut entries,
        TrackGrammar::FixedSizeOrRepeat,
        false,
    );
    Some(entries)
}

// https://drafts.csswg.org/css-grid-2/#track-sizing
// none | <track-list> | <auto-track-list> | subgrid <line-name-list>?
fn parse_grid_track_size_list(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    if parse_keyword(tokens.next_token(), keyword::NONE).is_some() {
        tokens.discard_a_token();
        tokens.discard_whitespace();
        return tokens.is_empty().then(|| grid_track_list(false, false, Vec::new()));
    }
    if tokens
        .next_token()
        .ident()
        .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"subgrid"))
    {
        tokens.discard_a_token();
        let mut entries = Vec::new();
        let mut has_auto_fill_repeat = false;
        tokens.discard_whitespace();
        while tokens.has_next_token() {
            if tokens.next_token().square_block().is_some() {
                entries.push(RetainedGridTrackEntry::line_names(parse_grid_line_names(
                    context,
                    &mut tokens,
                )?));
            } else {
                let repeat = parse_grid_name_repeat(context, property, &mut tokens)?;
                if repeat.repeat_type == GRID_REPEAT_AUTO_FILL {
                    if has_auto_fill_repeat {
                        return None;
                    }
                    has_auto_fill_repeat = true;
                }
                entries.push(repeat);
            }
            tokens.discard_whitespace();
        }
        return Some(grid_track_list(true, true, entries));
    }
    let start = tokens.position;
    if let Some(entries) = parse_grid_auto_track_list(context, property, &mut tokens) {
        tokens.discard_whitespace();
        if tokens.is_empty() {
            return Some(grid_track_list(false, false, entries));
        }
    }
    tokens.position = start;
    let entries = parse_grid_track_list(context, property, &mut tokens)?;
    tokens.discard_whitespace();
    tokens.is_empty().then(|| grid_track_list(false, false, entries))
}

fn parse_grid_auto_track_sizes(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    let mut entries = Vec::new();
    tokens.discard_whitespace();
    while tokens.has_next_token() {
        entries.push(parse_grid_track_size(context, property, &mut tokens)?);
        tokens.discard_whitespace();
    }
    (!entries.is_empty()).then(|| grid_track_list(false, false, entries))
}

fn implicit_grid_line_name(name: &[u16], suffix: &[u8]) -> Vec<u16> {
    let mut result = name.to_vec();
    result.extend(suffix.iter().copied().map(u16::from));
    result
}

// https://drafts.csswg.org/css-grid-2/#line-placement
// <grid-line> = auto | <custom-ident> | [ [ <integer [-∞,-1]> | <integer [1,∞]> ] &&
//               <custom-ident>? ] | [ span && [ <integer [1,∞]> || <custom-ident> ] ]
fn parse_grid_track_placement(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    if parse_keyword(tokens.next_token(), keyword::AUTO).is_some() {
        tokens.discard_a_token();
        tokens.discard_whitespace();
        if tokens.is_empty() {
            return Some(StyleValueData::GridTrackPlacement {
                kind: 0,
                value: RetainedStyleValueData::none(),
                has_name: false,
                name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
                implicit_start_name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
                implicit_end_name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
            });
        }
        return None;
    }

    let mut is_span = false;
    let mut parsed_name: Option<Vec<u16>> = None;
    let mut parsed_integer = None;
    while tokens.has_next_token() {
        if tokens
            .next_token()
            .ident()
            .is_some_and(|identifier| equals_ascii_case_insensitive(identifier, b"span"))
        {
            if is_span {
                return None;
            }
            tokens.discard_a_token();
            // NB: span may follow the integer or name only when it is the final token, not when it
            //     separates the two.
            if tokens.has_next_token() && (parsed_name.is_some() || parsed_integer.is_some()) {
                return None;
            }
            is_span = true;
            tokens.discard_whitespace();
            continue;
        }
        if let Some(identifier) = tokens.next_token().ident()
            && is_valid_custom_ident(identifier, &["auto"])
        {
            if parsed_name.is_some() {
                return None;
            }
            parsed_name = Some(identifier.to_vec());
            tokens.discard_a_token();
            tokens.discard_whitespace();
            continue;
        }
        if parsed_integer.is_none()
            && let Some(integer) = parse_tree_counting_value(context, tokens.next_token(), 1)
        {
            parsed_integer = Some(integer);
            tokens.discard_a_token();
            tokens.discard_whitespace();
            continue;
        }
        if parsed_integer.is_none()
            && let Some(integer) = parse_integer_from_stream(context, property, &mut tokens, NumericRange::INFINITE)
        {
            parsed_integer = Some(integer);
            tokens.discard_whitespace();
            continue;
        }
        return None;
    }

    let has_name = parsed_name.is_some();
    let name = match parsed_name.as_deref() {
        Some(name) => Some(retain_fly_string(context, name)?),
        None => None,
    };
    let implicit_start_name = if !is_span {
        match parsed_name.as_deref() {
            Some(name) => Some(retain_fly_string(context, &implicit_grid_line_name(name, b"-start"))?),
            None => None,
        }
    } else {
        None
    };
    let implicit_end_name = if !is_span {
        match parsed_name.as_deref() {
            Some(name) => Some(retain_fly_string(context, &implicit_grid_line_name(name, b"-end"))?),
            None => None,
        }
    } else {
        None
    };
    if !is_span
        && (parsed_integer.is_some() || has_name)
        && !matches!(parsed_integer, Some(StyleValueData::Integer { value: 0 }))
    {
        return Some(StyleValueData::GridTrackPlacement {
            kind: 2,
            value: parsed_integer.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
            has_name,
            name: name.unwrap_or_else(crate::css::retained_fly_string::RetainedUtf16FlyString::none),
            implicit_start_name: implicit_start_name
                .unwrap_or_else(crate::css::retained_fly_string::RetainedUtf16FlyString::none),
            implicit_end_name: implicit_end_name
                .unwrap_or_else(crate::css::retained_fly_string::RetainedUtf16FlyString::none),
        });
    }
    if is_span
        && (parsed_integer.is_some() || has_name)
        && !matches!(parsed_integer, Some(StyleValueData::Integer { value }) if value <= 0)
    {
        return Some(StyleValueData::GridTrackPlacement {
            kind: 1,
            value: RetainedStyleValueData::from_owned(parsed_integer.unwrap_or(StyleValueData::Integer { value: 1 })),
            has_name,
            name: name.unwrap_or_else(crate::css::retained_fly_string::RetainedUtf16FlyString::none),
            implicit_start_name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
            implicit_end_name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
        });
    }
    None
}

fn auto_grid_track_placement() -> StyleValueData {
    StyleValueData::GridTrackPlacement {
        kind: 0,
        value: RetainedStyleValueData::none(),
        has_name: false,
        name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
        implicit_start_name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
        implicit_end_name: crate::css::retained_fly_string::RetainedUtf16FlyString::none(),
    }
}

fn is_custom_ident_placement(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::GridTrackPlacement {
            kind: 2,
            value,
            has_name: true,
            ..
        } if value.optional_data().is_none()
    )
}

fn grid_shorthand(property: u16, sub_properties: Vec<u16>, values: Vec<StyleValueData>) -> ParseOutcome {
    ParseOutcome::Parsed(Arc::new(StyleValueData::Shorthand {
        shorthand_property: property,
        sub_properties: RetainedPropertyIdList::from_property_ids(sub_properties),
        values: RetainedStyleValueDataList::from_retained_values(
            values.into_iter().map(RetainedStyleValueData::from_owned).collect(),
        ),
    }))
}

fn slash_separated_components(values: &[ComponentValue]) -> Option<Vec<&[ComponentValue]>> {
    let mut components = Vec::new();
    let mut start = 0;
    for (index, value) in values.iter().enumerate() {
        if value.is_delim(b'/') {
            components.push(&values[start..index]);
            start = index + 1;
        }
    }
    components.push(&values[start..]);
    components
        .iter()
        .all(|component| component.iter().any(|value| !value.is_whitespace()))
        .then_some(components)
}

fn parse_grid_track_placement_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
) -> ParseOutcome {
    if !matches!(property, property_id::GRID_COLUMN | property_id::GRID_ROW) {
        return ParseOutcome::NotHandled;
    }
    let Some(components) = slash_separated_components(values).filter(|components| components.len() <= 2) else {
        return ParseOutcome::Invalid;
    };
    let Some(start) = parse_grid_track_placement(context, property, components[0]) else {
        return ParseOutcome::Invalid;
    };
    let end = if let Some(component) = components.get(1) {
        let Some(end) = parse_grid_track_placement(context, property, component) else {
            return ParseOutcome::Invalid;
        };
        end
    } else if is_custom_ident_placement(&start) {
        start.clone()
    } else {
        auto_grid_track_placement()
    };
    let sub_properties = if property == property_id::GRID_COLUMN {
        vec![property_id::GRID_COLUMN_START, property_id::GRID_COLUMN_END]
    } else {
        vec![property_id::GRID_ROW_START, property_id::GRID_ROW_END]
    };
    grid_shorthand(property, sub_properties, vec![start, end])
}

fn parse_grid_area_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    let Some(components) = slash_separated_components(values).filter(|components| components.len() <= 4) else {
        return ParseOutcome::Invalid;
    };
    let Some(row_start) = parse_grid_track_placement(context, property_id::GRID_ROW_START, components[0]) else {
        return ParseOutcome::Invalid;
    };
    let mut placements = vec![row_start];
    for (index, property) in [
        property_id::GRID_COLUMN_START,
        property_id::GRID_ROW_END,
        property_id::GRID_COLUMN_END,
    ]
    .into_iter()
    .enumerate()
    {
        let value = if let Some(component) = components.get(index + 1) {
            let Some(value) = parse_grid_track_placement(context, property, component) else {
                return ParseOutcome::Invalid;
            };
            value
        } else {
            let source = if index == 2 { &placements[1] } else { &placements[0] };
            if is_custom_ident_placement(source) {
                source.clone()
            } else {
                auto_grid_track_placement()
            }
        };
        placements.push(value);
    }
    grid_shorthand(
        property_id::GRID_AREA,
        vec![
            property_id::GRID_ROW_START,
            property_id::GRID_COLUMN_START,
            property_id::GRID_ROW_END,
            property_id::GRID_COLUMN_END,
        ],
        placements,
    )
}

fn initial_grid_values() -> [StyleValueData; 6] {
    [
        StyleValueData::GridAutoFlow {
            row: true,
            dense: false,
        },
        grid_track_list(
            false,
            false,
            vec![RetainedGridTrackEntry::size(StyleValueData::Keyword {
                keyword: keyword::AUTO,
            })],
        ),
        grid_track_list(
            false,
            false,
            vec![RetainedGridTrackEntry::size(StyleValueData::Keyword {
                keyword: keyword::AUTO,
            })],
        ),
        StyleValueData::GridTemplateArea {
            grid_areas: RetainedGridAreaList::from_retained_elements(Vec::new()),
            row_count: 0,
            column_count: 0,
        },
        grid_track_list(false, false, Vec::new()),
        grid_track_list(false, false, Vec::new()),
    ]
}

fn parse_grid_template_area_form(context: &ParseContext, values: &[ComponentValue]) -> Option<[StyleValueData; 3]> {
    fn append_line_names(
        rows: &mut Vec<RetainedGridTrackEntry>,
        mut names: Vec<crate::css::retained_fly_string::RetainedUtf16FlyString>,
    ) {
        if names.is_empty() {
            return;
        }
        if let Some(last) = rows.last_mut()
            && last.kind == crate::css::style_value::GridTrackEntryKind::LineNames
        {
            let mut combined = last.names.as_slice().to_vec();
            combined.append(&mut names);
            *last = RetainedGridTrackEntry::line_names(combined);
            return;
        }
        rows.push(RetainedGridTrackEntry::line_names(names));
    }

    let mut tokens = TokenStream::new(values);
    let mut rows = Vec::new();
    let mut area_tokens = Vec::new();
    let mut parsed_rows = 0;
    loop {
        let start = tokens.position;
        tokens.discard_whitespace();
        if let Some(line_names) = parse_grid_line_names(context, &mut tokens) {
            append_line_names(&mut rows, line_names);
        } else {
            tokens.position = start;
            tokens.discard_whitespace();
        }
        tokens.discard_whitespace();
        let Some(_) = tokens.next_token().string() else {
            tokens.position = start;
            break;
        };
        area_tokens.push(tokens.consume_a_token().clone());
        tokens.discard_whitespace();
        let track_size_start = tokens.position;
        rows.push(
            parse_grid_track_size(context, property_id::GRID_TEMPLATE_ROWS, &mut tokens).unwrap_or_else(|| {
                tokens.position = track_size_start;
                RetainedGridTrackEntry::size(StyleValueData::Keyword { keyword: keyword::AUTO })
            }),
        );
        tokens.discard_whitespace();
        if let Some(line_names) = parse_grid_line_names(context, &mut tokens)
            && !line_names.is_empty()
        {
            append_line_names(&mut rows, line_names);
        }
        parsed_rows += 1;
    }
    if parsed_rows == 0 {
        return None;
    }
    tokens.discard_whitespace();
    let columns = if tokens.has_next_token() && tokens.next_token().is_delim(b'/') {
        tokens.discard_a_token();
        let start = tokens.position;
        let columns = parse_grid_track_list(context, property_id::GRID_TEMPLATE_COLUMNS, &mut tokens)?;
        if tokens.position == start {
            return None;
        }
        grid_track_list(false, false, columns)
    } else {
        grid_track_list(false, false, Vec::new())
    };
    tokens.discard_whitespace();
    if tokens.has_next_token() {
        return None;
    }
    Some([
        parse_grid_template_areas(context, &area_tokens)?,
        grid_track_list(false, false, rows),
        columns,
    ])
}

fn parse_grid_template_shorthand(
    context: &ParseContext,
    property: u16,
    values: &[ComponentValue],
    include_auto_properties: bool,
) -> ParseOutcome {
    let initial = initial_grid_values();
    let template_values = if values
        .iter()
        .filter(|value| !value.is_whitespace())
        .collect::<Vec<_>>()
        .as_slice()
        .first()
        .is_some_and(|value| {
            value
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"none"))
        })
        && values.iter().filter(|value| !value.is_whitespace()).count() == 1
    {
        [initial[3].clone(), initial[4].clone(), initial[5].clone()]
    } else if let Some(components) = slash_separated_components(values).filter(|components| components.len() == 2) {
        let Some(rows) = parse_grid_track_size_list(context, property_id::GRID_TEMPLATE_ROWS, components[0]) else {
            return parse_grid_template_area_form(context, values).map_or(ParseOutcome::Invalid, |parsed| {
                grid_template_result(property, include_auto_properties, initial, parsed)
            });
        };
        let Some(columns) = parse_grid_track_size_list(context, property_id::GRID_TEMPLATE_COLUMNS, components[1])
        else {
            return ParseOutcome::Invalid;
        };
        [initial[3].clone(), rows, columns]
    } else {
        let Some(parsed) = parse_grid_template_area_form(context, values) else {
            return ParseOutcome::Invalid;
        };
        parsed
    };
    grid_template_result(property, include_auto_properties, initial, template_values)
}

fn grid_template_result(
    property: u16,
    include_auto_properties: bool,
    initial: [StyleValueData; 6],
    template: [StyleValueData; 3],
) -> ParseOutcome {
    if include_auto_properties {
        grid_shorthand(
            property,
            vec![
                property_id::GRID_AUTO_FLOW,
                property_id::GRID_AUTO_ROWS,
                property_id::GRID_AUTO_COLUMNS,
                property_id::GRID_TEMPLATE_AREAS,
                property_id::GRID_TEMPLATE_ROWS,
                property_id::GRID_TEMPLATE_COLUMNS,
            ],
            vec![
                initial[0].clone(),
                initial[1].clone(),
                initial[2].clone(),
                template[0].clone(),
                template[1].clone(),
                template[2].clone(),
            ],
        )
    } else {
        grid_shorthand(
            property,
            vec![
                property_id::GRID_TEMPLATE_AREAS,
                property_id::GRID_TEMPLATE_ROWS,
                property_id::GRID_TEMPLATE_COLUMNS,
            ],
            template.to_vec(),
        )
    }
}

fn parse_auto_flow_component(
    context: &ParseContext,
    auto_track_property: u16,
    values: &[ComponentValue],
    row: bool,
) -> Option<(StyleValueData, Option<StyleValueData>)> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    let mut found_auto_flow = false;
    let mut dense = false;
    for _ in 0..2 {
        let Some(identifier) = tokens.next_token().ident() else {
            break;
        };
        if equals_ascii_case_insensitive(identifier, b"auto-flow") && !found_auto_flow {
            found_auto_flow = true;
        } else if equals_ascii_case_insensitive(identifier, b"dense") && !dense {
            dense = true;
        } else {
            break;
        }
        tokens.discard_a_token();
        tokens.discard_whitespace();
    }
    if !found_auto_flow {
        return None;
    }
    let auto_tracks = values[tokens.position..]
        .iter()
        .any(|value| !value.is_whitespace())
        .then(|| parse_grid_auto_track_sizes(context, auto_track_property, &values[tokens.position..]))
        .flatten();
    if values[tokens.position..].iter().any(|value| !value.is_whitespace()) && auto_tracks.is_none() {
        return None;
    }
    Some((StyleValueData::GridAutoFlow { row, dense }, auto_tracks))
}

fn parse_grid_shorthand(context: &ParseContext, values: &[ComponentValue]) -> ParseOutcome {
    if let outcome @ ParseOutcome::Parsed(_) = parse_grid_template_shorthand(context, property_id::GRID, values, true) {
        return outcome;
    }
    let Some(components) = slash_separated_components(values).filter(|components| components.len() == 2) else {
        return ParseOutcome::Invalid;
    };
    let initial = initial_grid_values();
    if let Some((flow, auto_rows)) =
        parse_auto_flow_component(context, property_id::GRID_AUTO_ROWS, components[0], true)
    {
        let Some(template_columns) =
            parse_grid_track_size_list(context, property_id::GRID_TEMPLATE_COLUMNS, components[1])
        else {
            return ParseOutcome::Invalid;
        };
        return grid_shorthand(
            property_id::GRID,
            vec![
                property_id::GRID_AUTO_FLOW,
                property_id::GRID_AUTO_ROWS,
                property_id::GRID_AUTO_COLUMNS,
                property_id::GRID_TEMPLATE_AREAS,
                property_id::GRID_TEMPLATE_ROWS,
                property_id::GRID_TEMPLATE_COLUMNS,
            ],
            vec![
                flow,
                auto_rows.unwrap_or_else(|| initial[1].clone()),
                initial[2].clone(),
                initial[3].clone(),
                initial[4].clone(),
                template_columns,
            ],
        );
    }
    let Some(template_rows) = parse_grid_track_size_list(context, property_id::GRID_TEMPLATE_ROWS, components[0])
    else {
        return ParseOutcome::Invalid;
    };
    let Some((flow, auto_columns)) =
        parse_auto_flow_component(context, property_id::GRID_AUTO_COLUMNS, components[1], false)
    else {
        return ParseOutcome::Invalid;
    };
    grid_shorthand(
        property_id::GRID,
        vec![
            property_id::GRID_AUTO_FLOW,
            property_id::GRID_AUTO_ROWS,
            property_id::GRID_AUTO_COLUMNS,
            property_id::GRID_TEMPLATE_AREAS,
            property_id::GRID_TEMPLATE_ROWS,
            property_id::GRID_TEMPLATE_COLUMNS,
        ],
        vec![
            flow,
            initial[1].clone(),
            auto_columns.unwrap_or_else(|| initial[2].clone()),
            initial[3].clone(),
            template_rows,
            initial[5].clone(),
        ],
    )
}

fn next_code_point(units: &[u16], position: usize) -> (u32, usize) {
    let first = units[position];
    if (0xd800..=0xdbff).contains(&first)
        && let Some(&second) = units.get(position + 1)
        && (0xdc00..=0xdfff).contains(&second)
    {
        let code_point = 0x10000 + ((u32::from(first) - 0xd800) << 10) + (u32::from(second) - 0xdc00);
        return (code_point, 2);
    }
    (u32::from(first), 1)
}

fn is_grid_area_whitespace(code_point: u32) -> bool {
    matches!(code_point, 0x09 | 0x0a | 0x0c | 0x0d | 0x20)
}

fn is_ident_code_point(code_point: u32) -> bool {
    (u32::from(b'0')..=u32::from(b'9')).contains(&code_point)
        || (u32::from(b'A')..=u32::from(b'Z')).contains(&code_point)
        || (u32::from(b'a')..=u32::from(b'z')).contains(&code_point)
        || code_point >= 0x80
        || code_point == u32::from(b'_')
        || code_point == u32::from(b'-')
}

fn parse_grid_area_row(string: &[u16]) -> Option<Vec<Vec<u16>>> {
    let mut columns = Vec::new();
    let mut position = 0;
    while position < string.len() {
        let (code_point, width) = next_code_point(string, position);
        if is_grid_area_whitespace(code_point) {
            position += width;
            while position < string.len() {
                let (code_point, width) = next_code_point(string, position);
                if !is_grid_area_whitespace(code_point) {
                    break;
                }
                position += width;
            }
        } else if code_point == b'.' as u32 {
            position += width;
            while position < string.len() {
                let (code_point, width) = next_code_point(string, position);
                if code_point != b'.' as u32 {
                    break;
                }
                position += width;
            }
            columns.push(vec![u16::from(b'.')]);
        } else if is_ident_code_point(code_point) {
            let start = position;
            position += width;
            while position < string.len() {
                let (code_point, width) = next_code_point(string, position);
                if !is_ident_code_point(code_point) {
                    break;
                }
                position += width;
            }
            columns.push(string[start..position].to_vec());
        } else {
            return None;
        }
    }
    (!columns.is_empty()).then_some(columns)
}

// https://drafts.csswg.org/css-grid-2/#grid-template-areas-property
// none | <string>+
fn parse_grid_template_areas(context: &ParseContext, values: &[ComponentValue]) -> Option<StyleValueData> {
    let mut tokens = TokenStream::new(values);
    tokens.discard_whitespace();
    if parse_keyword(tokens.next_token(), keyword::NONE).is_some() {
        tokens.discard_a_token();
        tokens.discard_whitespace();
        return tokens.is_empty().then(|| StyleValueData::GridTemplateArea {
            grid_areas: RetainedGridAreaList::from_retained_elements(Vec::new()),
            row_count: 0,
            column_count: 0,
        });
    }
    let mut rows = Vec::new();
    let mut column_count = None;
    while let Some(string) = tokens.next_token().string() {
        let row = parse_grid_area_row(string)?;
        if column_count.is_some_and(|count| count != row.len()) {
            return None;
        }
        column_count = Some(row.len());
        rows.push(row);
        tokens.discard_a_token();
        tokens.discard_whitespace();
    }
    if rows.is_empty() || tokens.has_next_token() {
        return None;
    }

    let mut counts = HashMap::<Vec<u16>, usize>::new();
    for cell in rows
        .iter()
        .flatten()
        .filter(|cell| cell.as_slice() != [u16::from(b'.')])
    {
        *counts.entry(cell.clone()).or_default() += 1;
    }
    let mut areas = Vec::new();
    let mut seen = HashMap::<Vec<u16>, ()>::new();
    for y in 0..rows.len() {
        for x in 0..rows[y].len() {
            let name = &rows[y][x];
            if name.as_slice() == [u16::from(b'.')] || seen.contains_key(name) {
                continue;
            }
            let mut x_end = x;
            while x_end < rows[y].len() && rows[y][x_end] == *name {
                x_end += 1;
            }
            let mut y_end = y;
            while y_end < rows.len() && rows[y_end][x] == *name {
                y_end += 1;
            }
            let expected_count = (x_end - x) * (y_end - y);
            if (y..y_end).any(|check_y| (x..x_end).any(|check_x| rows[check_y][check_x] != *name))
                || counts.get(name).copied().unwrap_or(0) != expected_count
            {
                return None;
            }
            seen.insert(name.clone(), ());
            areas.push(RetainedGridArea::new(
                retain_fly_string(context, name)?,
                retain_fly_string(context, &implicit_grid_line_name(name, b"-start"))?,
                retain_fly_string(context, &implicit_grid_line_name(name, b"-end"))?,
                y,
                y_end,
                x,
                x_end,
            ));
        }
    }
    Some(StyleValueData::GridTemplateArea {
        grid_areas: RetainedGridAreaList::from_retained_elements(areas),
        row_count: rows.len(),
        column_count: column_count.unwrap_or(0),
    })
}

fn contains_math_function(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| {
        value
            .function()
            .is_some_and(|(name, children)| math_function_from_name(name).is_some() || contains_math_function(children))
            || value.square_block().is_some_and(contains_math_function)
    })
}

fn contains_deferred_function(values: &[ComponentValue]) -> bool {
    values.iter().any(|value| {
        if let Some((name, children)) = value.function() {
            if is_arbitrary_substitution_function(name) {
                return true;
            }
            if math_function_from_name(name).is_some()
                || equals_ascii_case_insensitive(name, b"repeat")
                || equals_ascii_case_insensitive(name, b"minmax")
                || equals_ascii_case_insensitive(name, b"fit-content")
            {
                return contains_deferred_function(children);
            }
            return true;
        }
        value.square_block().is_some_and(contains_deferred_function)
    })
}

pub(crate) fn parse_grid_property(context: &ParseContext, property: u16, values: &[ComponentValue]) -> ParseOutcome {
    let shorthand = match property {
        property_id::GRID => parse_grid_shorthand(context, values),
        property_id::GRID_TEMPLATE => parse_grid_template_shorthand(context, property, values, false),
        property_id::GRID_COLUMN | property_id::GRID_ROW => {
            parse_grid_track_placement_shorthand(context, property, values)
        }
        property_id::GRID_AREA => parse_grid_area_shorthand(context, values),
        _ => ParseOutcome::NotHandled,
    };
    if !matches!(shorthand, ParseOutcome::NotHandled) {
        return shorthand;
    }
    let parsed = match property {
        property_id::GRID_TEMPLATE_COLUMNS | property_id::GRID_TEMPLATE_ROWS => {
            parse_grid_track_size_list(context, property, values)
        }
        property_id::GRID_AUTO_COLUMNS | property_id::GRID_AUTO_ROWS => {
            parse_grid_auto_track_sizes(context, property, values)
        }
        property_id::GRID_COLUMN_END
        | property_id::GRID_COLUMN_START
        | property_id::GRID_ROW_END
        | property_id::GRID_ROW_START => parse_grid_track_placement(context, property, values),
        property_id::GRID_TEMPLATE_AREAS => parse_grid_template_areas(context, values),
        _ => return ParseOutcome::NotHandled,
    };
    if let Some(parsed) = parsed {
        ParseOutcome::Parsed(Arc::new(parsed))
    } else if contains_deferred_function(values) || contains_math_function(values) {
        ParseOutcome::NotHandled
    } else {
        ParseOutcome::Invalid
    }
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
            declared_namespaces: std::ptr::null(),
            declared_namespace_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            intern_utf16_fly_string: Some(discard_interned_string),
            normalize_svg_path_data: None,
            length_resolution_context: std::ptr::null(),
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_grid_property(&context(), property, &values)
    }

    fn assert_parsed(property: u16, source: &str) {
        assert!(matches!(parse(property, source), ParseOutcome::Parsed(_)), "{source}");
    }

    fn assert_invalid(property: u16, source: &str) {
        assert!(matches!(parse(property, source), ParseOutcome::Invalid), "{source}");
    }

    fn assert_not_handled(property: u16, source: &str) {
        assert!(matches!(parse(property, source), ParseOutcome::NotHandled), "{source}");
    }

    #[test]
    fn parses_track_lists_and_repeats() {
        for source in [
            "none",
            "[first] 10px [second] minmax(min-content, 1fr)",
            "repeat(3, [line] 20% 1fr)",
            "10px repeat(auto-fill, minmax(20px, 1fr)) 30px",
            "subgrid [a] repeat(2, [b] [c])",
            "subgrid repeat(auto-fill, [a])",
        ] {
            assert_parsed(property_id::GRID_TEMPLATE_COLUMNS, source);
        }
        for source in [
            "repeat(0, 1fr)",
            "repeat(auto-fit, 1fr)",
            "subgrid repeat(auto-fit, [a])",
        ] {
            assert_invalid(property_id::GRID_TEMPLATE_COLUMNS, source);
        }
        assert_not_handled(property_id::GRID_TEMPLATE_COLUMNS, "var(--tracks)");
    }

    #[test]
    fn parses_auto_tracks() {
        for source in ["auto", "minmax(min-content, 1fr) 20px", "fit-content(50%)"] {
            assert_parsed(property_id::GRID_AUTO_ROWS, source);
        }
        for source in ["none", "[line] 20px", "repeat(2, 10px)"] {
            assert_invalid(property_id::GRID_AUTO_ROWS, source);
        }
    }

    #[test]
    fn parses_grid_line_placements() {
        for source in [
            "auto",
            "header",
            "2",
            "-3 footer",
            "span 2",
            "span name",
            "span name 2",
            "2 span",
            "2 name span",
        ] {
            assert_parsed(property_id::GRID_COLUMN_START, source);
        }
        for source in ["0", "span", "span 0", "span -1", "name span 2"] {
            assert_invalid(property_id::GRID_COLUMN_START, source);
        }
    }

    #[test]
    fn parses_grid_placement_shorthands() {
        for (property, source, expected_values) in [
            (property_id::GRID_COLUMN, "foo", 2),
            (property_id::GRID_ROW, "2 / span 3", 2),
            (property_id::GRID_AREA, "header", 4),
            (property_id::GRID_AREA, "1 / col / 3 / col-end", 4),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand { values, .. } = &*value else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(values.as_slice().len(), expected_values);
        }
        for (property, source) in [
            (property_id::GRID_COLUMN, "1 / 2 / 3"),
            (property_id::GRID_ROW, "1.0"),
            (property_id::GRID_ROW, "/ 2"),
            (property_id::GRID_AREA, "a / b / c / d / e"),
        ] {
            assert_invalid(property, source);
        }
    }

    #[test]
    fn parses_grid_template_and_grid_shorthands() {
        for (property, source, expected_values) in [
            (property_id::GRID_TEMPLATE, "none", 3),
            (property_id::GRID_TEMPLATE, "100px / 1fr 2fr", 3),
            (
                property_id::GRID_TEMPLATE,
                "[top] 'a a' 40px [middle] 'b c' [bottom] / 1fr 1fr",
                3,
            ),
            (property_id::GRID, "none", 6),
            (property_id::GRID, "auto-flow dense 40px / 1fr 2fr", 6),
            (property_id::GRID, "100px / dense auto-flow 20px", 6),
        ] {
            let ParseOutcome::Parsed(value) = parse(property, source) else {
                panic!("shorthand should parse: {source}");
            };
            let StyleValueData::Shorthand { values, .. } = &*value else {
                panic!("value should be a shorthand: {source}");
            };
            assert_eq!(values.as_slice().len(), expected_values);
        }
        for (property, source) in [
            (property_id::GRID_TEMPLATE, "100px"),
            (property_id::GRID_TEMPLATE, "'a' 'b c' / 1fr"),
            (property_id::GRID, "auto-flow"),
            (property_id::GRID, "dense / 1fr"),
        ] {
            assert_invalid(property, source);
        }
    }

    #[test]
    fn parses_rectangular_template_areas() {
        for source in ["none", "\"a a\" \"b c\"", "\".. a ...\" \"b a c\""] {
            assert_parsed(property_id::GRID_TEMPLATE_AREAS, source);
        }
        for source in ["\"\"", "\"a b\" \"c\"", "\"a b\" \"b b\"", "\"10%\""] {
            assert_invalid(property_id::GRID_TEMPLATE_AREAS, source);
        }
    }
}
