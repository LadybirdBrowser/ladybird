/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::keyword_from_ascii_case_insensitive;
use crate::css::css_pixels::CssPixels;
use crate::css::css_tokenizer::{ParserString, ParserTokenKind, TokenizerInput, tokenize_for_parser};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{
    ComponentKind, ComponentValue, consume_a_list_of_component_values, trim_whitespace,
};
use crate::css::parser::token_stream::TokenStream;
use crate::css::parser::value_parser::{
    FfiValueParsingContext, FfiValueParsingContextKind, NumericRange, ParseContext, equals_ascii_case_insensitive,
    is_valid_custom_ident, parse_integer_from_stream, parse_length_from_stream, parse_number_from_stream,
    parse_ratio_value_with_context, parse_resolution_from_stream,
};
use crate::css::serialize::{StringUnits, TextSink, serialize_an_identifier, serialize_component_values_to_utf16};
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::css::style_value::StyleValueData;
use std::ffi::c_void;
use std::sync::Arc;

include!(concat!(env!("OUT_DIR"), "/media_features_generated.rs"));

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum QueryKind {
    Media,
    Size,
    Style,
    Supports,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[allow(dead_code)] // True is part of the C++ MatchResult ABI, but no query grammar constructs it.
pub(crate) enum MatchResult {
    False,
    True,
    Unknown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[allow(dead_code)] // Constructed by C++ when it builds the media environment snapshot.
pub enum FfiMediaFeatureValueKind {
    Absent,
    Ident,
    Integer,
    Length,
    Ratio,
    Resolution,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSupportsFeatureKind {
    Declaration,
    Selector,
    FontTech,
    FontFormat,
    AtRule,
    Env,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiMediaFeatureValue {
    pub kind: FfiMediaFeatureValueKind,
    pub keyword: u16,
    pub value: f64,
    pub second_value: f64,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiMediaEnvironment {
    pub values: *const FfiMediaFeatureValue,
    pub value_count: usize,
    pub length_resolution_context: *const c_void,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiContainerStyleFeatureKind {
    Boolean,
    Plain,
    Range,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleRangeValueKind {
    Property,
    Components,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleRangeValue {
    pub kind: FfiStyleRangeValueKind,
    pub value: FfiUtf16View,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiContainerStyleFeature {
    pub kind: FfiContainerStyleFeatureKind,
    pub values: *const FfiStyleRangeValue,
    pub value_count: usize,
    pub first_comparison: u8,
    pub second_comparison: u8,
}

type EvaluateContainerStyleFeature = unsafe extern "C" fn(*mut c_void, FfiContainerStyleFeature) -> u8;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiContainerFacts {
    pub container_available: bool,
    pub size_available: bool,
    pub width: f64,
    pub height: f64,
    pub inline_axis_horizontal: bool,
    pub length_resolution_context: *const c_void,
    pub style_context: *mut c_void,
    pub evaluate_style_feature: EvaluateContainerStyleFeature,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum FeatureComparison {
    Equal,
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FeatureNameType {
    Normal,
    Min,
    Max,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct QueryFeatureValue {
    pub components: Vec<ComponentValue>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum QueryFeature {
    Boolean {
        id: u8,
    },
    Plain {
        id: u8,
        name_type: FeatureNameType,
        value: QueryFeatureValue,
    },
    Range {
        id: u8,
        left: Option<(QueryFeatureValue, FeatureComparison)>,
        right: Option<(FeatureComparison, QueryFeatureValue)>,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum SupportsFeature {
    Declaration {
        components: Vec<ComponentValue>,
        matches: bool,
    },
    Selector {
        components: Vec<ComponentValue>,
        matches: bool,
    },
    FontTech {
        name: ParserString,
        matches: bool,
    },
    FontFormat {
        name: ParserString,
        matches: bool,
    },
    AtRule {
        name: ParserString,
        matches: bool,
    },
    Env {
        name: ParserString,
        matches: bool,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum StyleRangeValue {
    Property(ParserString),
    Components(Vec<ComponentValue>),
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum StyleFeature {
    Boolean(ParserString),
    Plain {
        name: ParserString,
        value: Vec<ComponentValue>,
        original_source: ParserString,
        original_value_source: ParserString,
    },
    Range {
        left: StyleRangeValue,
        left_comparison: FeatureComparison,
        middle: StyleRangeValue,
        right: Option<(FeatureComparison, StyleRangeValue)>,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Expression {
    Not(Box<Expression>),
    And(Vec<Expression>),
    Or(Vec<Expression>),
    InParens(Box<Expression>),
    GeneralEnclosed {
        component: ComponentValue,
        result: MatchResult,
    },
    GeneralEnclosedValues {
        components: Vec<ComponentValue>,
        result: MatchResult,
    },
    QueryFeature(QueryFeature),
    SupportsFeature(SupportsFeature),
    StyleFunction(Box<Expression>),
    StyleFeature(StyleFeature),
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct MediaQuery {
    pub negated: bool,
    pub media_type: Option<ParserString>,
    pub condition: Option<Expression>,
    pub valid: bool,
}

fn is_ident(value: &ComponentValue, expected: &[u8]) -> bool {
    value
        .ident()
        .is_some_and(|value| equals_ascii_case_insensitive(value, expected))
}

fn parenthesized_values(value: &ComponentValue) -> Option<&[ComponentValue]> {
    match &value.kind {
        ComponentKind::SimpleBlock {
            opening: ParserTokenKind::OpenParen,
            values,
        } => Some(values),
        _ => None,
    }
}

fn original_source(values: &[ComponentValue]) -> ParserString {
    ParserString::from(
        values
            .iter()
            .flat_map(|component| component.original_source_text.iter())
            .collect::<Vec<_>>()
            .into_boxed_slice(),
    )
}

fn components_from_source<'a>(source: impl Into<TokenizerInput<'a>>) -> Option<Vec<ComponentValue>> {
    consume_a_list_of_component_values(tokenize_for_parser(source)).ok()
}

fn contains_only_any_value(values: &[ComponentValue]) -> bool {
    values.iter().all(|value| match &value.kind {
        ComponentKind::Function { values, .. } | ComponentKind::SimpleBlock { values, .. } => {
            contains_only_any_value(values)
        }
        ComponentKind::Token(
            ParserTokenKind::EndOfFile
            | ParserTokenKind::BadString
            | ParserTokenKind::BadUrl
            | ParserTokenKind::Function(_)
            | ParserTokenKind::OpenCurly
            | ParserTokenKind::OpenParen
            | ParserTokenKind::OpenSquare
            | ParserTokenKind::CloseCurly
            | ParserTokenKind::CloseParen
            | ParserTokenKind::CloseSquare,
        ) => false,
        ComponentKind::Token(_) => true,
    })
}

fn contains_only_declaration_value(values: &[ComponentValue]) -> bool {
    contains_only_any_value(values)
        && values.iter().all(|value| {
            !matches!(value.kind, ComponentKind::Token(ParserTokenKind::Semicolon)) && !value.is_delim(b'!')
        })
}

fn strip_important(values: &[ComponentValue]) -> &[ComponentValue] {
    let values = trim_whitespace(values);
    let Some(last) = values.last() else {
        return values;
    };
    let Some(name) = last.ident() else {
        return values;
    };
    if !equals_ascii_case_insensitive(name, b"important") {
        return values;
    }
    let important_start = values[..values.len() - 1]
        .iter()
        .rposition(|value| !value.is_whitespace());
    let Some(important_start) = important_start else {
        return values;
    };
    if !values[important_start].is_delim(b'!') {
        return values;
    }
    trim_whitespace(&values[..important_start])
}

fn parse_general_enclosed(stream: &mut TokenStream<'_>, result: MatchResult) -> Option<Expression> {
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let component = transaction.consume_a_token();
    let values = match &component.kind {
        ComponentKind::Function { values, .. } => values.as_ref(),
        ComponentKind::SimpleBlock {
            opening: ParserTokenKind::OpenParen,
            values,
        } => values.as_ref(),
        _ => return None,
    };
    if !contains_only_any_value(values) {
        return None;
    }
    let component = component.clone();
    transaction.commit();
    Some(Expression::GeneralEnclosed { component, result })
}

fn parse_boolean_expression_group<F>(
    stream: &mut TokenStream<'_>,
    result_for_general_enclosed: MatchResult,
    parse_test: &F,
) -> Option<Expression>
where
    F: Fn(&mut TokenStream<'_>) -> Option<Expression>,
{
    let first = stream.next_token().clone();
    if let Some(values) = parenthesized_values(&first) {
        let mut transaction = stream.begin_transaction();
        transaction.discard_a_token();
        transaction.discard_whitespace();
        let mut child_stream = TokenStream::new(values);
        if let Some(mut expression) =
            parse_boolean_expression(&mut child_stream, result_for_general_enclosed, parse_test)
            && child_stream.is_empty()
        {
            if let Expression::GeneralEnclosedValues { components, .. } = &mut expression {
                *components = values.to_vec();
            }
            transaction.commit();
            return Some(Expression::InParens(Box::new(expression)));
        }
        if trim_whitespace(values)
            .first()
            .is_some_and(|value| is_ident(value, b"not"))
        {
            return None;
        }
    }

    parse_test(stream).or_else(|| parse_general_enclosed(stream, result_for_general_enclosed))
}

pub(crate) fn parse_boolean_expression<F>(
    stream: &mut TokenStream<'_>,
    result_for_general_enclosed: MatchResult,
    parse_test: &F,
) -> Option<Expression>
where
    F: Fn(&mut TokenStream<'_>) -> Option<Expression>,
{
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();

    if is_ident(transaction.next_token(), b"not") {
        transaction.discard_a_token();
        transaction.discard_whitespace();
        let child = parse_boolean_expression_group(&mut transaction, result_for_general_enclosed, parse_test)?;
        transaction.discard_whitespace();
        transaction.commit();
        return Some(Expression::Not(Box::new(child)));
    }

    #[derive(Clone, Copy, PartialEq, Eq)]
    enum Combinator {
        And,
        Or,
    }

    let mut children = Vec::new();
    let mut combinator = None;
    while transaction.has_next_token() {
        if !children.is_empty() {
            let next_combinator = if is_ident(transaction.next_token(), b"and") {
                Combinator::And
            } else if is_ident(transaction.next_token(), b"or") {
                Combinator::Or
            } else {
                return None;
            };
            transaction.discard_a_token();
            if combinator.is_some_and(|combinator| combinator != next_combinator) {
                return None;
            }
            combinator = Some(next_combinator);
        }

        transaction.discard_whitespace();
        children.push(parse_boolean_expression_group(
            &mut transaction,
            result_for_general_enclosed,
            parse_test,
        )?);
        transaction.discard_whitespace();
    }

    if children.is_empty() {
        return None;
    }
    transaction.commit();
    if children.len() == 1 {
        return children.pop();
    }
    match combinator? {
        Combinator::And => Some(Expression::And(children)),
        Combinator::Or => Some(Expression::Or(children)),
    }
}

fn parse_feature_comparison(stream: &mut TokenStream<'_>) -> Option<FeatureComparison> {
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    if transaction.next_token().is_delim(b'=') {
        transaction.discard_a_token();
        transaction.commit();
        return Some(FeatureComparison::Equal);
    }
    let comparison = if transaction.next_token().is_delim(b'<') {
        FeatureComparison::LessThan
    } else if transaction.next_token().is_delim(b'>') {
        FeatureComparison::GreaterThan
    } else {
        return None;
    };
    transaction.discard_a_token();
    let comparison = if transaction.next_token().is_delim(b'=') {
        transaction.discard_a_token();
        match comparison {
            FeatureComparison::LessThan => FeatureComparison::LessThanOrEqual,
            FeatureComparison::GreaterThan => FeatureComparison::GreaterThanOrEqual,
            _ => unreachable!(),
        }
    } else {
        comparison
    };
    transaction.commit();
    Some(comparison)
}

fn comparisons_match(left: FeatureComparison, right: FeatureComparison) -> bool {
    matches!(
        (left, right),
        (
            FeatureComparison::LessThan | FeatureComparison::LessThanOrEqual,
            FeatureComparison::LessThan | FeatureComparison::LessThanOrEqual
        ) | (
            FeatureComparison::GreaterThan | FeatureComparison::GreaterThanOrEqual,
            FeatureComparison::GreaterThan | FeatureComparison::GreaterThanOrEqual
        )
    )
}

fn is_feature_value_component(value: &ComponentValue) -> bool {
    match &value.kind {
        ComponentKind::Function { .. } | ComponentKind::SimpleBlock { .. } => true,
        ComponentKind::Token(ParserTokenKind::Delim(value)) => !matches!(*value, 0x3c..=0x3e),
        ComponentKind::Token(
            ParserTokenKind::Ident(_)
            | ParserTokenKind::AtKeyword(_)
            | ParserTokenKind::Hash { .. }
            | ParserTokenKind::String(_)
            | ParserTokenKind::BadString
            | ParserTokenKind::Url(_)
            | ParserTokenKind::BadUrl
            | ParserTokenKind::Number { .. }
            | ParserTokenKind::Percentage { .. }
            | ParserTokenKind::Dimension { .. }
            | ParserTokenKind::Whitespace
            | ParserTokenKind::Comma,
        ) => true,
        ComponentKind::Token(_) => false,
    }
}

fn parse_feature_value(stream: &mut TokenStream<'_>) -> Option<QueryFeatureValue> {
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let start = transaction.current_index();
    while transaction.has_next_token() && is_feature_value_component(transaction.next_token()) {
        transaction.discard_a_token();
    }
    let components = trim_whitespace(transaction.tokens_since(start)).to_vec();
    if components.is_empty() {
        return None;
    }
    transaction.commit();
    Some(QueryFeatureValue { components })
}

fn parse_feature_name<R>(
    stream: &mut TokenStream<'_>,
    kind: QueryKind,
    allow_min_max_prefix: bool,
    resolve_feature: &R,
) -> Option<(FeatureNameType, u8, bool)>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut transaction = stream.begin_transaction();
    let name = transaction.consume_a_token().ident()?;
    if let Some((id, allows_range)) = resolve_feature(kind, name) {
        transaction.commit();
        return Some((FeatureNameType::Normal, id, allows_range));
    }
    if !allow_min_max_prefix || name.len() <= 4 {
        return None;
    }
    let name_type = if equals_ascii_case_insensitive(&name[..4], b"min-") {
        FeatureNameType::Min
    } else if equals_ascii_case_insensitive(&name[..4], b"max-") {
        FeatureNameType::Max
    } else {
        return None;
    };
    let (id, allows_range) = resolve_feature(kind, &name[4..])?;
    if !allows_range {
        return None;
    }
    transaction.commit();
    Some((name_type, id, allows_range))
}

fn parse_query_feature<R>(stream: &mut TokenStream<'_>, kind: QueryKind, resolve_feature: &R) -> Option<QueryFeature>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    {
        let mut transaction = stream.begin_transaction();
        transaction.discard_whitespace();
        if let Some((_, id, _)) = parse_feature_name(&mut transaction, kind, false, resolve_feature) {
            transaction.discard_whitespace();
            if !transaction.has_next_token() {
                transaction.commit();
                return Some(QueryFeature::Boolean { id });
            }
        }
    }
    {
        let mut transaction = stream.begin_transaction();
        transaction.discard_whitespace();
        if let Some((name_type, id, _)) = parse_feature_name(&mut transaction, kind, true, resolve_feature) {
            transaction.discard_whitespace();
            if transaction.next_token().is_colon() {
                transaction.discard_a_token();
                let value = parse_feature_value(&mut transaction)?;
                transaction.discard_whitespace();
                if !transaction.has_next_token() {
                    transaction.commit();
                    return Some(QueryFeature::Plain { id, name_type, value });
                }
            }
        }
    }
    {
        let mut transaction = stream.begin_transaction();
        transaction.discard_whitespace();
        if let Some((_, id, true)) = parse_feature_name(&mut transaction, kind, false, resolve_feature) {
            transaction.discard_whitespace();
            let comparison = parse_feature_comparison(&mut transaction)?;
            let value = parse_feature_value(&mut transaction)?;
            transaction.discard_whitespace();
            if !transaction.has_next_token() {
                transaction.commit();
                return Some(QueryFeature::Range {
                    id,
                    left: None,
                    right: Some((comparison, value)),
                });
            }
        }
    }

    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let start = transaction.current_index();
    while transaction.has_next_token() {
        let mut lookahead = transaction.clone();
        if parse_feature_comparison(&mut lookahead).is_some() {
            break;
        }
        transaction.discard_a_token();
    }
    let left_components = trim_whitespace(transaction.tokens_since(start)).to_vec();
    if left_components.is_empty() {
        return None;
    }
    let left_comparison = parse_feature_comparison(&mut transaction)?;
    transaction.discard_whitespace();
    let (_, id, true) = parse_feature_name(&mut transaction, kind, false, resolve_feature)? else {
        return None;
    };
    let left = QueryFeatureValue {
        components: left_components,
    };
    transaction.discard_whitespace();
    if !transaction.has_next_token() {
        transaction.commit();
        return Some(QueryFeature::Range {
            id,
            left: Some((left, left_comparison)),
            right: None,
        });
    }
    let right_comparison = parse_feature_comparison(&mut transaction)?;
    let right = parse_feature_value(&mut transaction)?;
    transaction.discard_whitespace();
    if transaction.has_next_token()
        || !comparisons_match(left_comparison, right_comparison)
        || left_comparison == FeatureComparison::Equal
    {
        return None;
    }
    transaction.commit();
    Some(QueryFeature::Range {
        id,
        left: Some((left, left_comparison)),
        right: Some((right_comparison, right)),
    })
}

fn parse_media_feature<R>(stream: &mut TokenStream<'_>, resolve_feature: &R) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let first = transaction.next_token().clone();
    let values = parenthesized_values(&first)?;
    transaction.discard_a_token();
    let mut inner = TokenStream::new(values);
    let feature = parse_query_feature(&mut inner, QueryKind::Media, resolve_feature)?;
    inner.discard_whitespace();
    if inner.has_next_token() {
        return None;
    }
    transaction.commit();
    Some(Expression::QueryFeature(feature))
}

pub(crate) fn parse_media_condition<R>(values: &[ComponentValue], resolve_feature: &R) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut stream = TokenStream::new(values);
    let expression = parse_boolean_expression(&mut stream, MatchResult::Unknown, &|stream| {
        parse_media_feature(stream, resolve_feature)
    })?;
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(expression)
}

fn parse_media_feature_from_source<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    let mut stream = TokenStream::new(&values);
    let expression = Expression::QueryFeature(parse_query_feature(&mut stream, QueryKind::Media, resolve_feature)?);
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(expression)
}

fn invalid_media_query() -> MediaQuery {
    MediaQuery {
        negated: true,
        media_type: Some(ParserString::from(
            "all".encode_utf16().collect::<Vec<_>>().into_boxed_slice(),
        )),
        condition: None,
        valid: false,
    }
}

fn parse_media_type(stream: &mut TokenStream<'_>) -> Option<ParserString> {
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let name = transaction.consume_a_token().ident()?;
    if [b"layer".as_slice(), b"not", b"and", b"only", b"or"]
        .iter()
        .any(|reserved| equals_ascii_case_insensitive(name, reserved))
    {
        return None;
    }
    let name = ParserString::from(name.to_vec().into_boxed_slice());
    transaction.commit();
    Some(name)
}

fn parse_one_media_query<R>(values: &[ComponentValue], resolve_feature: &R) -> MediaQuery
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    {
        let mut condition_stream = stream.clone();
        if let Some(condition) = parse_boolean_expression(&mut condition_stream, MatchResult::Unknown, &|stream| {
            parse_media_feature(stream, resolve_feature)
        }) {
            condition_stream.discard_whitespace();
            if !condition_stream.has_next_token() {
                return MediaQuery {
                    negated: false,
                    media_type: None,
                    condition: Some(condition),
                    valid: true,
                };
            }
        }
    }

    let negated = if is_ident(stream.next_token(), b"not") {
        stream.discard_a_token();
        stream.discard_whitespace();
        true
    } else if is_ident(stream.next_token(), b"only") {
        stream.discard_a_token();
        stream.discard_whitespace();
        false
    } else {
        false
    };
    let Some(media_type) = parse_media_type(&mut stream) else {
        return invalid_media_query();
    };
    stream.discard_whitespace();
    if !stream.has_next_token() {
        return MediaQuery {
            negated,
            media_type: Some(media_type),
            condition: None,
            valid: true,
        };
    }
    if !is_ident(stream.next_token(), b"and") {
        return invalid_media_query();
    }
    stream.discard_a_token();
    let condition = parse_boolean_expression(&mut stream, MatchResult::Unknown, &|stream| {
        parse_media_feature(stream, resolve_feature)
    });
    stream.discard_whitespace();
    let Some(condition) = condition else {
        return invalid_media_query();
    };
    if stream.has_next_token() || matches!(condition, Expression::Or(_)) {
        return invalid_media_query();
    }
    MediaQuery {
        negated,
        media_type: Some(media_type),
        condition: Some(condition),
        valid: true,
    }
}

pub(crate) fn parse_media_query_list_from_component_values<R>(
    values: &[ComponentValue],
    resolve_feature: &R,
) -> Vec<MediaQuery>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    if trim_whitespace(values).is_empty() {
        return Vec::new();
    }
    let mut result = Vec::new();
    let mut start = 0;
    for index in 0..=values.len() {
        if index == values.len() || values[index].is_comma() {
            result.push(parse_one_media_query(&values[start..index], resolve_feature));
            start = index + 1;
        }
    }
    result
}

pub(crate) fn parse_media_query_list<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Vec<MediaQuery>>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    Some(parse_media_query_list_from_component_values(&values, resolve_feature))
}

fn single_ident(values: &[ComponentValue]) -> Option<ParserString> {
    let values = trim_whitespace(values);
    (values.len() == 1)
        .then(|| values[0].ident())
        .flatten()
        .map(|name| ParserString::from(name.to_vec().into_boxed_slice()))
}

fn looks_like_supports_declaration(values: &[ComponentValue]) -> bool {
    let values = trim_whitespace(values);
    let Some((name, remainder)) = values.split_first() else {
        return false;
    };
    let Some(colon_index) = remainder.iter().position(|value| !value.is_whitespace()) else {
        return false;
    };
    let colon = &remainder[colon_index];
    let value = &remainder[colon_index + 1..];
    name.ident().is_some()
        && colon.is_colon()
        && value
            .iter()
            .all(|value| !matches!(value.kind, ComponentKind::Token(ParserTokenKind::Semicolon)))
}

fn supports_components_source(components: &[ComponentValue]) -> Vec<u16> {
    components
        .iter()
        .flat_map(|component| component.original_source_text.iter())
        .collect()
}

fn parse_supports_feature<E>(stream: &mut TokenStream<'_>, evaluate_feature: &E) -> Option<Expression>
where
    E: Fn(FfiSupportsFeatureKind, &[u16]) -> bool,
{
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let first = transaction.consume_a_token().clone();
    if let Some(values) = parenthesized_values(&first) {
        let values = trim_whitespace(values);
        if looks_like_supports_declaration(values) && contains_only_any_value(values) {
            let matches = evaluate_feature(FfiSupportsFeatureKind::Declaration, &supports_components_source(values));
            transaction.commit();
            return Some(Expression::InParens(Box::new(Expression::SupportsFeature(
                SupportsFeature::Declaration {
                    components: values.to_vec(),
                    matches,
                },
            ))));
        }
        return None;
    }
    let (name, values) = first.function()?;
    let (kind, name_or_source) = if equals_ascii_case_insensitive(name, b"selector") {
        (FfiSupportsFeatureKind::Selector, supports_components_source(values))
    } else if equals_ascii_case_insensitive(name, b"font-tech") {
        (
            FfiSupportsFeatureKind::FontTech,
            single_ident(values)?.as_ref().to_vec(),
        )
    } else if equals_ascii_case_insensitive(name, b"font-format") {
        (
            FfiSupportsFeatureKind::FontFormat,
            single_ident(values)?.as_ref().to_vec(),
        )
    } else if equals_ascii_case_insensitive(name, b"env") {
        (FfiSupportsFeatureKind::Env, single_ident(values)?.as_ref().to_vec())
    } else if equals_ascii_case_insensitive(name, b"at-rule") {
        let values = trim_whitespace(values);
        let [value] = values else {
            return None;
        };
        let ComponentKind::Token(ParserTokenKind::AtKeyword(name)) = &value.kind else {
            return None;
        };
        (FfiSupportsFeatureKind::AtRule, name.as_ref().to_vec())
    } else {
        return None;
    };
    let matches = evaluate_feature(kind, &name_or_source);
    let feature = match kind {
        FfiSupportsFeatureKind::Selector => SupportsFeature::Selector {
            components: values.to_vec(),
            matches,
        },
        FfiSupportsFeatureKind::FontTech => SupportsFeature::FontTech {
            name: ParserString::from(name_or_source.into_boxed_slice()),
            matches,
        },
        FfiSupportsFeatureKind::FontFormat => SupportsFeature::FontFormat {
            name: ParserString::from(name_or_source.into_boxed_slice()),
            matches,
        },
        FfiSupportsFeatureKind::AtRule => SupportsFeature::AtRule {
            name: ParserString::from(name_or_source.into_boxed_slice()),
            matches,
        },
        FfiSupportsFeatureKind::Env => SupportsFeature::Env {
            name: ParserString::from(name_or_source.into_boxed_slice()),
            matches,
        },
        FfiSupportsFeatureKind::Declaration => unreachable!(),
    };
    transaction.commit();
    Some(Expression::SupportsFeature(feature))
}

pub(crate) fn parse_supports_condition_from_component_values<E>(
    values: &[ComponentValue],
    evaluate_feature: &E,
) -> Option<Expression>
where
    E: Fn(FfiSupportsFeatureKind, &[u16]) -> bool,
{
    let mut stream = TokenStream::new(values);
    let expression = parse_boolean_expression(&mut stream, MatchResult::False, &|stream| {
        parse_supports_feature(stream, evaluate_feature)
    })?;
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(expression)
}

pub(crate) fn parse_supports_condition<'a, E>(
    source: impl Into<TokenizerInput<'a>>,
    evaluate_feature: &E,
) -> Option<Expression>
where
    E: Fn(FfiSupportsFeatureKind, &[u16]) -> bool,
{
    let values = components_from_source(source)?;
    parse_supports_condition_from_component_values(&values, evaluate_feature)
}

pub(crate) fn parse_supports_declaration_from_component_values<E>(
    values: &[ComponentValue],
    evaluate_feature: &E,
) -> Option<Expression>
where
    E: Fn(FfiSupportsFeatureKind, &[u16]) -> bool,
{
    let trimmed = trim_whitespace(values);
    if !looks_like_supports_declaration(trimmed) || !contains_only_any_value(values) {
        return None;
    }
    let matches = evaluate_feature(FfiSupportsFeatureKind::Declaration, &supports_components_source(values));
    Some(Expression::SupportsFeature(SupportsFeature::Declaration {
        components: values.to_vec(),
        matches,
    }))
}

fn parse_supports_declaration_from_source<'a, E>(
    source: impl Into<TokenizerInput<'a>>,
    evaluate_feature: &E,
) -> Option<Expression>
where
    E: Fn(FfiSupportsFeatureKind, &[u16]) -> bool,
{
    let values = components_from_source(source)?;
    parse_supports_declaration_from_component_values(&values, evaluate_feature)
}

fn parse_style_range_value(values: &[ComponentValue]) -> Option<StyleRangeValue> {
    let values = trim_whitespace(values);
    if values.is_empty() || !contains_only_declaration_value(values) {
        return None;
    }
    if let [value] = values
        && let Some(name) = value.ident()
        && name.len() > 2
        && name.starts_with(&[u16::from(b'-'), u16::from(b'-')])
    {
        return Some(StyleRangeValue::Property(ParserString::from(
            name.to_vec().into_boxed_slice(),
        )));
    }
    Some(StyleRangeValue::Components(values.to_vec()))
}

fn parse_style_feature<R>(stream: &mut TokenStream<'_>, resolve_feature: &R) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let original_values = &stream.values[stream.position..];
    let values = trim_whitespace(original_values);
    if values.is_empty() {
        return None;
    }
    let comparisons = values
        .iter()
        .enumerate()
        .filter_map(|(index, value)| {
            if !value.is_delim(b'<') && !value.is_delim(b'>') && !value.is_delim(b'=') {
                return None;
            }
            let mut comparison_stream = TokenStream::new(&values[index..]);
            parse_feature_comparison(&mut comparison_stream)
                .map(|comparison| (index, comparison, comparison_stream.position))
        })
        .collect::<Vec<_>>();
    if let Some((first_index, left_comparison, consumed)) = comparisons.first().copied() {
        let left = parse_style_range_value(&values[..first_index])?;
        let middle_start = first_index + consumed;
        if let Some((second_index, right_comparison, second_consumed)) =
            comparisons.iter().copied().find(|(index, _, _)| *index >= middle_start)
        {
            if !comparisons_match(left_comparison, right_comparison) || left_comparison == FeatureComparison::Equal {
                return None;
            }
            let middle = parse_style_range_value(&values[middle_start..second_index])?;
            let right = parse_style_range_value(&values[second_index + second_consumed..])?;
            stream.position = stream.values.len();
            return Some(Expression::StyleFeature(StyleFeature::Range {
                left,
                left_comparison,
                middle,
                right: Some((right_comparison, right)),
            }));
        }
        let middle = parse_style_range_value(&values[middle_start..])?;
        stream.position = stream.values.len();
        return Some(Expression::StyleFeature(StyleFeature::Range {
            left,
            left_comparison,
            middle,
            right: None,
        }));
    }

    if let Some(colon) = values.iter().position(ComponentValue::is_colon) {
        let name = single_ident(&values[..colon])?;
        if resolve_feature(QueryKind::Style, name.as_ref()).is_none() {
            stream.position = stream.values.len();
            return Some(Expression::GeneralEnclosedValues {
                components: original_values.to_vec(),
                result: MatchResult::Unknown,
            });
        }
        let value = strip_important(&values[colon + 1..]);
        if !contains_only_declaration_value(value)
            || value
                .iter()
                .any(|value| value.is_delim(b'<') || value.is_delim(b'>') || value.is_delim(b'='))
        {
            return None;
        }
        stream.position = stream.values.len();
        return Some(Expression::StyleFeature(StyleFeature::Plain {
            name,
            value: value.to_vec(),
            original_source: original_source(original_values),
            original_value_source: original_source(value),
        }));
    }

    let name = single_ident(values)?;
    if resolve_feature(QueryKind::Style, name.as_ref()).is_none() {
        stream.position = stream.values.len();
        return Some(Expression::GeneralEnclosedValues {
            components: original_values.to_vec(),
            result: MatchResult::Unknown,
        });
    }
    stream.position = stream.values.len();
    Some(Expression::StyleFeature(StyleFeature::Boolean(name)))
}

fn parse_style_query_from_source<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    let mut stream = TokenStream::new(&values);
    let expression = parse_boolean_expression(&mut stream, MatchResult::False, &|stream| {
        parse_style_feature(stream, resolve_feature)
    })?;
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(expression)
}

fn parse_container_feature<R>(stream: &mut TokenStream<'_>, resolve_feature: &R) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let first = transaction.next_token().clone();
    if let Some(values) = parenthesized_values(&first) {
        transaction.discard_a_token();
        let mut inner = TokenStream::new(values);
        let feature = parse_query_feature(&mut inner, QueryKind::Size, resolve_feature)?;
        inner.discard_whitespace();
        if inner.has_next_token() {
            return None;
        }
        transaction.commit();
        return Some(Expression::QueryFeature(feature));
    }
    if let Some((name, values)) = first.function()
        && equals_ascii_case_insensitive(name, b"style")
    {
        transaction.discard_a_token();
        let mut inner = TokenStream::new(values);
        let parsed_expression = parse_boolean_expression(&mut inner, MatchResult::Unknown, &|stream| {
            parse_style_feature(stream, resolve_feature)
        });
        inner.discard_whitespace();
        let expression = parsed_expression.filter(|_| !inner.has_next_token()).or_else(|| {
            (!trim_whitespace(values).is_empty() && contains_only_any_value(values)).then_some(
                Expression::GeneralEnclosedValues {
                    components: values.to_vec(),
                    result: MatchResult::Unknown,
                },
            )
        })?;
        transaction.commit();
        return Some(Expression::StyleFunction(Box::new(expression)));
    }
    None
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ContainerCondition {
    pub name: Option<ParserString>,
    pub query: Option<Expression>,
}

fn parse_container_condition<R>(values: &[ComponentValue], resolve_feature: &R) -> Option<ContainerCondition>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut stream = TokenStream::new(values);
    stream.discard_whitespace();
    let name = stream.next_token().ident().and_then(|name| {
        is_valid_custom_ident(name, &["none", "and", "not", "or"])
            .then(|| ParserString::from(name.to_vec().into_boxed_slice()))
    });
    if name.is_some() {
        stream.discard_a_token();
        stream.discard_whitespace();
    }
    let query = if stream.has_next_token() {
        Some(parse_boolean_expression(
            &mut stream,
            MatchResult::Unknown,
            &|stream| parse_container_feature(stream, resolve_feature),
        )?)
    } else {
        None
    };
    stream.discard_whitespace();
    if stream.has_next_token() || name.is_none() && query.is_none() {
        return None;
    }
    Some(ContainerCondition { name, query })
}

pub(crate) fn parse_container_condition_list_from_component_values<R>(
    values: &[ComponentValue],
    resolve_feature: &R,
) -> Option<Vec<ContainerCondition>>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    if trim_whitespace(values).is_empty() {
        return None;
    }
    let mut result = Vec::new();
    let mut start = 0;
    for index in 0..=values.len() {
        if index == values.len() || values[index].is_comma() {
            result.push(parse_container_condition(&values[start..index], resolve_feature)?);
            start = index + 1;
        }
    }
    Some(result)
}

pub(crate) fn parse_container_condition_list<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Vec<ContainerCondition>>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    parse_container_condition_list_from_component_values(&values, resolve_feature)
}

#[derive(Clone, Debug, PartialEq)]
enum QueryTree {
    MediaQuery(MediaQuery),
    Expression { expression: Expression, kind: QueryKind },
}

pub struct FfiQueryHandle {
    #[allow(dead_code)] // Read by query operations added in subsequent porting stages.
    tree: QueryTree,
}

#[allow(clippy::arc_with_non_send_sync)]
pub(crate) fn media_query_handle(query: MediaQuery) -> Arc<FfiQueryHandle> {
    Arc::new(FfiQueryHandle {
        tree: QueryTree::MediaQuery(query),
    })
}

#[allow(clippy::arc_with_non_send_sync)]
pub(crate) fn expression_query_handle(expression: Expression, kind: QueryKind) -> Arc<FfiQueryHandle> {
    Arc::new(FfiQueryHandle {
        tree: QueryTree::Expression { expression, kind },
    })
}

fn push_utf16(sink: &mut TextSink, value: &[u16]) {
    for &unit in value {
        sink.push_code_unit(unit);
    }
}

fn serialize_query_feature_value(sink: &mut TextSink, value: &QueryFeatureValue, id: u8, kind: QueryKind) {
    let components = trim_whitespace(&value.components);
    let media_feature_value_types = if kind == QueryKind::Media {
        MEDIA_FEATURES
            .get(usize::from(id))
            .map_or(0, |metadata| metadata.accepted_value_types)
    } else {
        0
    };
    let is_ratio = media_feature_value_types & MEDIA_FEATURE_VALUE_RATIO != 0 || (kind == QueryKind::Size && id == 0);
    if is_ratio {
        let components = components
            .iter()
            .filter(|component| !component.is_whitespace())
            .collect::<Vec<_>>();
        if let [numerator, slash, denominator] = components.as_slice()
            && slash.is_delim(b'/')
        {
            push_utf16(
                sink,
                &serialize_component_values_to_utf16(
                    std::slice::from_ref(*numerator),
                    crate::css::parser::component_value::ComponentSerializationMode::Normalized,
                ),
            );
            sink.push_ascii(" / ");
            push_utf16(
                sink,
                &serialize_component_values_to_utf16(
                    std::slice::from_ref(*denominator),
                    crate::css::parser::component_value::ComponentSerializationMode::Normalized,
                ),
            );
            return;
        }
    }
    if kind == QueryKind::Media
        && media_feature_value_types & MEDIA_FEATURE_VALUE_RESOLUTION != 0
        && let [component] = components
        && let Some((name, values)) = component.function()
        && let Some(serialized) = crate::css::calc::serialize_context_free_calculation(name, values, 4)
    {
        push_utf16(sink, &serialized);
        return;
    }
    let serialized = serialize_component_values_to_utf16(
        &value.components,
        crate::css::parser::component_value::ComponentSerializationMode::Normalized,
    );
    push_utf16(sink, &serialized);
}

fn feature_comparison_text(comparison: FeatureComparison) -> &'static str {
    match comparison {
        FeatureComparison::Equal => "=",
        FeatureComparison::LessThan => "<",
        FeatureComparison::LessThanOrEqual => "<=",
        FeatureComparison::GreaterThan => ">",
        FeatureComparison::GreaterThanOrEqual => ">=",
    }
}

fn serialize_query_feature(sink: &mut TextSink, feature: &QueryFeature, kind: QueryKind) {
    let id = match feature {
        QueryFeature::Boolean { id } | QueryFeature::Plain { id, .. } | QueryFeature::Range { id, .. } => *id,
    };
    let name = match kind {
        QueryKind::Media => MEDIA_FEATURES[usize::from(id)].name,
        QueryKind::Size => [
            "aspect-ratio",
            "block-size",
            "height",
            "inline-size",
            "orientation",
            "width",
        ][usize::from(id)],
        QueryKind::Supports | QueryKind::Style => unreachable!("this query kind does not use query features"),
    };

    sink.push_ascii("(");
    match feature {
        QueryFeature::Boolean { .. } => sink.push_ascii(name),
        QueryFeature::Plain { name_type, value, .. } => {
            match name_type {
                FeatureNameType::Normal => {}
                FeatureNameType::Min => sink.push_ascii("min-"),
                FeatureNameType::Max => sink.push_ascii("max-"),
            }
            sink.push_ascii(name);
            sink.push_ascii(": ");
            serialize_query_feature_value(sink, value, id, kind);
        }
        QueryFeature::Range { left, right, .. } => {
            if let Some((value, comparison)) = left {
                serialize_query_feature_value(sink, value, id, kind);
                sink.push_ascii(" ");
                sink.push_ascii(feature_comparison_text(*comparison));
                sink.push_ascii(" ");
            }
            sink.push_ascii(name);
            if let Some((comparison, value)) = right {
                sink.push_ascii(" ");
                sink.push_ascii(feature_comparison_text(*comparison));
                sink.push_ascii(" ");
                serialize_query_feature_value(sink, value, id, kind);
            }
        }
    }
    sink.push_ascii(")");
}

fn serialize_style_property_name(sink: &mut TextSink, name: &[u16]) {
    if name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) {
        serialize_an_identifier(sink, &StringUnits::Utf16(name));
        return;
    }
    let lowercase = name
        .iter()
        .map(|unit| {
            if *unit >= u16::from(b'A') && *unit <= u16::from(b'Z') {
                *unit + u16::from(b'a' - b'A')
            } else {
                *unit
            }
        })
        .collect::<Vec<_>>();
    serialize_an_identifier(sink, &StringUnits::Utf16(&lowercase));
}

fn serialize_style_range_value(sink: &mut TextSink, value: &StyleRangeValue) {
    match value {
        StyleRangeValue::Property(name) => serialize_style_property_name(sink, name),
        StyleRangeValue::Components(components) => push_utf16(
            sink,
            &serialize_component_values_to_utf16(
                components,
                crate::css::parser::component_value::ComponentSerializationMode::Normalized,
            ),
        ),
    }
}

fn serialize_expression(sink: &mut TextSink, expression: &Expression, kind: QueryKind) {
    match expression {
        Expression::Not(child) => {
            sink.push_ascii("not ");
            serialize_expression(sink, child, kind);
        }
        Expression::And(children) | Expression::Or(children) => {
            let separator = if matches!(expression, Expression::And(_)) {
                " and "
            } else {
                " or "
            };
            for (index, child) in children.iter().enumerate() {
                if index != 0 {
                    sink.push_ascii(separator);
                }
                serialize_expression(sink, child, kind);
            }
        }
        Expression::InParens(child) => {
            sink.push_ascii("(");
            serialize_expression(sink, child, kind);
            sink.push_ascii(")");
        }
        Expression::GeneralEnclosed { component, .. } => push_utf16(sink, &component.original_source_text.to_vec()),
        Expression::GeneralEnclosedValues { components, .. } => {
            for component in components {
                push_utf16(sink, &component.original_source_text.to_vec());
            }
        }
        Expression::QueryFeature(feature) => serialize_query_feature(sink, feature, kind),
        Expression::SupportsFeature(feature) => match feature {
            SupportsFeature::Declaration { components, .. } => {
                for component in components {
                    push_utf16(sink, &component.original_source_text.to_vec());
                }
            }
            SupportsFeature::Selector { components, .. } => {
                sink.push_ascii("selector(");
                for component in components {
                    push_utf16(sink, &component.original_source_text.to_vec());
                }
                sink.push_ascii(")");
            }
            SupportsFeature::FontTech { name, .. } => {
                sink.push_ascii("font-tech(");
                push_utf16(sink, name);
                sink.push_ascii(")");
            }
            SupportsFeature::FontFormat { name, .. } => {
                sink.push_ascii("font-format(");
                push_utf16(sink, name);
                sink.push_ascii(")");
            }
            SupportsFeature::AtRule { name, .. } => {
                sink.push_ascii("at-rule(@");
                serialize_an_identifier(sink, &StringUnits::Utf16(name));
                sink.push_ascii(")");
            }
            SupportsFeature::Env { name, .. } => {
                // NB: Preserve the existing C++ serialization until the behavior can be changed separately.
                sink.push_ascii("font-format(");
                serialize_an_identifier(sink, &StringUnits::Utf16(name));
                sink.push_ascii(")");
            }
        },
        Expression::StyleFunction(child) => {
            sink.push_ascii("style(");
            serialize_expression(sink, child, QueryKind::Style);
            sink.push_ascii(")");
        }
        Expression::StyleFeature(feature) => match feature {
            StyleFeature::Boolean(name) => serialize_style_property_name(sink, name),
            StyleFeature::Plain {
                name,
                original_value_source,
                ..
            } => {
                serialize_style_property_name(sink, name);
                sink.push_ascii(": ");
                push_utf16(sink, original_value_source);
            }
            StyleFeature::Range {
                left,
                left_comparison,
                middle,
                right,
            } => {
                serialize_style_range_value(sink, left);
                sink.push_ascii(" ");
                sink.push_ascii(feature_comparison_text(*left_comparison));
                sink.push_ascii(" ");
                serialize_style_range_value(sink, middle);
                if let Some((comparison, right)) = right {
                    sink.push_ascii(" ");
                    sink.push_ascii(feature_comparison_text(*comparison));
                    sink.push_ascii(" ");
                    serialize_style_range_value(sink, right);
                }
            }
        },
    }
}

fn serialize_media_query(query: &MediaQuery) -> Vec<u16> {
    let mut sink = TextSink::new();
    if query.negated {
        sink.push_ascii("not ");
    }

    let media_type_is_all = query
        .media_type
        .as_ref()
        .is_none_or(|media_type| equals_ascii_case_insensitive(media_type, b"all"));
    if query.negated || !media_type_is_all || query.condition.is_none() {
        let media_type = query.media_type.as_deref().unwrap_or(&[]);
        if equals_ascii_case_insensitive(media_type, b"all") {
            sink.push_ascii("all");
        } else if equals_ascii_case_insensitive(media_type, b"print") {
            sink.push_ascii("print");
        } else if equals_ascii_case_insensitive(media_type, b"screen") {
            sink.push_ascii("screen");
        } else {
            let lowercase = media_type
                .iter()
                .map(|unit| {
                    if *unit >= u16::from(b'A') && *unit <= u16::from(b'Z') {
                        *unit + u16::from(b'a' - b'A')
                    } else {
                        *unit
                    }
                })
                .collect::<Vec<_>>();
            serialize_an_identifier(&mut sink, &StringUnits::Utf16(&lowercase));
        }
        if query.condition.is_some() {
            sink.push_ascii(" and ");
        }
    }

    if let Some(condition) = &query.condition {
        serialize_expression(&mut sink, condition, QueryKind::Media);
    }
    sink.into_utf16()
}

#[derive(Debug, PartialEq)]
enum ResolvedFeatureValue {
    Ident(u16),
    Integer(i32),
    Length(f64),
    Ratio { numerator: f64, denominator: f64 },
    Resolution(f64),
    Unknown,
}

fn media_value_parse_context(value_context: &FfiValueParsingContext) -> ParseContext {
    ParseContext {
        in_quirks_mode: false,
        is_svg_presentation_attribute: false,
        is_substituted_value: false,
        contains_attr_tainted_values: false,
        is_ua_style_sheet: false,
        value_contexts: value_context,
        value_context_count: 1,
        document_url: std::ptr::null(),
        document_url_length: 0,
        document_base_url: std::ptr::null(),
        document_base_url_length: 0,
        intern_utf16_fly_string: None,
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

fn parse_one_value_from_stream(
    components: &[ComponentValue],
    parse: impl FnOnce(&mut TokenStream<'_>) -> Option<StyleValueData>,
) -> Option<StyleValueData> {
    let mut stream = TokenStream::new(components);
    let value = parse(&mut stream)?;
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(value)
}

fn resolve_number(value: &StyleValueData, length_context: Option<&FfiLengthResolutionContext>) -> Option<f64> {
    match value {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Calculated { .. } => length_context
            .and_then(|context| crate::css::calc::resolve_calculated_number_with_context(value, context))
            .or_else(|| crate::css::calc::resolve_calculated_number_without_context(value)),
        _ => None,
    }
}

fn resolve_parsed_media_feature_value(
    value: StyleValueData,
    length_context: Option<&FfiLengthResolutionContext>,
) -> Option<ResolvedFeatureValue> {
    match &value {
        StyleValueData::Integer { value } => Some(ResolvedFeatureValue::Integer(*value)),
        StyleValueData::Length { value, unit } => {
            let pixels = if let Some(context) = length_context {
                let result = crate::css::style_compute::absolutize_length(*value, usize::from(*unit), context);
                result.handled.then_some(result.px)?
            } else {
                crate::css::style_compute::absolute_length_to_px(*value, *unit)?
            };
            Some(ResolvedFeatureValue::Length(
                CssPixels::nearest_value_for(pixels).to_double(),
            ))
        }
        StyleValueData::Ratio { numerator, denominator } => Some(ResolvedFeatureValue::Ratio {
            numerator: resolve_number(numerator.data(), length_context)?,
            denominator: resolve_number(denominator.data(), length_context)?,
        }),
        StyleValueData::Resolution { value, unit } => {
            let value = crate::css::calc::CalcNumericValue::Resolution {
                value: *value,
                unit: *unit,
            }
            .to_canonical_number(crate::css::calc::LengthResolution::default());
            Some(ResolvedFeatureValue::Resolution(value))
        }
        StyleValueData::Calculated { .. } => {
            if let Some(value) = length_context
                .and_then(|context| crate::css::calc::resolve_calculated_integer_with_context(&value, context))
                .or_else(|| crate::css::calc::resolve_calculated_integer_without_context(&value))
            {
                return Some(ResolvedFeatureValue::Integer(value));
            }
            if let Some(context) = length_context
                && let Some(value) = crate::css::calc::resolve_calculated_length_with_context(&value, context)
            {
                return Some(ResolvedFeatureValue::Length(
                    CssPixels::nearest_value_for(value).to_double(),
                ));
            }
            length_context
                .and_then(|context| crate::css::calc::resolve_calculated_resolution_with_context(&value, context))
                .or_else(|| crate::css::calc::resolve_calculated_resolution_without_context(&value))
                .map(ResolvedFeatureValue::Resolution)
        }
        _ => None,
    }
}

fn parse_media_feature_value(
    id: u8,
    value: &QueryFeatureValue,
    length_context: Option<&FfiLengthResolutionContext>,
) -> ResolvedFeatureValue {
    let Some(metadata) = MEDIA_FEATURES.get(usize::from(id)) else {
        return ResolvedFeatureValue::Unknown;
    };
    let components = trim_whitespace(&value.components);
    if let [component] = components
        && let Some(name) = component.ident()
        && let Some(keyword) = keyword_from_ascii_case_insensitive(name)
        && metadata.accepted_keywords.contains(&keyword)
    {
        return ResolvedFeatureValue::Ident(keyword);
    }

    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Special,
        value: 2, // SpecialContext::MediaCondition
        secondary_value: 0,
        name: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: std::ptr::null(),
            length: 0,
        },
    };
    let context = media_value_parse_context(&value_context);
    let types = metadata.accepted_value_types;
    if types & MEDIA_FEATURE_VALUE_BOOLEAN != 0
        && let Some(value) = parse_one_value_from_stream(components, |stream| {
            parse_integer_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        })
        && (matches!(value, StyleValueData::Calculated { .. })
            || matches!(value, StyleValueData::Integer { value: 0 | 1 }))
    {
        return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
    }
    if types & MEDIA_FEATURE_VALUE_INTEGER != 0
        && let Some(value) = parse_one_value_from_stream(components, |stream| {
            parse_integer_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        })
    {
        return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
    }
    if types & MEDIA_FEATURE_VALUE_LENGTH != 0 {
        if let Some(value) = parse_one_value_from_stream(components, |stream| {
            parse_length_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        }) {
            return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
        }
        if let Some(value @ StyleValueData::Calculated { .. }) = parse_one_value_from_stream(components, |stream| {
            parse_number_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        }) && resolve_number(&value, length_context) == Some(0.0)
        {
            return ResolvedFeatureValue::Length(0.0);
        }
    }
    if types & MEDIA_FEATURE_VALUE_RATIO != 0 {
        let values = components
            .iter()
            .filter(|value| !value.is_whitespace())
            .collect::<Vec<_>>();
        if let Some(value) =
            parse_ratio_value_with_context(&context, crate::css::property_metadata::property_id::CUSTOM, &values)
        {
            return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
        }
    }
    if types & MEDIA_FEATURE_VALUE_RESOLUTION != 0
        && let Some(value) = parse_one_value_from_stream(components, |stream| {
            parse_resolution_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        })
    {
        return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
    }
    ResolvedFeatureValue::Unknown
}

fn parse_size_feature_value(
    id: u8,
    value: &QueryFeatureValue,
    length_context: Option<&FfiLengthResolutionContext>,
) -> ResolvedFeatureValue {
    let components = trim_whitespace(&value.components);
    if id == 4
        && let [component] = components
        && let Some(name) = component.ident()
        && let Some(keyword) = keyword_from_ascii_case_insensitive(name)
        && matches!(
            keyword,
            crate::css::css_enums::keyword::LANDSCAPE | crate::css::css_enums::keyword::PORTRAIT
        )
    {
        return ResolvedFeatureValue::Ident(keyword);
    }

    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Special,
        value: 2, // SpecialContext::MediaCondition
        secondary_value: 0,
        name: FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: std::ptr::null(),
            length: 0,
        },
    };
    let context = media_value_parse_context(&value_context);
    if matches!(id, 1 | 2 | 3 | 5) {
        if let Some(value) = parse_one_value_from_stream(components, |stream| {
            parse_length_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        }) {
            return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
        }
        if let Some(value @ StyleValueData::Calculated { .. }) = parse_one_value_from_stream(components, |stream| {
            parse_number_from_stream(
                &context,
                crate::css::property_metadata::property_id::CUSTOM,
                stream,
                NumericRange::INFINITE,
            )
        }) && resolve_number(&value, length_context) == Some(0.0)
        {
            return ResolvedFeatureValue::Length(0.0);
        }
    }
    if id == 0 {
        let values = components
            .iter()
            .filter(|value| !value.is_whitespace())
            .collect::<Vec<_>>();
        if let Some(value) =
            parse_ratio_value_with_context(&context, crate::css::property_metadata::property_id::CUSTOM, &values)
        {
            return resolve_parsed_media_feature_value(value, length_context).unwrap_or(ResolvedFeatureValue::Unknown);
        }
    }
    ResolvedFeatureValue::Unknown
}

fn resolved_environment_value(value: &FfiMediaFeatureValue) -> Option<ResolvedFeatureValue> {
    match value.kind {
        FfiMediaFeatureValueKind::Absent => None,
        FfiMediaFeatureValueKind::Ident => Some(ResolvedFeatureValue::Ident(value.keyword)),
        FfiMediaFeatureValueKind::Integer => Some(ResolvedFeatureValue::Integer(value.value as i32)),
        FfiMediaFeatureValueKind::Length => Some(ResolvedFeatureValue::Length(value.value)),
        FfiMediaFeatureValueKind::Ratio => Some(ResolvedFeatureValue::Ratio {
            numerator: value.value,
            denominator: value.second_value,
        }),
        FfiMediaFeatureValueKind::Resolution => Some(ResolvedFeatureValue::Resolution(value.value)),
    }
}

fn compare_feature_values(
    left: &ResolvedFeatureValue,
    comparison: FeatureComparison,
    right: &ResolvedFeatureValue,
) -> MatchResult {
    if matches!(left, ResolvedFeatureValue::Unknown) || matches!(right, ResolvedFeatureValue::Unknown) {
        return MatchResult::Unknown;
    }
    let comparison_matches = |ordering: Option<std::cmp::Ordering>| match comparison {
        FeatureComparison::Equal => ordering == Some(std::cmp::Ordering::Equal),
        FeatureComparison::LessThan => ordering == Some(std::cmp::Ordering::Less),
        FeatureComparison::LessThanOrEqual => {
            matches!(ordering, Some(std::cmp::Ordering::Less | std::cmp::Ordering::Equal))
        }
        FeatureComparison::GreaterThan => ordering == Some(std::cmp::Ordering::Greater),
        FeatureComparison::GreaterThanOrEqual => {
            matches!(ordering, Some(std::cmp::Ordering::Greater | std::cmp::Ordering::Equal))
        }
    };
    let matches = match (left, right) {
        (ResolvedFeatureValue::Ident(left), ResolvedFeatureValue::Ident(right)) => {
            comparison == FeatureComparison::Equal && left == right
        }
        (ResolvedFeatureValue::Integer(left), ResolvedFeatureValue::Integer(right)) => {
            comparison_matches(Some(left.cmp(right)))
        }
        (ResolvedFeatureValue::Length(left), ResolvedFeatureValue::Length(right))
        | (ResolvedFeatureValue::Resolution(left), ResolvedFeatureValue::Resolution(right)) => {
            comparison_matches(left.partial_cmp(right))
        }
        (
            ResolvedFeatureValue::Ratio {
                numerator: left_numerator,
                denominator: left_denominator,
            },
            ResolvedFeatureValue::Ratio {
                numerator: right_numerator,
                denominator: right_denominator,
            },
        ) => {
            comparison_matches((left_numerator / left_denominator).partial_cmp(&(right_numerator / right_denominator)))
        }
        _ => false,
    };
    if matches { MatchResult::True } else { MatchResult::False }
}

fn evaluate_query_feature(
    feature: &QueryFeature,
    environment: &[FfiMediaFeatureValue],
    length_context: Option<&FfiLengthResolutionContext>,
) -> MatchResult {
    let id = match feature {
        QueryFeature::Boolean { id } | QueryFeature::Plain { id, .. } | QueryFeature::Range { id, .. } => *id,
    };
    let Some(queried_value) = environment.get(usize::from(id)).and_then(resolved_environment_value) else {
        return MatchResult::False;
    };
    match feature {
        QueryFeature::Boolean { .. } => match &queried_value {
            ResolvedFeatureValue::Integer(value) => {
                if *value != 0 {
                    MatchResult::True
                } else {
                    MatchResult::False
                }
            }
            ResolvedFeatureValue::Length(value) | ResolvedFeatureValue::Resolution(value) => {
                if *value != 0.0 {
                    MatchResult::True
                } else {
                    MatchResult::False
                }
            }
            ResolvedFeatureValue::Ratio { numerator, denominator } => {
                if numerator.is_finite() && *numerator != 0.0 && denominator.is_finite() && *denominator != 0.0 {
                    MatchResult::True
                } else {
                    MatchResult::False
                }
            }
            ResolvedFeatureValue::Ident(keyword) => {
                if MEDIA_FEATURES[usize::from(id)].false_keywords.contains(keyword) {
                    MatchResult::False
                } else {
                    MatchResult::True
                }
            }
            ResolvedFeatureValue::Unknown => MatchResult::False,
        },
        QueryFeature::Plain { name_type, value, .. } => {
            let value = parse_media_feature_value(id, value, length_context);
            match name_type {
                FeatureNameType::Normal => compare_feature_values(&value, FeatureComparison::Equal, &queried_value),
                FeatureNameType::Min => {
                    compare_feature_values(&queried_value, FeatureComparison::GreaterThanOrEqual, &value)
                }
                FeatureNameType::Max => {
                    compare_feature_values(&queried_value, FeatureComparison::LessThanOrEqual, &value)
                }
            }
        }
        QueryFeature::Range { left, right, .. } => {
            if let Some((value, comparison)) = left {
                let value = parse_media_feature_value(id, value, length_context);
                let result = compare_feature_values(&value, *comparison, &queried_value);
                if result != MatchResult::True {
                    return result;
                }
            }
            if let Some((comparison, value)) = right {
                let value = parse_media_feature_value(id, value, length_context);
                let result = compare_feature_values(&queried_value, *comparison, &value);
                if result != MatchResult::True {
                    return result;
                }
            }
            MatchResult::True
        }
    }
}

fn evaluate_media_expression(
    expression: &Expression,
    environment: &[FfiMediaFeatureValue],
    length_context: Option<&FfiLengthResolutionContext>,
) -> MatchResult {
    match expression {
        Expression::Not(child) => match evaluate_media_expression(child, environment, length_context) {
            MatchResult::False => MatchResult::True,
            MatchResult::True => MatchResult::False,
            MatchResult::Unknown => MatchResult::Unknown,
        },
        Expression::And(children) => {
            let mut result = MatchResult::True;
            for child in children {
                match evaluate_media_expression(child, environment, length_context) {
                    MatchResult::False => return MatchResult::False,
                    MatchResult::Unknown => result = MatchResult::Unknown,
                    MatchResult::True => {}
                }
            }
            result
        }
        Expression::Or(children) => {
            let mut result = MatchResult::False;
            for child in children {
                match evaluate_media_expression(child, environment, length_context) {
                    MatchResult::True => return MatchResult::True,
                    MatchResult::Unknown => result = MatchResult::Unknown,
                    MatchResult::False => {}
                }
            }
            result
        }
        Expression::InParens(child) => evaluate_media_expression(child, environment, length_context),
        Expression::GeneralEnclosed { result, .. } | Expression::GeneralEnclosedValues { result, .. } => *result,
        Expression::QueryFeature(feature) => evaluate_query_feature(feature, environment, length_context),
        _ => MatchResult::Unknown,
    }
}

fn evaluate_supports_expression(expression: &Expression) -> MatchResult {
    match expression {
        Expression::Not(child) => match evaluate_supports_expression(child) {
            MatchResult::False => MatchResult::True,
            MatchResult::True => MatchResult::False,
            MatchResult::Unknown => MatchResult::Unknown,
        },
        Expression::And(children) => {
            let mut result = MatchResult::True;
            for child in children {
                match evaluate_supports_expression(child) {
                    MatchResult::False => return MatchResult::False,
                    MatchResult::Unknown => result = MatchResult::Unknown,
                    MatchResult::True => {}
                }
            }
            result
        }
        Expression::Or(children) => {
            let mut result = MatchResult::False;
            for child in children {
                match evaluate_supports_expression(child) {
                    MatchResult::True => return MatchResult::True,
                    MatchResult::Unknown => result = MatchResult::Unknown,
                    MatchResult::False => {}
                }
            }
            result
        }
        Expression::InParens(child) => evaluate_supports_expression(child),
        Expression::GeneralEnclosed { result, .. } | Expression::GeneralEnclosedValues { result, .. } => *result,
        Expression::SupportsFeature(feature) => {
            let matches = match feature {
                SupportsFeature::Declaration { matches, .. }
                | SupportsFeature::Selector { matches, .. }
                | SupportsFeature::FontTech { matches, .. }
                | SupportsFeature::FontFormat { matches, .. }
                | SupportsFeature::AtRule { matches, .. }
                | SupportsFeature::Env { matches, .. } => *matches,
            };
            if matches { MatchResult::True } else { MatchResult::False }
        }
        _ => MatchResult::Unknown,
    }
}

pub const CONTAINER_QUERY_REQUIRES_WIDTH: u8 = 1 << 0;
pub const CONTAINER_QUERY_REQUIRES_HEIGHT: u8 = 1 << 1;
pub const CONTAINER_QUERY_REQUIRES_INLINE_SIZE: u8 = 1 << 2;
pub const CONTAINER_QUERY_REQUIRES_BLOCK_SIZE: u8 = 1 << 3;
pub const CONTAINER_QUERY_REQUIRES_STYLE: u8 = 1 << 4;
#[allow(dead_code)] // Reserved until scroll-state container query parsing is implemented.
pub const CONTAINER_QUERY_REQUIRES_SCROLL_STATE: u8 = 1 << 5;
pub const CONTAINER_QUERY_HAS_UNKNOWN_FEATURE: u8 = 1 << 6;

fn container_requirements(expression: &Expression) -> u8 {
    match expression {
        Expression::Not(child) | Expression::InParens(child) | Expression::StyleFunction(child) => {
            let requirements = container_requirements(child);
            if matches!(expression, Expression::StyleFunction(_)) {
                requirements | CONTAINER_QUERY_REQUIRES_STYLE
            } else {
                requirements
            }
        }
        Expression::And(children) | Expression::Or(children) => children
            .iter()
            .fold(0, |requirements, child| requirements | container_requirements(child)),
        Expression::GeneralEnclosed { .. } | Expression::GeneralEnclosedValues { .. } => {
            CONTAINER_QUERY_HAS_UNKNOWN_FEATURE
        }
        Expression::QueryFeature(feature) => {
            let id = match feature {
                QueryFeature::Boolean { id } | QueryFeature::Plain { id, .. } | QueryFeature::Range { id, .. } => *id,
            };
            match id {
                0 | 4 => CONTAINER_QUERY_REQUIRES_WIDTH | CONTAINER_QUERY_REQUIRES_HEIGHT,
                1 => CONTAINER_QUERY_REQUIRES_BLOCK_SIZE,
                2 => CONTAINER_QUERY_REQUIRES_HEIGHT,
                3 => CONTAINER_QUERY_REQUIRES_INLINE_SIZE,
                5 => CONTAINER_QUERY_REQUIRES_WIDTH,
                _ => CONTAINER_QUERY_HAS_UNKNOWN_FEATURE,
            }
        }
        Expression::StyleFeature(_) => CONTAINER_QUERY_REQUIRES_STYLE,
        _ => CONTAINER_QUERY_HAS_UNKNOWN_FEATURE,
    }
}

fn container_size_value(id: u8, facts: &FfiContainerFacts) -> Option<ResolvedFeatureValue> {
    if !facts.size_available {
        return None;
    }
    Some(match id {
        0 => ResolvedFeatureValue::Ratio {
            numerator: facts.width,
            denominator: facts.height,
        },
        1 => ResolvedFeatureValue::Length(if facts.inline_axis_horizontal {
            facts.height
        } else {
            facts.width
        }),
        2 => ResolvedFeatureValue::Length(facts.height),
        3 => ResolvedFeatureValue::Length(if facts.inline_axis_horizontal {
            facts.width
        } else {
            facts.height
        }),
        4 => ResolvedFeatureValue::Ident(if facts.height >= facts.width {
            crate::css::css_enums::keyword::PORTRAIT
        } else {
            crate::css::css_enums::keyword::LANDSCAPE
        }),
        5 => ResolvedFeatureValue::Length(facts.width),
        _ => return None,
    })
}

fn evaluate_container_size_feature(
    feature: &QueryFeature,
    facts: &FfiContainerFacts,
    length_context: Option<&FfiLengthResolutionContext>,
) -> MatchResult {
    let id = match feature {
        QueryFeature::Boolean { id } | QueryFeature::Plain { id, .. } | QueryFeature::Range { id, .. } => *id,
    };
    let Some(queried_value) = container_size_value(id, facts) else {
        return MatchResult::Unknown;
    };
    match feature {
        QueryFeature::Boolean { .. } => match &queried_value {
            ResolvedFeatureValue::Length(value) => {
                if *value != 0.0 {
                    MatchResult::True
                } else {
                    MatchResult::False
                }
            }
            ResolvedFeatureValue::Ratio { numerator, denominator } => {
                if numerator.is_finite() && *numerator != 0.0 && denominator.is_finite() && *denominator != 0.0 {
                    MatchResult::True
                } else {
                    MatchResult::False
                }
            }
            ResolvedFeatureValue::Ident(_) => MatchResult::True,
            _ => MatchResult::False,
        },
        QueryFeature::Plain { name_type, value, .. } => {
            let value = parse_size_feature_value(id, value, length_context);
            match name_type {
                FeatureNameType::Normal => compare_feature_values(&value, FeatureComparison::Equal, &queried_value),
                FeatureNameType::Min => {
                    compare_feature_values(&queried_value, FeatureComparison::GreaterThanOrEqual, &value)
                }
                FeatureNameType::Max => {
                    compare_feature_values(&queried_value, FeatureComparison::LessThanOrEqual, &value)
                }
            }
        }
        QueryFeature::Range { left, right, .. } => {
            if let Some((value, comparison)) = left {
                let value = parse_size_feature_value(id, value, length_context);
                let result = compare_feature_values(&value, *comparison, &queried_value);
                if result != MatchResult::True {
                    return result;
                }
            }
            if let Some((comparison, value)) = right {
                let value = parse_size_feature_value(id, value, length_context);
                let result = compare_feature_values(&queried_value, *comparison, &value);
                if result != MatchResult::True {
                    return result;
                }
            }
            MatchResult::True
        }
    }
}

fn serialize_style_range_value_for_evaluation(value: &StyleRangeValue) -> (FfiStyleRangeValueKind, Vec<u16>) {
    match value {
        StyleRangeValue::Property(name) => (FfiStyleRangeValueKind::Property, name.as_ref().to_vec()),
        StyleRangeValue::Components(components) => (
            FfiStyleRangeValueKind::Components,
            serialize_component_values_to_utf16(
                components,
                crate::css::parser::component_value::ComponentSerializationMode::Normalized,
            ),
        ),
    }
}

fn evaluate_container_style_feature(feature: &StyleFeature, facts: &FfiContainerFacts) -> MatchResult {
    let evaluate = facts.evaluate_style_feature;
    let (kind, owned_values, first_comparison, second_comparison) = match feature {
        StyleFeature::Boolean(name) => (
            FfiContainerStyleFeatureKind::Boolean,
            vec![(FfiStyleRangeValueKind::Property, name.as_ref().to_vec())],
            0,
            0,
        ),
        StyleFeature::Plain { name, value, .. } => (
            FfiContainerStyleFeatureKind::Plain,
            vec![
                (FfiStyleRangeValueKind::Property, name.as_ref().to_vec()),
                (
                    FfiStyleRangeValueKind::Components,
                    serialize_component_values_to_utf16(
                        value,
                        crate::css::parser::component_value::ComponentSerializationMode::Normalized,
                    ),
                ),
            ],
            0,
            0,
        ),
        StyleFeature::Range {
            left,
            left_comparison,
            middle,
            right,
        } => {
            let mut values = vec![
                serialize_style_range_value_for_evaluation(left),
                serialize_style_range_value_for_evaluation(middle),
            ];
            let second_comparison = if let Some((comparison, value)) = right {
                values.push(serialize_style_range_value_for_evaluation(value));
                *comparison as u8
            } else {
                0
            };
            (
                FfiContainerStyleFeatureKind::Range,
                values,
                *left_comparison as u8,
                second_comparison,
            )
        }
    };
    let ffi_values = owned_values
        .iter()
        .map(|(kind, value)| FfiStyleRangeValue {
            kind: *kind,
            value: FfiUtf16View {
                ascii: std::ptr::null(),
                utf16: value.as_ptr(),
                length: value.len(),
            },
        })
        .collect::<Vec<_>>();
    let result = unsafe {
        evaluate(
            facts.style_context,
            FfiContainerStyleFeature {
                kind,
                values: ffi_values.as_ptr(),
                value_count: ffi_values.len(),
                first_comparison,
                second_comparison,
            },
        )
    };
    match result {
        0 => MatchResult::False,
        1 => MatchResult::True,
        _ => MatchResult::Unknown,
    }
}

fn evaluate_container_expression(
    expression: &Expression,
    facts: &FfiContainerFacts,
    length_context: Option<&FfiLengthResolutionContext>,
) -> MatchResult {
    match expression {
        Expression::Not(child) => match evaluate_container_expression(child, facts, length_context) {
            MatchResult::False => MatchResult::True,
            MatchResult::True => MatchResult::False,
            MatchResult::Unknown => MatchResult::Unknown,
        },
        Expression::And(children) => {
            let mut result = MatchResult::True;
            for child in children {
                match evaluate_container_expression(child, facts, length_context) {
                    MatchResult::False => return MatchResult::False,
                    MatchResult::Unknown => result = MatchResult::Unknown,
                    MatchResult::True => {}
                }
            }
            result
        }
        Expression::Or(children) => {
            let mut result = MatchResult::False;
            for child in children {
                match evaluate_container_expression(child, facts, length_context) {
                    MatchResult::True => return MatchResult::True,
                    MatchResult::Unknown => result = MatchResult::Unknown,
                    MatchResult::False => {}
                }
            }
            result
        }
        Expression::InParens(child) | Expression::StyleFunction(child) => {
            evaluate_container_expression(child, facts, length_context)
        }
        Expression::GeneralEnclosed { result, .. } | Expression::GeneralEnclosedValues { result, .. } => *result,
        Expression::QueryFeature(feature) => evaluate_container_size_feature(feature, facts, length_context),
        Expression::StyleFeature(feature) => evaluate_container_style_feature(feature, facts),
        _ => MatchResult::Unknown,
    }
}

fn evaluate_media_query(
    query: &MediaQuery,
    environment: &[FfiMediaFeatureValue],
    length_context: Option<&FfiLengthResolutionContext>,
) -> MatchResult {
    let mut result = match query.media_type.as_deref() {
        None => MatchResult::True,
        Some(media_type) if equals_ascii_case_insensitive(media_type, b"all") => MatchResult::True,
        Some(media_type) if equals_ascii_case_insensitive(media_type, b"screen") => MatchResult::True,
        Some(media_type) if equals_ascii_case_insensitive(media_type, b"print") => MatchResult::False,
        Some(_) => MatchResult::False,
    };
    if result != MatchResult::False
        && let Some(condition) = &query.condition
    {
        result = match (
            result,
            evaluate_media_expression(condition, environment, length_context),
        ) {
            (MatchResult::False, _) | (_, MatchResult::False) => MatchResult::False,
            (MatchResult::True, MatchResult::True) => MatchResult::True,
            _ => MatchResult::Unknown,
        };
    }
    if query.negated {
        result = match result {
            MatchResult::False => MatchResult::True,
            MatchResult::True => MatchResult::False,
            MatchResult::Unknown => MatchResult::Unknown,
        };
    }
    result
}

pub(crate) type ResolveQueryFeature = unsafe extern "C" fn(u8, *const u16, usize) -> u16;

type EvaluateSupportsFeature = unsafe extern "C" fn(*mut c_void, FfiSupportsFeatureKind, FfiUtf16View) -> bool;

type VisitSizesAttributeEntry = unsafe extern "C" fn(*mut c_void, *const u16, usize, *const u16, usize);

type VisitQueryHandle = unsafe extern "C" fn(*mut c_void, *const FfiQueryHandle);

type VisitContainerCondition = unsafe extern "C" fn(*mut c_void, *const u16, usize, bool, *const FfiQueryHandle);

type VisitQuerySerialization = unsafe extern "C" fn(*mut c_void, *const u16, usize);

/// Splits a sizes attribute into its top-level condition and source-size value components.
///
/// # Safety
/// The source must remain readable during the call. The callback must be valid and may only
/// retain copies of the borrowed source slices.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_visit_sizes_attribute_entries(
    source: FfiUtf16View,
    context: *mut c_void,
    visit: VisitSizesAttributeEntry,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return false;
        };
        let Some(values) = components_from_source(source) else {
            return false;
        };
        for entry in values.split(ComponentValue::is_comma) {
            let entry = trim_whitespace(entry);
            let (condition, size) = entry.split_last().map_or((&[][..], &[][..]), |(size, condition)| {
                (trim_whitespace(condition), std::slice::from_ref(size))
            });
            let condition = original_source(condition);
            let size = original_source(size);
            unsafe { visit(context, condition.as_ptr(), condition.len(), size.as_ptr(), size.len()) };
        }
        true
    })
}

pub(crate) fn ffi_resolver(callback: ResolveQueryFeature) -> impl Fn(QueryKind, &[u16]) -> Option<(u8, bool)> {
    move |kind, name| {
        // SAFETY: The name slice remains live for the duration of the callback.
        let result = unsafe { callback(kind as u8, name.as_ptr(), name.len()) };
        if result == u16::MAX {
            return None;
        }
        Some(((result & 0xff) as u8, result & 0x100 != 0))
    }
}

fn ffi_supports_evaluator(
    context: *mut c_void,
    callback: EvaluateSupportsFeature,
) -> impl Fn(FfiSupportsFeatureKind, &[u16]) -> bool {
    move |kind, value| {
        let value = FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: value.as_ptr(),
            length: value.len(),
        };
        // SAFETY: The UTF-16 slice remains live for the duration of the callback.
        unsafe { callback(context, kind, value) }
    }
}

fn reevaluate_supports_features<E>(expression: &mut Expression, evaluate_feature: &E)
where
    E: Fn(FfiSupportsFeatureKind, &[u16]) -> bool,
{
    match expression {
        Expression::Not(child) | Expression::InParens(child) => {
            reevaluate_supports_features(child, evaluate_feature);
        }
        Expression::And(children) | Expression::Or(children) => {
            for child in children {
                reevaluate_supports_features(child, evaluate_feature);
            }
        }
        Expression::SupportsFeature(feature) => {
            let (kind, value, matches) = match feature {
                SupportsFeature::Declaration { components, matches } => (
                    FfiSupportsFeatureKind::Declaration,
                    supports_components_source(components),
                    matches,
                ),
                SupportsFeature::Selector { components, matches } => (
                    FfiSupportsFeatureKind::Selector,
                    supports_components_source(components),
                    matches,
                ),
                SupportsFeature::FontTech { name, matches } => {
                    (FfiSupportsFeatureKind::FontTech, name.as_ref().to_vec(), matches)
                }
                SupportsFeature::FontFormat { name, matches } => {
                    (FfiSupportsFeatureKind::FontFormat, name.as_ref().to_vec(), matches)
                }
                SupportsFeature::AtRule { name, matches } => {
                    (FfiSupportsFeatureKind::AtRule, name.as_ref().to_vec(), matches)
                }
                SupportsFeature::Env { name, matches } => {
                    (FfiSupportsFeatureKind::Env, name.as_ref().to_vec(), matches)
                }
            };
            *matches = evaluate_feature(kind, &value);
        }
        _ => {}
    }
}

/// Parses a media-query list and visits each resulting retained query tree.
///
/// # Safety
/// The source pointers must identify readable storage for the duration of the call. The callback
/// must be valid and may retain a query handle with `css_query_ref`.
#[unsafe(no_mangle)]
#[allow(clippy::arc_with_non_send_sync)]
pub unsafe extern "C" fn rust_visit_media_query_list(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
    context: *mut c_void,
    visit: VisitQueryHandle,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return false;
        };
        let Some(queries) = parse_media_query_list(source, &ffi_resolver(resolve_feature)) else {
            return false;
        };
        for query in queries {
            let handle = Arc::new(FfiQueryHandle {
                tree: QueryTree::MediaQuery(query),
            });
            unsafe { visit(context, Arc::as_ptr(&handle)) };
        }
        true
    })
}

#[allow(clippy::arc_with_non_send_sync)]
fn create_expression_handle(expression: Expression, kind: QueryKind) -> *const FfiQueryHandle {
    Arc::into_raw(Arc::new(FfiQueryHandle {
        tree: QueryTree::Expression { expression, kind },
    }))
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_media_condition(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Some(values) = components_from_source(source) else {
            return std::ptr::null();
        };
        let Some(expression) = parse_media_condition(&values, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null();
        };
        create_expression_handle(expression, QueryKind::Media)
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_media_feature(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Some(expression) = parse_media_feature_from_source(source, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null();
        };
        create_expression_handle(expression, QueryKind::Media)
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_supports_condition(
    source: FfiUtf16View,
    context: *mut c_void,
    evaluate_feature: EvaluateSupportsFeature,
) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Some(expression) = parse_supports_condition(source, &ffi_supports_evaluator(context, evaluate_feature))
        else {
            return std::ptr::null();
        };
        create_expression_handle(expression, QueryKind::Supports)
    })
}

/// Re-evaluates the features in an already parsed supports condition and returns an owned handle.
///
/// This keeps feature evaluation at the C++ rule-conversion point, after preceding namespace rules
/// have been installed, without tokenizing and parsing the supports condition again.
///
/// # Safety
/// `handle` must point to a live supports-query handle and the callback must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_reevaluate_supports_condition(
    handle: *const FfiQueryHandle,
    context: *mut c_void,
    evaluate_feature: EvaluateSupportsFeature,
) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        let QueryTree::Expression {
            expression,
            kind: QueryKind::Supports,
        } = &unsafe { &*handle }.tree
        else {
            return std::ptr::null();
        };
        let mut expression = expression.clone();
        reevaluate_supports_features(&mut expression, &ffi_supports_evaluator(context, evaluate_feature));
        create_expression_handle(expression, QueryKind::Supports)
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_supports_declaration(
    source: FfiUtf16View,
    context: *mut c_void,
    evaluate_feature: EvaluateSupportsFeature,
) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Some(expression) =
            parse_supports_declaration_from_source(source, &ffi_supports_evaluator(context, evaluate_feature))
        else {
            return std::ptr::null();
        };
        create_expression_handle(expression, QueryKind::Supports)
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_style_query(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null();
        };
        let Some(expression) = parse_style_query_from_source(source, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null();
        };
        create_expression_handle(expression, QueryKind::Style)
    })
}

/// Parses a container-condition list and visits each condition name and optional retained tree.
///
/// # Safety
/// The source pointers must identify readable storage for the duration of the call. The callback
/// must be valid and may retain a query handle with `css_query_ref`.
#[unsafe(no_mangle)]
#[allow(clippy::arc_with_non_send_sync)]
pub unsafe extern "C" fn rust_visit_container_condition_list(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
    context: *mut c_void,
    visit: VisitContainerCondition,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return false;
        };
        let Some(conditions) = parse_container_condition_list(source, &ffi_resolver(resolve_feature)) else {
            return false;
        };
        for condition in conditions {
            let query_handle = condition.query.map(|expression| {
                Arc::new(FfiQueryHandle {
                    tree: QueryTree::Expression {
                        expression,
                        kind: QueryKind::Size,
                    },
                })
            });
            let (name, has_name) = condition.name.as_deref().map_or((&[][..], false), |name| (name, true));
            unsafe {
                visit(
                    context,
                    name.as_ptr(),
                    name.len(),
                    has_name,
                    query_handle.as_ref().map_or(std::ptr::null(), Arc::as_ptr),
                );
            };
        }
        true
    })
}

/// Adds one strong reference to a borrowed query handle.
///
/// # Safety
/// `handle` must be null or point to a live query handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_ref(handle: *const FfiQueryHandle) -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        if !handle.is_null() {
            unsafe { Arc::increment_strong_count(handle) };
        }
        handle
    })
}

/// Releases one strong reference to a query handle.
///
/// # Safety
/// `handle` must be null or own one strong reference to a live query handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_unref(handle: *const FfiQueryHandle) {
    crate::abort_on_panic(|| {
        if !handle.is_null() {
            unsafe { Arc::decrement_strong_count(handle) };
        }
    });
}

#[unsafe(no_mangle)]
#[allow(clippy::arc_with_non_send_sync)]
pub extern "C" fn css_query_create_not_all() -> *const FfiQueryHandle {
    crate::abort_on_panic(|| {
        Arc::into_raw(Arc::new(FfiQueryHandle {
            tree: QueryTree::MediaQuery(invalid_media_query()),
        }))
    })
}

/// Serializes a retained media query without changing its UTF-16 representation.
///
/// # Safety
/// `handle` must point to a live media-query handle. The callback must be valid and may only
/// retain a copy of the borrowed UTF-16 slice.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_serialize_media_query(
    handle: *const FfiQueryHandle,
    context: *mut c_void,
    visit: VisitQuerySerialization,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return false;
        };
        let QueryTree::MediaQuery(query) = &handle.tree else {
            return false;
        };
        let serialized = serialize_media_query(query);
        unsafe { visit(context, serialized.as_ptr(), serialized.len()) };
        true
    })
}

/// Evaluates a retained media query against an immutable feature snapshot.
///
/// # Safety
/// `handle` must point to a live media-query handle. The environment slices and optional length
/// resolution context must remain readable for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_evaluate_media(
    handle: *const FfiQueryHandle,
    environment: FfiMediaEnvironment,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return false;
        };
        let QueryTree::MediaQuery(query) = &handle.tree else {
            return false;
        };
        let values = if environment.value_count == 0 {
            &[]
        } else {
            if environment.values.is_null() {
                return false;
            }
            unsafe { std::slice::from_raw_parts(environment.values, environment.value_count) }
        };
        let length_context = unsafe {
            environment
                .length_resolution_context
                .cast::<FfiLengthResolutionContext>()
                .as_ref()
        };
        evaluate_media_query(query, values, length_context) == MatchResult::True
    })
}

/// Evaluates a retained standalone media condition against an immutable feature snapshot.
///
/// # Safety
/// `handle` must point to a live expression handle. The environment slices and optional length
/// resolution context must remain readable for the duration of this call. Returns 3 when the
/// handle does not contain a media expression, allowing the caller to use another evaluator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_evaluate_media_condition(
    handle: *const FfiQueryHandle,
    environment: FfiMediaEnvironment,
) -> u8 {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return 3;
        };
        let QueryTree::Expression {
            expression,
            kind: QueryKind::Media,
        } = &handle.tree
        else {
            return 3;
        };
        let values = if environment.value_count == 0 {
            &[]
        } else {
            if environment.values.is_null() {
                return MatchResult::False as u8;
            }
            unsafe { std::slice::from_raw_parts(environment.values, environment.value_count) }
        };
        let length_context = unsafe {
            environment
                .length_resolution_context
                .cast::<FfiLengthResolutionContext>()
                .as_ref()
        };
        evaluate_media_expression(expression, values, length_context) as u8
    })
}

/// Evaluates a retained supports condition whose feature results were captured while parsing.
/// Returns 3 when the handle does not contain a supports expression.
///
/// # Safety
/// `handle` must point to a live supports-expression handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_evaluate_supports(handle: *const FfiQueryHandle) -> u8 {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return 3;
        };
        let QueryTree::Expression {
            expression,
            kind: QueryKind::Supports,
        } = &handle.tree
        else {
            return 3;
        };
        evaluate_supports_expression(expression) as u8
    })
}

/// Returns the query-container capabilities needed by a retained container condition.
///
/// # Safety
/// `handle` must point to a live container-expression handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_container_requirements(handle: *const FfiQueryHandle) -> u8 {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return CONTAINER_QUERY_HAS_UNKNOWN_FEATURE;
        };
        let QueryTree::Expression {
            expression,
            kind: QueryKind::Size,
        } = &handle.tree
        else {
            return CONTAINER_QUERY_HAS_UNKNOWN_FEATURE;
        };
        container_requirements(expression)
    })
}

/// Evaluates a retained container condition against immutable size facts and style callbacks.
///
/// # Safety
/// `handle` must point to a live container-expression handle. All pointers and callbacks in
/// `facts` must remain valid for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_evaluate_container(handle: *const FfiQueryHandle, facts: FfiContainerFacts) -> u8 {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return MatchResult::Unknown as u8;
        };
        let QueryTree::Expression { expression, kind } = &handle.tree else {
            return MatchResult::Unknown as u8;
        };
        if !matches!(kind, QueryKind::Size | QueryKind::Style) {
            return MatchResult::Unknown as u8;
        }
        if !facts.container_available {
            return MatchResult::Unknown as u8;
        }
        let length_context = unsafe {
            facts
                .length_resolution_context
                .cast::<FfiLengthResolutionContext>()
                .as_ref()
        };
        evaluate_container_expression(expression, &facts, length_context) as u8
    })
}

/// Serializes a retained query condition without changing its UTF-16 representation.
///
/// # Safety
/// `handle` must point to a live expression handle. The callback must be valid and may only
/// retain a copy of the borrowed UTF-16 slice.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn css_query_serialize_condition(
    handle: *const FfiQueryHandle,
    context: *mut c_void,
    visit: VisitQuerySerialization,
) -> bool {
    crate::abort_on_panic(|| {
        let Some(handle) = (unsafe { handle.as_ref() }) else {
            return false;
        };
        let QueryTree::Expression { expression, kind } = &handle.tree else {
            return false;
        };
        let mut sink = TextSink::new();
        serialize_expression(&mut sink, expression, *kind);
        let serialized = sink.into_utf16();
        unsafe { visit(context, serialized.as_ptr(), serialized.len()) };
        true
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resolver(kind: QueryKind, name: &[u16]) -> Option<(u8, bool)> {
        if kind == QueryKind::Style && name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) {
            return Some((5, false));
        }
        let names: &[(&[u8], u8, bool)] = match kind {
            QueryKind::Media => &[(b"width", 1, true), (b"orientation", 2, false)],
            QueryKind::Supports => &[],
            QueryKind::Size => &[(b"width", 3, true), (b"orientation", 4, false)],
            QueryKind::Style => &[],
        };
        names
            .iter()
            .find(|(expected, _, _)| equals_ascii_case_insensitive(name, expected))
            .map(|(_, id, range)| (*id, *range))
    }

    fn parse_single_container_query(source: &[u8]) -> Option<Expression> {
        let mut conditions = parse_container_condition_list(source, &resolver)?;
        if conditions.len() != 1 || conditions[0].name.is_some() {
            return None;
        }
        conditions.pop()?.query
    }

    #[test]
    fn parses_media_query_lists_and_replaces_invalid_queries() {
        let queries = parse_media_query_list(
            b"screen and (width >= 600px), (orientation: landscape), not or".as_slice(),
            &resolver,
        )
        .unwrap();
        assert_eq!(queries.len(), 3);
        assert!(queries[0].valid);
        assert!(queries[1].valid);
        assert!(!queries[2].valid);
    }

    #[test]
    fn parses_media_boolean_operators_and_ranges() {
        let values = components_from_source(b"(400px < width <= 800px) and (not (orientation))".as_slice()).unwrap();
        assert!(matches!(
            parse_media_condition(&values, &resolver),
            Some(Expression::And(_))
        ));
        let values = components_from_source(b"(width) and (orientation) or (width)".as_slice()).unwrap();
        assert!(parse_media_condition(&values, &resolver).is_none());
        assert!(!parse_media_query_list(b"(not (width) and (orientation))".as_slice(), &resolver).unwrap()[0].valid);
    }

    #[test]
    fn parses_supports_features() {
        let supported = |_, _: &[u16]| true;
        assert!(matches!(
            parse_supports_condition(b"(display: grid) and selector(:has(*))".as_slice(), &supported),
            Some(Expression::And(_))
        ));
        assert!(matches!(
            parse_supports_condition(b"font-tech(color-COLRv1)".as_slice(), &supported),
            Some(Expression::SupportsFeature(SupportsFeature::FontTech { .. }))
        ));
        assert!(
            parse_supports_condition(
                b"(display: grid) or (color: red) and (width: 1px)".as_slice(),
                &supported
            )
            .is_none()
        );
        assert!(parse_supports_condition(b"(--: a)".as_slice(), &supported).is_some());
        assert!(parse_supports_condition(b"(display : grid)".as_slice(), &supported).is_some());
        assert!(parse_supports_declaration_from_source(b"display : grid".as_slice(), &supported).is_some());

        let mut condition =
            parse_supports_condition(b"(display: grid) and selector(:has(*))".as_slice(), &|_, _| false).unwrap();
        assert_eq!(evaluate_supports_expression(&condition), MatchResult::False);
        reevaluate_supports_features(&mut condition, &supported);
        assert_eq!(evaluate_supports_expression(&condition), MatchResult::True);
    }

    #[test]
    fn parses_container_size_and_style_queries() {
        let values = components_from_source(b"--".as_slice()).unwrap();
        assert!(matches!(
            parse_style_range_value(&values),
            Some(StyleRangeValue::Components(_))
        ));
        assert!(matches!(
            parse_single_container_query(b"(width > 10px) and style(--theme: dark)"),
            Some(Expression::And(_))
        ));
        let conditions =
            parse_container_condition_list(b"card (width > 10px), style(--theme: dark)".as_slice(), &resolver).unwrap();
        assert_eq!(conditions.len(), 2);
        assert_eq!(
            conditions[0].name.as_ref().map(AsRef::as_ref),
            Some("card".encode_utf16().collect::<Vec<_>>().as_slice())
        );
        assert!(matches!(
            parse_single_container_query(b"style(1 < --level < 3)"),
            Some(Expression::StyleFunction(_))
        ));
        assert!(matches!(
            parse_single_container_query(b"style(--foo: bar;)"),
            Some(Expression::StyleFunction(_))
        ));
        assert!(matches!(
            parse_single_container_query(b"style(10px < 10em !)"),
            Some(Expression::StyleFunction(_))
        ));
    }

    #[test]
    fn keeps_general_enclosed_forgiving() {
        assert!(matches!(
            parse_supports_condition(b"future(foo bar)".as_slice(), &|_, _| false),
            Some(Expression::GeneralEnclosed {
                result: MatchResult::False,
                ..
            })
        ));
        assert!(matches!(
            parse_single_container_query(b"future(foo bar)"),
            Some(Expression::GeneralEnclosed {
                result: MatchResult::Unknown,
                ..
            })
        ));
    }

    #[test]
    fn serializes_media_feature_values_from_generated_metadata() {
        let serialize = |feature_name: &str, source: &[u8]| {
            let id = MEDIA_FEATURES
                .iter()
                .position(|metadata| metadata.name == feature_name)
                .and_then(|id| u8::try_from(id).ok())
                .unwrap();
            let value = QueryFeatureValue {
                components: components_from_source(source).unwrap(),
            };
            let mut sink = TextSink::new();
            serialize_query_feature_value(&mut sink, &value, id, QueryKind::Media);
            String::from_utf16(&sink.into_utf16()).unwrap()
        };

        assert_eq!(serialize("aspect-ratio", b"1/3"), "1 / 3");
        assert_eq!(serialize("device-aspect-ratio", b"4 / 3"), "4 / 3");
        assert_eq!(serialize("resolution", b"calc(1x + 2x)"), "calc(3dppx)");
    }

    #[test]
    fn container_requirement_bits_cover_every_ffi_capability() {
        let requirement_for_id = |id| container_requirements(&Expression::QueryFeature(QueryFeature::Boolean { id }));
        assert_eq!(
            requirement_for_id(0),
            CONTAINER_QUERY_REQUIRES_WIDTH | CONTAINER_QUERY_REQUIRES_HEIGHT
        );
        assert_eq!(requirement_for_id(1), CONTAINER_QUERY_REQUIRES_BLOCK_SIZE);
        assert_eq!(requirement_for_id(2), CONTAINER_QUERY_REQUIRES_HEIGHT);
        assert_eq!(requirement_for_id(3), CONTAINER_QUERY_REQUIRES_INLINE_SIZE);
        assert_eq!(requirement_for_id(5), CONTAINER_QUERY_REQUIRES_WIDTH);
        assert_eq!(requirement_for_id(u8::MAX), CONTAINER_QUERY_HAS_UNKNOWN_FEATURE);
        assert_eq!(CONTAINER_QUERY_REQUIRES_SCROLL_STATE, 1 << 5);

        let style = Expression::StyleFunction(Box::new(Expression::And(Vec::new())));
        assert_eq!(container_requirements(&style), CONTAINER_QUERY_REQUIRES_STYLE);
    }

    #[test]
    fn rejects_non_supports_handles_without_panicking() {
        let handle = FfiQueryHandle {
            tree: QueryTree::Expression {
                expression: Expression::And(Vec::new()),
                kind: QueryKind::Media,
            },
        };

        assert_eq!(unsafe { css_query_evaluate_supports(std::ptr::null()) }, 3);
        assert_eq!(unsafe { css_query_evaluate_supports(&raw const handle) }, 3);
    }
}
