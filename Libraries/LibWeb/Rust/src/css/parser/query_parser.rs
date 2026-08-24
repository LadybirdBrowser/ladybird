/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::{ParserString, ParserTokenKind, TokenizerInput, tokenize_for_parser};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{
    ComponentKind, ComponentValue, consume_a_list_of_component_values, trim_whitespace,
};
use crate::css::parser::syntax_parser::{FfiSyntaxParse, FfiSyntaxParseData};
use crate::css::parser::token_stream::TokenStream;
use crate::css::parser::value_parser::{equals_ascii_case_insensitive, is_valid_custom_ident};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum QueryKind {
    Media,
    Size,
    Style,
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
    Declaration(Vec<ComponentValue>),
    Selector(Vec<ComponentValue>),
    FontTech(ParserString),
    FontFormat(ParserString),
    AtRule(ParserString),
    Env(ParserString),
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

pub(crate) fn parse_media_query_list<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Vec<MediaQuery>>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    if trim_whitespace(&values).is_empty() {
        return Some(Vec::new());
    }
    let mut result = Vec::new();
    let mut start = 0;
    for index in 0..=values.len() {
        if index == values.len() || values[index].is_comma() {
            result.push(parse_one_media_query(&values[start..index], resolve_feature));
            start = index + 1;
        }
    }
    Some(result)
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

fn parse_supports_feature(stream: &mut TokenStream<'_>) -> Option<Expression> {
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let first = transaction.consume_a_token().clone();
    if let Some(values) = parenthesized_values(&first) {
        let values = trim_whitespace(values);
        if looks_like_supports_declaration(values) && contains_only_any_value(values) {
            transaction.commit();
            return Some(Expression::InParens(Box::new(Expression::SupportsFeature(
                SupportsFeature::Declaration(values.to_vec()),
            ))));
        }
        return None;
    }
    let (name, values) = first.function()?;
    let feature = if equals_ascii_case_insensitive(name, b"selector") {
        SupportsFeature::Selector(values.to_vec())
    } else if equals_ascii_case_insensitive(name, b"font-tech") {
        SupportsFeature::FontTech(single_ident(values)?)
    } else if equals_ascii_case_insensitive(name, b"font-format") {
        SupportsFeature::FontFormat(single_ident(values)?)
    } else if equals_ascii_case_insensitive(name, b"env") {
        SupportsFeature::Env(single_ident(values)?)
    } else if equals_ascii_case_insensitive(name, b"at-rule") {
        let values = trim_whitespace(values);
        let [value] = values else {
            return None;
        };
        let ComponentKind::Token(ParserTokenKind::AtKeyword(name)) = &value.kind else {
            return None;
        };
        SupportsFeature::AtRule(name.clone())
    } else {
        return None;
    };
    transaction.commit();
    Some(Expression::SupportsFeature(feature))
}

pub(crate) fn parse_supports_condition<'a>(source: impl Into<TokenizerInput<'a>>) -> Option<Expression> {
    let values = components_from_source(source)?;
    let mut stream = TokenStream::new(&values);
    let expression = parse_boolean_expression(&mut stream, MatchResult::False, &parse_supports_feature)?;
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(expression)
}

fn parse_supports_declaration_from_source<'a>(source: impl Into<TokenizerInput<'a>>) -> Option<Expression> {
    let values = components_from_source(source)?;
    let trimmed = trim_whitespace(&values);
    (looks_like_supports_declaration(trimmed) && contains_only_any_value(&values))
        .then_some(Expression::SupportsFeature(SupportsFeature::Declaration(values)))
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

pub(crate) fn parse_container_condition_list<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Vec<ContainerCondition>>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    if trim_whitespace(&values).is_empty() {
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

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiQueryValue {
    pub value_type: u8,
    pub source_offset: usize,
    pub source_length: usize,
    pub name_offset: usize,
    pub name_length: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiQueryNode {
    pub node_type: u8,
    pub feature_id: u8,
    pub feature_type: u8,
    pub match_result: u8,
    pub first_comparison: u8,
    pub second_comparison: u8,
    pub name_offset: usize,
    pub name_length: usize,
    pub children_start: usize,
    pub child_count: usize,
    pub values_start: usize,
    pub value_count: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiMediaQuery {
    pub negated: bool,
    pub valid: bool,
    pub has_media_type: bool,
    pub has_condition: bool,
    pub media_type_offset: usize,
    pub media_type_length: usize,
    pub condition: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiQueryParseData {
    pub syntax: FfiSyntaxParseData,
    pub nodes: *const FfiQueryNode,
    pub node_count: usize,
    pub node_indices: *const usize,
    pub node_index_count: usize,
    pub query_values: *const FfiQueryValue,
    pub query_value_count: usize,
    pub media_queries: *const FfiMediaQuery,
    pub media_query_count: usize,
    pub root: usize,
    pub has_root: bool,
}

pub struct FfiQueryParse {
    syntax: FfiSyntaxParse,
    nodes: Vec<FfiQueryNode>,
    node_indices: Vec<usize>,
    query_values: Vec<FfiQueryValue>,
    media_queries: Vec<FfiMediaQuery>,
    root: Option<usize>,
}

impl FfiQueryParse {
    fn new() -> Self {
        Self {
            syntax: FfiSyntaxParse::new(std::ptr::null(), None, false),
            nodes: Vec::new(),
            node_indices: Vec::new(),
            query_values: Vec::new(),
            media_queries: Vec::new(),
            root: None,
        }
    }

    fn append_name(&mut self, name: &[u16]) -> (usize, usize) {
        self.syntax.append_value(name)
    }

    fn append_query_value(&mut self, value: &StyleRangeValue) -> usize {
        let (value_type, source_offset, source_length, name_offset, name_length) = match value {
            StyleRangeValue::Property(name) => {
                let (name_offset, name_length) = self.append_name(name.as_ref());
                (1, 0, 0, name_offset, name_length)
            }
            StyleRangeValue::Components(components) => {
                let source = crate::css::serialize::serialize_component_values_to_utf16(
                    components,
                    crate::css::parser::component_value::ComponentSerializationMode::Normalized,
                );
                let (source_offset, source_length) = self.syntax.append_value(&source);
                (0, source_offset, source_length, 0, 0)
            }
        };
        let index = self.query_values.len();
        self.query_values.push(FfiQueryValue {
            value_type,
            source_offset,
            source_length,
            name_offset,
            name_length,
        });
        index
    }

    fn append_feature_value(&mut self, value: &QueryFeatureValue) -> usize {
        let source = crate::css::serialize::serialize_component_values_to_utf16(
            &value.components,
            crate::css::parser::component_value::ComponentSerializationMode::PreserveNumericSource,
        );
        let (source_offset, source_length) = self.syntax.append_value(&source);
        let index = self.query_values.len();
        self.query_values.push(FfiQueryValue {
            value_type: 0,
            source_offset,
            source_length,
            name_offset: 0,
            name_length: 0,
        });
        index
    }

    fn append_children(&mut self, children: &[Expression]) -> (usize, usize) {
        let indices = children
            .iter()
            .map(|child| self.append_expression(child))
            .collect::<Vec<_>>();
        let start = self.node_indices.len();
        self.node_indices.extend(indices);
        (start, children.len())
    }

    fn append_expression(&mut self, expression: &Expression) -> usize {
        let mut node = FfiQueryNode {
            node_type: 0,
            feature_id: 0,
            feature_type: 0,
            match_result: 0,
            first_comparison: 0,
            second_comparison: 0,
            name_offset: 0,
            name_length: 0,
            children_start: 0,
            child_count: 0,
            values_start: 0,
            value_count: 0,
        };
        match expression {
            Expression::Not(child) => {
                node.node_type = 0;
                let child = self.append_expression(child);
                node.children_start = self.node_indices.len();
                node.child_count = 1;
                self.node_indices.push(child);
            }
            Expression::And(children) => {
                node.node_type = 1;
                (node.children_start, node.child_count) = self.append_children(children);
            }
            Expression::Or(children) => {
                node.node_type = 2;
                (node.children_start, node.child_count) = self.append_children(children);
            }
            Expression::InParens(child) => {
                node.node_type = 3;
                let child = self.append_expression(child);
                node.children_start = self.node_indices.len();
                node.child_count = 1;
                self.node_indices.push(child);
            }
            Expression::GeneralEnclosed { component, result } => {
                node.node_type = 4;
                node.match_result = *result as u8;
                (node.name_offset, node.name_length) = self
                    .syntax
                    .append_value(&component.original_source_text.iter().collect::<Vec<_>>());
            }
            Expression::GeneralEnclosedValues { components, result } => {
                node.node_type = 4;
                node.match_result = *result as u8;
                let source = components
                    .iter()
                    .flat_map(|component| component.original_source_text.iter())
                    .collect::<Vec<_>>();
                (node.name_offset, node.name_length) = self.syntax.append_value(&source);
            }
            Expression::QueryFeature(feature) => {
                node.node_type = 5;
                match feature {
                    QueryFeature::Boolean { id } => {
                        node.feature_id = *id;
                        node.feature_type = 0;
                    }
                    QueryFeature::Plain { id, name_type, value } => {
                        node.feature_id = *id;
                        node.feature_type = match name_type {
                            FeatureNameType::Normal => 1,
                            FeatureNameType::Min => 2,
                            FeatureNameType::Max => 3,
                        };
                        node.values_start = self.append_feature_value(value);
                        node.value_count = 1;
                    }
                    QueryFeature::Range { id, left, right } => {
                        node.feature_id = *id;
                        node.feature_type = 4;
                        let start = self.query_values.len();
                        if let Some((value, comparison)) = left {
                            node.first_comparison = *comparison as u8;
                            self.append_feature_value(value);
                        }
                        if let Some((comparison, value)) = right {
                            if left.is_some() {
                                node.second_comparison = *comparison as u8;
                            } else {
                                node.first_comparison = *comparison as u8;
                            }
                            self.append_feature_value(value);
                        }
                        node.values_start = start;
                        node.value_count = self.query_values.len() - start;
                        node.match_result = u8::from(left.is_some()) | (u8::from(right.is_some()) << 1);
                    }
                }
            }
            Expression::SupportsFeature(feature) => {
                node.node_type = match feature {
                    SupportsFeature::Declaration(_) => 6,
                    SupportsFeature::Selector(_) => 7,
                    SupportsFeature::FontTech(_) => 8,
                    SupportsFeature::FontFormat(_) => 9,
                    SupportsFeature::AtRule(_) => 10,
                    SupportsFeature::Env(_) => 11,
                };
                match feature {
                    SupportsFeature::Declaration(components) | SupportsFeature::Selector(components) => {
                        let source = components
                            .iter()
                            .flat_map(|component| component.original_source_text.iter())
                            .collect::<Vec<_>>();
                        let (source_offset, source_length) = self.syntax.append_value(&source);
                        node.values_start = self.query_values.len();
                        node.value_count = 1;
                        self.query_values.push(FfiQueryValue {
                            value_type: 0,
                            source_offset,
                            source_length,
                            name_offset: 0,
                            name_length: 0,
                        });
                    }
                    SupportsFeature::FontTech(name)
                    | SupportsFeature::FontFormat(name)
                    | SupportsFeature::AtRule(name)
                    | SupportsFeature::Env(name) => {
                        (node.name_offset, node.name_length) = self.append_name(name.as_ref());
                    }
                }
            }
            Expression::StyleFunction(child) => {
                node.node_type = 12;
                let child = self.append_expression(child);
                node.children_start = self.node_indices.len();
                node.child_count = 1;
                self.node_indices.push(child);
            }
            Expression::StyleFeature(feature) => match feature {
                StyleFeature::Boolean(name) => {
                    node.node_type = 13;
                    (node.name_offset, node.name_length) = self.append_name(name.as_ref());
                }
                StyleFeature::Plain {
                    name,
                    value,
                    original_source,
                    original_value_source,
                } => {
                    node.node_type = 14;
                    (node.name_offset, node.name_length) = self.append_name(name.as_ref());
                    node.values_start = self.append_query_value(&StyleRangeValue::Components(value.clone()));
                    node.value_count = 1;
                    (node.children_start, node.child_count) = self.append_name(original_source.as_ref());
                    let (offset, length) = self.append_name(original_value_source.as_ref());
                    self.query_values[node.values_start].name_offset = offset;
                    self.query_values[node.values_start].name_length = length;
                }
                StyleFeature::Range {
                    left,
                    left_comparison,
                    middle,
                    right,
                } => {
                    node.node_type = 15;
                    node.first_comparison = *left_comparison as u8;
                    node.values_start = self.query_values.len();
                    self.append_query_value(left);
                    self.append_query_value(middle);
                    if let Some((comparison, right)) = right {
                        node.second_comparison = *comparison as u8;
                        self.append_query_value(right);
                    }
                    node.value_count = self.query_values.len() - node.values_start;
                }
            },
        }
        let index = self.nodes.len();
        self.nodes.push(node);
        index
    }

    fn append_media_query(&mut self, query: &MediaQuery) {
        let (media_type_offset, media_type_length) = query
            .media_type
            .as_ref()
            .map_or((0, 0), |name| self.append_name(name.as_ref()));
        let condition = query
            .condition
            .as_ref()
            .map(|condition| self.append_expression(condition));
        self.media_queries.push(FfiMediaQuery {
            negated: query.negated,
            valid: query.valid,
            has_media_type: query.media_type.is_some(),
            has_condition: condition.is_some(),
            media_type_offset,
            media_type_length,
            condition: condition.unwrap_or(0),
        });
    }

    fn append_container_condition(&mut self, condition: &ContainerCondition) {
        let (name_offset, name_length) = condition
            .name
            .as_ref()
            .map_or((0, 0), |name| self.append_name(name.as_ref()));
        let query = condition.query.as_ref().map(|query| self.append_expression(query));
        self.media_queries.push(FfiMediaQuery {
            negated: false,
            valid: true,
            has_media_type: condition.name.is_some(),
            has_condition: query.is_some(),
            media_type_offset: name_offset,
            media_type_length: name_length,
            condition: query.unwrap_or(0),
        });
    }

    fn data(&self) -> FfiQueryParseData {
        FfiQueryParseData {
            syntax: self.syntax.data(),
            nodes: self.nodes.as_ptr(),
            node_count: self.nodes.len(),
            node_indices: self.node_indices.as_ptr(),
            node_index_count: self.node_indices.len(),
            query_values: self.query_values.as_ptr(),
            query_value_count: self.query_values.len(),
            media_queries: self.media_queries.as_ptr(),
            media_query_count: self.media_queries.len(),
            root: self.root.unwrap_or(0),
            has_root: self.root.is_some(),
        }
    }
}

type ResolveQueryFeature = unsafe extern "C" fn(u8, *const u16, usize) -> u16;

type VisitSizesAttributeEntry = unsafe extern "C" fn(*mut c_void, *const u16, usize, *const u16, usize);

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

fn ffi_resolver(callback: ResolveQueryFeature) -> impl Fn(QueryKind, &[u16]) -> Option<(u8, bool)> {
    move |kind, name| {
        // SAFETY: The name slice remains live for the duration of the callback.
        let result = unsafe { callback(kind as u8, name.as_ptr(), name.len()) };
        if result == u16::MAX {
            return None;
        }
        Some(((result & 0xff) as u8, result & 0x100 != 0))
    }
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_media_query_list(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(queries) = parse_media_query_list(source, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        for query in &queries {
            parse.append_media_query(query);
        }
        Box::into_raw(Box::new(parse))
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_media_condition(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(values) = components_from_source(source) else {
            return std::ptr::null_mut();
        };
        let Some(expression) = parse_media_condition(&values, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        parse.root = Some(parse.append_expression(&expression));
        Box::into_raw(Box::new(parse))
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_media_feature(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(expression) = parse_media_feature_from_source(source, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        parse.root = Some(parse.append_expression(&expression));
        Box::into_raw(Box::new(parse))
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_supports_condition(source: FfiUtf16View) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(expression) = parse_supports_condition(source) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        parse.root = Some(parse.append_expression(&expression));
        Box::into_raw(Box::new(parse))
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_supports_declaration(source: FfiUtf16View) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(expression) = parse_supports_declaration_from_source(source) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        parse.root = Some(parse.append_expression(&expression));
        Box::into_raw(Box::new(parse))
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_style_query(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(expression) = parse_style_query_from_source(source, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        parse.root = Some(parse.append_expression(&expression));
        Box::into_raw(Box::new(parse))
    })
}

/// # Safety
/// The source pointers must identify readable storage for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_container_condition_list(
    source: FfiUtf16View,
    resolve_feature: ResolveQueryFeature,
) -> *mut FfiQueryParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(conditions) = parse_container_condition_list(source, &ffi_resolver(resolve_feature)) else {
            return std::ptr::null_mut();
        };
        let mut parse = FfiQueryParse::new();
        for condition in &conditions {
            parse.append_container_condition(condition);
        }
        Box::into_raw(Box::new(parse))
    })
}

/// Returns borrowed arena slices which remain live until `rust_query_parse_free`.
///
/// # Safety
/// `parse` must be a live pointer returned by a query parsing function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_query_parse_data(parse: *const FfiQueryParse) -> FfiQueryParseData {
    crate::abort_on_panic(|| unsafe { &*parse }.data())
}

/// # Safety
/// `parse` must be null or a live pointer returned by a query parsing function, and may be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_query_parse_free(parse: *mut FfiQueryParse) {
    crate::abort_on_panic(|| {
        if !parse.is_null() {
            drop(unsafe { Box::from_raw(parse) });
        }
    });
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
        assert!(matches!(
            parse_supports_condition(b"(display: grid) and selector(:has(*))".as_slice()),
            Some(Expression::And(_))
        ));
        assert!(matches!(
            parse_supports_condition(b"font-tech(color-COLRv1)".as_slice()),
            Some(Expression::SupportsFeature(SupportsFeature::FontTech(_)))
        ));
        assert!(parse_supports_condition(b"(display: grid) or (color: red) and (width: 1px)".as_slice()).is_none());
        assert!(parse_supports_condition(b"(--: a)".as_slice()).is_some());
        assert!(parse_supports_condition(b"(display : grid)".as_slice()).is_some());
        assert!(parse_supports_declaration_from_source(b"display : grid".as_slice()).is_some());
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
            parse_supports_condition(b"future(foo bar)".as_slice()),
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
}
