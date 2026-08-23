/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::{ParserString, ParserTokenKind, TokenizerInput, tokenize_for_parser};
use crate::css::parser::component_value::{ComponentKind, ComponentValue, consume_a_list_of_component_values};
use crate::css::parser::token_stream::TokenStream;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum QueryKind {
    Media,
    Size,
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

fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left).is_ok_and(|left| left.eq_ignore_ascii_case(&right)))
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

fn trim_whitespace(values: &[ComponentValue]) -> &[ComponentValue] {
    let start = values
        .iter()
        .position(|value| !value.is_whitespace())
        .unwrap_or(values.len());
    let end = values
        .iter()
        .rposition(|value| !value.is_whitespace())
        .map_or(start, |end| end + 1);
    &values[start..end]
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
        if let Some(expression) = parse_boolean_expression(&mut child_stream, result_for_general_enclosed, parse_test)
            && child_stream.is_empty()
        {
            transaction.commit();
            return Some(Expression::InParens(Box::new(expression)));
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

fn parse_supports_feature(stream: &mut TokenStream<'_>) -> Option<Expression> {
    let mut transaction = stream.begin_transaction();
    transaction.discard_whitespace();
    let first = transaction.consume_a_token().clone();
    if let Some(values) = parenthesized_values(&first) {
        if !trim_whitespace(values).is_empty() && contains_only_any_value(values) {
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

fn parse_style_range_value(values: &[ComponentValue]) -> Option<StyleRangeValue> {
    let values = trim_whitespace(values);
    if values.is_empty() || !contains_only_any_value(values) {
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

fn parse_style_feature(stream: &mut TokenStream<'_>) -> Option<Expression> {
    let values = trim_whitespace(&stream.values[stream.position..]);
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
        let value = trim_whitespace(&values[colon + 1..]);
        if value.is_empty()
            || !contains_only_any_value(value)
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
        }));
    }

    let name = single_ident(values)?;
    stream.position = stream.values.len();
    Some(Expression::StyleFeature(StyleFeature::Boolean(name)))
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
        let expression = parse_boolean_expression(&mut inner, MatchResult::Unknown, &parse_style_feature)?;
        inner.discard_whitespace();
        if inner.has_next_token() {
            return None;
        }
        transaction.commit();
        return Some(Expression::StyleFunction(Box::new(expression)));
    }
    None
}

pub(crate) fn parse_container_query<'a, R>(
    source: impl Into<TokenizerInput<'a>>,
    resolve_feature: &R,
) -> Option<Expression>
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let values = components_from_source(source)?;
    let mut stream = TokenStream::new(&values);
    let expression = parse_boolean_expression(&mut stream, MatchResult::Unknown, &|stream| {
        parse_container_feature(stream, resolve_feature)
    })?;
    stream.discard_whitespace();
    (!stream.has_next_token()).then_some(expression)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resolver(kind: QueryKind, name: &[u16]) -> Option<(u8, bool)> {
        let names: &[(&[u8], u8, bool)] = match kind {
            QueryKind::Media => &[(b"width", 1, true), (b"orientation", 2, false)],
            QueryKind::Size => &[(b"width", 3, true), (b"orientation", 4, false)],
        };
        names
            .iter()
            .find(|(expected, _, _)| equals_ascii_case_insensitive(name, expected))
            .map(|(_, id, range)| (*id, *range))
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
    }

    #[test]
    fn parses_container_size_and_style_queries() {
        let values = components_from_source(b"--".as_slice()).unwrap();
        assert!(matches!(
            parse_style_range_value(&values),
            Some(StyleRangeValue::Components(_))
        ));
        assert!(matches!(
            parse_container_query(b"(width > 10px) and style(--theme: dark)".as_slice(), &resolver),
            Some(Expression::And(_))
        ));
        assert!(matches!(
            parse_container_query(b"style(1 < --level < 3)".as_slice(), &resolver),
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
            parse_container_query(b"future(foo bar)".as_slice(), &resolver),
            Some(Expression::GeneralEnclosed {
                result: MatchResult::Unknown,
                ..
            })
        ));
    }
}
