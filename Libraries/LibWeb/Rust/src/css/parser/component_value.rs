/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fmt::Write;

use crate::css::css_tokenizer::{
    ParserSource, ParserString, ParserToken, ParserTokenKind, SmallParserTokenList, SourcePosition, TextDetacher,
};
use crate::css::serialize::{StringUnits, TextSink, serialize_a_string, serialize_an_identifier};
use smallvec::SmallVec;

const MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH: usize = 256;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct ComponentValueList<T = Vec<ComponentValue>> {
    values: T,
    pub(crate) token_count: usize,
}

pub(crate) type ComponentValueSlice<'a> = ComponentValueList<&'a [ComponentValue]>;

impl<T: AsRef<[ComponentValue]>> ComponentValueList<T> {
    pub(crate) fn new(values: T) -> Self {
        let token_count = values.as_ref().iter().map(ComponentValue::token_count).sum();
        Self { values, token_count }
    }

    pub(crate) fn with_token_count(values: T, token_count: usize) -> Self {
        Self { values, token_count }
    }

    pub(crate) fn as_slice(&self) -> ComponentValueSlice<'_> {
        ComponentValueSlice::with_token_count(self.values.as_ref(), self.token_count)
    }
}

impl ComponentValueList {
    pub(crate) fn into_vec(self) -> Vec<ComponentValue> {
        self.values
    }

    pub(crate) fn into_boxed(self) -> ComponentValueList<Box<[ComponentValue]>> {
        ComponentValueList {
            values: self.values.into_boxed_slice(),
            token_count: self.token_count,
        }
    }
}

impl<T: AsRef<[ComponentValue]>> std::ops::Deref for ComponentValueList<T> {
    type Target = [ComponentValue];

    fn deref(&self) -> &Self::Target {
        self.values.as_ref()
    }
}

impl<T: AsRef<[ComponentValue]>> From<T> for ComponentValueList<T> {
    fn from(values: T) -> Self {
        Self::new(values)
    }
}

impl<'a, T: AsRef<[ComponentValue]>> From<&'a ComponentValueList<T>> for ComponentValueSlice<'a> {
    fn from(values: &'a ComponentValueList<T>) -> Self {
        values.as_slice()
    }
}

impl ComponentValueSlice<'_> {
    pub(crate) fn trim_whitespace(self) -> Self {
        let values = trim_whitespace(self.values);
        Self {
            values,
            token_count: self.token_count - (self.values.len() - values.len()),
        }
    }

    pub(crate) fn trim_end_whitespace(mut self) -> Self {
        while self.values.last().is_some_and(ComponentValue::is_whitespace) {
            self.values = &self.values[..self.values.len() - 1];
            self.token_count -= 1;
        }
        self
    }

    pub(crate) fn to_owned(self) -> ComponentValueList {
        ComponentValueList::with_token_count(self.values.to_vec(), self.token_count)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ComponentKind {
    Token(ParserTokenKind),
    Function {
        name: ParserString,
        values: ComponentValueList<Box<[ComponentValue]>>,
    },
    SimpleBlock {
        opening: ParserTokenKind,
        values: ComponentValueList<Box<[ComponentValue]>>,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ComponentValue {
    pub kind: ComponentKind,
    pub original_source_text: ParserSource,
    pub opening_source_length: usize,
    pub closing_source_length: usize,
    pub start_position: SourcePosition,
    pub end_position: SourcePosition,
}

impl ComponentValue {
    /// Copies the value's text out of the sheet's source and token storage, for a value that outlives its parse.
    /// Otherwise the value keeps the text of the whole sheet alive.
    pub(crate) fn detach(&mut self, detacher: &mut TextDetacher) {
        self.original_source_text.detach(detacher);
        match &mut self.kind {
            ComponentKind::Token(token) => token.detach(detacher),
            ComponentKind::Function { name, values } => {
                name.detach(detacher);
                values.values.iter_mut().for_each(|value| value.detach(detacher));
            }
            ComponentKind::SimpleBlock { opening, values } => {
                opening.detach(detacher);
                values.values.iter_mut().for_each(|value| value.detach(detacher));
            }
        }
    }

    pub(crate) fn token_count(&self) -> usize {
        match &self.kind {
            ComponentKind::Token(_) => 1,
            // Include closing delimiters even when the component is closed at EOF.
            ComponentKind::Function { values, .. } | ComponentKind::SimpleBlock { values, .. } => {
                2 + values.token_count
            }
        }
    }

    pub(crate) fn end_of_file() -> Self {
        Self {
            kind: ComponentKind::Token(ParserTokenKind::EndOfFile),
            original_source_text: ParserSource::empty(),
            opening_source_length: 0,
            closing_source_length: 0,
            start_position: SourcePosition::default(),
            end_position: SourcePosition::default(),
        }
    }

    pub(crate) fn is_whitespace(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Whitespace))
    }

    pub(crate) fn is_comma(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Comma))
    }

    pub(crate) fn is_colon(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Colon))
    }

    pub(crate) fn is(&self, expected: &ParserTokenKind) -> bool {
        matches!(&self.kind, ComponentKind::Token(kind) if is_same_token_kind(kind, expected))
    }

    pub(crate) fn is_delim(&self, expected: u8) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Delim(value)) if value == u32::from(expected))
    }

    pub(crate) fn ident(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::Ident(value)) => Some(value.as_ref()),
            _ => None,
        }
    }

    pub(crate) fn string(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::String(value)) => Some(value.as_ref()),
            _ => None,
        }
    }

    pub(crate) fn function(&self) -> Option<(&[u16], &[ComponentValue])> {
        match &self.kind {
            ComponentKind::Function { name, values } => Some((name.as_ref(), values)),
            _ => None,
        }
    }

    pub(crate) fn square_block(&self) -> Option<&[ComponentValue]> {
        match &self.kind {
            ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenSquare,
                values,
            } => Some(values),
            _ => None,
        }
    }
}

pub(crate) fn trim_whitespace(mut values: &[ComponentValue]) -> &[ComponentValue] {
    while values.first().is_some_and(ComponentValue::is_whitespace) {
        values = &values[1..];
    }
    while values.last().is_some_and(ComponentValue::is_whitespace) {
        values = &values[..values.len() - 1];
    }
    values
}

#[derive(Clone, Copy)]
pub(crate) enum ComponentSerializationMode {
    Normalized,
    PreserveNumericSource,
    Retokenize,
}

fn is_numeric(value: &ComponentValue) -> bool {
    matches!(
        value.kind,
        ComponentKind::Token(
            ParserTokenKind::Number { .. } | ParserTokenKind::Percentage { .. } | ParserTokenKind::Dimension { .. }
        )
    )
}

fn is_function_or_paren(value: &ComponentValue) -> bool {
    matches!(
        value.kind,
        ComponentKind::Function { .. }
            | ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenParen,
                ..
            }
    )
}

fn token_needs_comment_between(first: &ComponentValue, second: &ComponentValue) -> bool {
    let second_kind = match &second.kind {
        ComponentKind::Token(kind) => Some(kind),
        _ => None,
    };
    let second_is = |predicate: fn(&ParserTokenKind) -> bool| second_kind.is_some_and(predicate);
    let second_is_common = || {
        second_is(|kind| {
            matches!(
                kind,
                ParserTokenKind::Ident(_)
                    | ParserTokenKind::Url(_)
                    | ParserTokenKind::BadUrl
                    | ParserTokenKind::Number { .. }
                    | ParserTokenKind::Percentage { .. }
                    | ParserTokenKind::Dimension { .. }
                    | ParserTokenKind::Cdc
            )
        })
    };
    let second_is_dash =
        || second_is(|kind| matches!(kind, ParserTokenKind::Delim(value) if *value == u32::from(b'-')));

    match &first.kind {
        ComponentKind::Token(ParserTokenKind::Ident(_)) => {
            is_function_or_paren(second)
                || second_is_dash()
                || second_is_common()
                || second_is(|kind| matches!(kind, ParserTokenKind::OpenParen))
        }
        ComponentKind::Token(
            ParserTokenKind::AtKeyword(_) | ParserTokenKind::Hash { .. } | ParserTokenKind::Dimension { .. },
        ) => matches!(second.kind, ComponentKind::Function { .. }) || second_is_dash() || second_is_common(),
        ComponentKind::Token(ParserTokenKind::Delim(value)) if matches!(*value, 0x23 | 0x2d) => {
            matches!(second.kind, ComponentKind::Function { .. }) || second_is_dash() || second_is_common()
        }
        ComponentKind::Token(ParserTokenKind::Number { .. }) => {
            matches!(second.kind, ComponentKind::Function { .. })
                || second_is_common()
                || second_is(|kind| matches!(kind, ParserTokenKind::Delim(value) if *value == u32::from(b'%')))
        }
        ComponentKind::Token(ParserTokenKind::Delim(value)) if *value == u32::from(b'@') => {
            matches!(second.kind, ComponentKind::Function { .. })
                || second_is_dash()
                || second_is(|kind| {
                    matches!(
                        kind,
                        ParserTokenKind::Ident(_)
                            | ParserTokenKind::Url(_)
                            | ParserTokenKind::BadUrl
                            | ParserTokenKind::Cdc
                    )
                })
        }
        ComponentKind::Token(ParserTokenKind::Delim(value)) if matches!(*value, 0x2e | 0x2b) => second_is(|kind| {
            matches!(
                kind,
                ParserTokenKind::Number { .. } | ParserTokenKind::Percentage { .. } | ParserTokenKind::Dimension { .. }
            )
        }),
        ComponentKind::Token(ParserTokenKind::Delim(value)) if *value == u32::from(b'/') => {
            second_is(|kind| matches!(kind, ParserTokenKind::Delim(value) if *value == u32::from(b'*')))
        }
        _ => false,
    }
}

fn serialize_token(sink: &mut TextSink, kind: &ParserTokenKind) {
    match kind {
        ParserTokenKind::EndOfFile | ParserTokenKind::BadString => {}
        ParserTokenKind::Ident(value) => serialize_an_identifier(sink, &StringUnits::Utf16(value)),
        ParserTokenKind::Function(value) => {
            serialize_an_identifier(sink, &StringUnits::Utf16(value));
            sink.push_ascii("(");
        }
        ParserTokenKind::AtKeyword(value) => {
            sink.push_ascii("@");
            serialize_an_identifier(sink, &StringUnits::Utf16(value));
        }
        ParserTokenKind::Hash { value, is_id } => {
            sink.push_ascii("#");
            if *is_id {
                serialize_an_identifier(sink, &StringUnits::Utf16(value));
            } else {
                value.iter().for_each(|&unit| sink.push_code_unit(unit));
            }
        }
        ParserTokenKind::String(value) => serialize_a_string(sink, &StringUnits::Utf16(value)),
        ParserTokenKind::Url(value) => {
            sink.push_ascii("url(");
            serialize_a_string(sink, &StringUnits::Utf16(value));
            sink.push_ascii(")");
        }
        ParserTokenKind::BadUrl => sink.push_ascii("url()"),
        ParserTokenKind::Delim(value) => sink.push_code_point(*value),
        ParserTokenKind::Number { value, .. } => write!(sink, "{value}").unwrap(),
        ParserTokenKind::Percentage { value, .. } => write!(sink, "{value}%").unwrap(),
        ParserTokenKind::Dimension { value, unit, .. } => {
            write!(sink, "{value}").unwrap();
            unit.iter().for_each(|&code_unit| sink.push_code_unit(code_unit));
        }
        ParserTokenKind::Whitespace => sink.push_ascii(" "),
        ParserTokenKind::Cdo => sink.push_ascii("<!--"),
        ParserTokenKind::Cdc => sink.push_ascii("-->"),
        ParserTokenKind::Colon => sink.push_ascii(":"),
        ParserTokenKind::Semicolon => sink.push_ascii(";"),
        ParserTokenKind::Comma => sink.push_ascii(","),
        ParserTokenKind::OpenSquare => sink.push_ascii("["),
        ParserTokenKind::CloseSquare => sink.push_ascii("]"),
        ParserTokenKind::OpenParen => sink.push_ascii("("),
        ParserTokenKind::CloseParen => sink.push_ascii(")"),
        ParserTokenKind::OpenCurly => sink.push_ascii("{"),
        ParserTokenKind::CloseCurly => sink.push_ascii("}"),
    }
}

fn append_original_source(sink: &mut TextSink, source: &ParserSource) -> bool {
    if source.len() == 0 {
        return false;
    }
    source.iter().for_each(|unit| sink.push_code_unit(unit));
    true
}

fn serialize_component(sink: &mut TextSink, value: &ComponentValue, mode: ComponentSerializationMode) -> bool {
    if matches!(mode, ComponentSerializationMode::PreserveNumericSource) && is_numeric(value) {
        return append_original_source(sink, &value.original_source_text);
    }
    if matches!(mode, ComponentSerializationMode::Retokenize)
        && (is_numeric(value)
            || matches!(
                value.kind,
                ComponentKind::Token(ParserTokenKind::BadString | ParserTokenKind::BadUrl)
            ))
    {
        return append_original_source(sink, &value.original_source_text);
    }

    match &value.kind {
        ComponentKind::Token(kind) => serialize_token(sink, kind),
        ComponentKind::Function { name, values } => {
            if matches!(mode, ComponentSerializationMode::Retokenize) {
                let mut opening = value.original_source_text.to_vec();
                opening.truncate(value.opening_source_length);
                if opening.is_empty() {
                    return false;
                }
                opening.into_iter().for_each(|unit| sink.push_code_unit(unit));
            } else {
                serialize_an_identifier(sink, &StringUnits::Utf16(name));
                sink.push_ascii("(");
            }
            let nested_mode = if matches!(mode, ComponentSerializationMode::PreserveNumericSource) {
                ComponentSerializationMode::Normalized
            } else {
                mode
            };
            if !serialize_component_values_into(sink, values, nested_mode) {
                return false;
            }
            if matches!(mode, ComponentSerializationMode::Retokenize) {
                if value.closing_source_length == 0 {
                    return false;
                }
                let source = value.original_source_text.to_vec();
                source[source.len() - value.closing_source_length..]
                    .iter()
                    .for_each(|&unit| sink.push_code_unit(unit));
            } else {
                sink.push_ascii(")");
            }
        }
        ComponentKind::SimpleBlock { opening, values } => {
            let (normalized_opening, normalized_closing) = match opening {
                ParserTokenKind::OpenSquare => ("[", "]"),
                ParserTokenKind::OpenParen => ("(", ")"),
                ParserTokenKind::OpenCurly => ("{", "}"),
                _ => unreachable!(),
            };
            if matches!(mode, ComponentSerializationMode::Retokenize) {
                let source = value.original_source_text.to_vec();
                if value.opening_source_length == 0 {
                    return false;
                }
                source[..value.opening_source_length]
                    .iter()
                    .for_each(|&unit| sink.push_code_unit(unit));
            } else {
                sink.push_ascii(normalized_opening);
            }
            let nested_mode = if matches!(mode, ComponentSerializationMode::PreserveNumericSource) {
                ComponentSerializationMode::Normalized
            } else {
                mode
            };
            if !serialize_component_values_into(sink, values, nested_mode) {
                return false;
            }
            if matches!(mode, ComponentSerializationMode::Retokenize) {
                if value.closing_source_length == 0 {
                    return false;
                }
                let source = value.original_source_text.to_vec();
                source[source.len() - value.closing_source_length..]
                    .iter()
                    .for_each(|&unit| sink.push_code_unit(unit));
            } else {
                sink.push_ascii(normalized_closing);
            }
        }
    }
    true
}

pub(crate) fn serialize_component_values_into(
    sink: &mut TextSink,
    values: &[ComponentValue],
    mode: ComponentSerializationMode,
) -> bool {
    for (index, value) in values.iter().enumerate() {
        if !serialize_component(sink, value, mode) {
            return false;
        }
        if values
            .get(index + 1)
            .is_some_and(|next| token_needs_comment_between(value, next))
        {
            sink.push_ascii("/**/");
        }
    }
    true
}

fn append_original_source_text(target: &mut Vec<u16>, values: &[ComponentValue]) {
    for value in values {
        value.original_source_text.append_to(target);
    }
}

fn is_same_token_kind(left: &ParserTokenKind, right: &ParserTokenKind) -> bool {
    std::mem::discriminant(left) == std::mem::discriminant(right)
}

/// The tokens that component values are consumed from, which a source may produce only as they're asked for.
pub(crate) trait ParserTokens {
    fn token(&mut self, index: usize) -> Option<&ParserToken>;
    fn token_mut(&mut self, index: usize) -> Option<&mut ParserToken>;
}

impl ParserTokens for [ParserToken] {
    fn token(&mut self, index: usize) -> Option<&ParserToken> {
        self.get(index)
    }

    fn token_mut(&mut self, index: usize) -> Option<&mut ParserToken> {
        self.get_mut(index)
    }
}

fn consume_component_values<T: ParserTokens + ?Sized>(
    tokens: &mut T,
    position: &mut usize,
    ending: Option<&ParserTokenKind>,
    depth: usize,
    take_tokens: bool,
) -> Result<ComponentValueList, ()> {
    if depth > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
        return Err(());
    }

    let mut values = Vec::with_capacity(4);
    let mut token_count = 0;
    while let Some(token) = tokens.token(*position) {
        if ending.is_some_and(|ending| is_same_token_kind(&token.kind, ending)) {
            break;
        }

        let value = consume_a_component_value(tokens, position, depth, take_tokens)?;
        token_count += value.token_count();
        values.push(value);
    }
    Ok(ComponentValueList::with_token_count(values, token_count))
}

pub(crate) fn consume_a_component_value<T: ParserTokens + ?Sized>(
    tokens: &mut T,
    position: &mut usize,
    depth: usize,
    take_tokens: bool,
) -> Result<ComponentValue, ()> {
    if depth > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
        return Err(());
    }
    let token = if take_tokens {
        let token = tokens.token_mut(*position).ok_or(())?;
        std::mem::replace(
            token,
            ParserToken {
                kind: ParserTokenKind::EndOfFile,
                source: ParserSource::empty(),
                start_position: SourcePosition::default(),
                end_position: SourcePosition::default(),
            },
        )
    } else {
        tokens.token(*position).ok_or(())?.clone()
    };
    *position += 1;
    let opening_source_length = token.source.len();
    let mut closing_source_length = 0;
    let start_position = token.start_position;
    let mut end_position = token.end_position;
    let original_source_text;
    let kind = match token.kind {
        ParserTokenKind::Function(name) => {
            let function_values = consume_component_values(
                tokens,
                position,
                Some(&ParserTokenKind::CloseParen),
                depth + 1,
                take_tokens,
            )?;
            let last_child_source = function_values.last().map(|value| &value.original_source_text);
            let mut closing_source = None;
            if let Some(closing) = tokens.token(*position)
                && matches!(closing.kind, ParserTokenKind::CloseParen)
            {
                *position += 1;
                closing_source = Some(&closing.source);
                closing_source_length = closing.source.len();
                end_position = closing.end_position;
            }
            original_source_text = ParserSource::covering(
                &token.source,
                closing_source.or(last_child_source).unwrap_or(&token.source),
            )
            .unwrap_or_else(|| {
                let mut source = token.source.to_vec();
                append_original_source_text(&mut source, &function_values);
                if let Some(closing_source) = closing_source {
                    closing_source.append_to(&mut source);
                }
                ParserSource::Owned(source.into_boxed_slice())
            });
            ComponentKind::Function {
                name,
                values: function_values.into_boxed(),
            }
        }
        opening @ (ParserTokenKind::OpenSquare | ParserTokenKind::OpenParen | ParserTokenKind::OpenCurly) => {
            let ending = match opening {
                ParserTokenKind::OpenSquare => ParserTokenKind::CloseSquare,
                ParserTokenKind::OpenParen => ParserTokenKind::CloseParen,
                ParserTokenKind::OpenCurly => ParserTokenKind::CloseCurly,
                _ => unreachable!(),
            };
            let block_values = consume_component_values(tokens, position, Some(&ending), depth + 1, take_tokens)?;
            let last_child_source = block_values.last().map(|value| &value.original_source_text);
            let mut closing_source = None;
            if let Some(closing) = tokens.token(*position)
                && is_same_token_kind(&closing.kind, &ending)
            {
                *position += 1;
                closing_source = Some(&closing.source);
                closing_source_length = closing.source.len();
                end_position = closing.end_position;
            }
            original_source_text = ParserSource::covering(
                &token.source,
                closing_source.or(last_child_source).unwrap_or(&token.source),
            )
            .unwrap_or_else(|| {
                let mut source = token.source.to_vec();
                append_original_source_text(&mut source, &block_values);
                if let Some(closing_source) = closing_source {
                    closing_source.append_to(&mut source);
                }
                ParserSource::Owned(source.into_boxed_slice())
            });
            ComponentKind::SimpleBlock {
                opening,
                values: block_values.into_boxed(),
            }
        }
        kind => {
            original_source_text = token.source;
            ComponentKind::Token(kind)
        }
    };
    Ok(ComponentValue {
        kind,
        original_source_text,
        opening_source_length,
        closing_source_length,
        start_position,
        end_position,
    })
}

// https://drafts.csswg.org/css-syntax-3/#consume-list-of-component-values
pub(crate) trait ComponentValueTokens {
    fn into_owned_tokens(self) -> Vec<ParserToken>;
}

impl ComponentValueTokens for Vec<ParserToken> {
    fn into_owned_tokens(self) -> Vec<ParserToken> {
        self
    }
}

impl ComponentValueTokens for &[ParserToken] {
    fn into_owned_tokens(self) -> Vec<ParserToken> {
        self.to_vec()
    }
}

pub(crate) fn consume_a_list_of_component_values(tokens: impl ComponentValueTokens) -> Result<Vec<ComponentValue>, ()> {
    consume_a_counted_list_of_component_values(tokens).map(ComponentValueList::into_vec)
}

pub(crate) fn consume_a_counted_list_of_component_values(
    tokens: impl ComponentValueTokens,
) -> Result<ComponentValueList, ()> {
    let mut tokens = tokens.into_owned_tokens();
    let mut position = 0;
    consume_component_values(tokens.as_mut_slice(), &mut position, None, 0, true)
}

pub(crate) fn consume_a_small_list_of_component_values(
    mut tokens: SmallParserTokenList,
) -> Result<SmallVec<[ComponentValue; 8]>, ()> {
    let mut values = SmallVec::new();
    let mut position = 0;
    while position < tokens.len() {
        values.push(consume_a_component_value(
            tokens.as_mut_slice(),
            &mut position,
            0,
            true,
        )?);
    }
    Ok(values)
}

#[cfg(test)]
mod tests {
    use super::{
        ComponentKind, ComponentSerializationMode, ComponentValueSlice, consume_a_counted_list_of_component_values,
        consume_a_list_of_component_values, serialize_component_values_into,
    };
    use crate::css::css_tokenizer::{ParserTokenKind, tokenize_for_parser};
    use crate::css::serialize::TextSink;

    fn parse(input: &str) -> Vec<super::ComponentValue> {
        consume_a_list_of_component_values(tokenize_for_parser(input.as_bytes())).unwrap()
    }

    fn utf16(value: &str) -> Box<[u16]> {
        value.encode_utf16().collect()
    }

    fn serialize(values: &[super::ComponentValue], mode: ComponentSerializationMode) -> Option<String> {
        let mut sink = TextSink::new();
        serialize_component_values_into(&mut sink, values, mode)
            .then(|| String::from_utf16(&sink.into_utf16()).unwrap())
    }

    #[test]
    fn retains_underlying_token_counts() {
        for (source, expected) in [
            ("", 0),
            ("a 1", 3),
            ("f([a])", 5),
            ("f([a", 5),
            ("[{}]", 4),
            (" f([a]) ", 7),
        ] {
            let values = consume_a_counted_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
            assert_eq!(values.token_count, expected, "{source}");
            let trimmed = values.as_slice().trim_whitespace().to_owned();
            assert_eq!(
                trimmed.token_count,
                ComponentValueSlice::new(&trimmed[..]).token_count,
                "{source}"
            );
        }
    }

    #[test]
    fn consumes_nested_functions_and_blocks() {
        let values = parse("outer([inner({ value })]) tail");
        assert_eq!(values.len(), 3);
        assert_eq!(
            values[0].original_source_text.to_vec(),
            utf16("outer([inner({ value })])").as_ref()
        );
        let ComponentKind::Function { name, values } = &values[0].kind else {
            panic!("expected a function");
        };
        assert_eq!(name.as_ref(), utf16("outer").as_ref());
        let ComponentKind::SimpleBlock { values, .. } = &values[0].kind else {
            panic!("expected a square block");
        };
        let ComponentKind::Function { name, values } = &values[0].kind else {
            panic!("expected a nested function");
        };
        assert_eq!(name.as_ref(), utf16("inner").as_ref());
        let ComponentKind::SimpleBlock { values, .. } = &values[0].kind else {
            panic!("expected a nested curly block");
        };
        assert_eq!(values.len(), 3);
    }

    #[test]
    fn retains_bad_string_and_bad_url_tokens() {
        let values = parse("\"bad\n url(foo\"bar)");
        assert!(matches!(
            values[0].kind,
            ComponentKind::Token(ParserTokenKind::BadString)
        ));
        assert!(
            values
                .iter()
                .any(|value| matches!(value.kind, ComponentKind::Token(ParserTokenKind::BadUrl)))
        );
    }

    #[test]
    fn closes_unterminated_constructs_at_eof() {
        let values = parse("function([value");
        assert_eq!(values.len(), 1);
        assert_eq!(
            values[0].original_source_text.to_vec(),
            utf16("function([value").as_ref()
        );
        let ComponentKind::Function { values, .. } = &values[0].kind else {
            panic!("expected a function");
        };
        assert!(matches!(values[0].kind, ComponentKind::SimpleBlock { .. }));
    }

    #[test]
    fn serializes_component_values_in_each_mode() {
        let values = parse("a b 1.00PX url(value) [x 2.00PX]");
        assert_eq!(
            serialize(&values, ComponentSerializationMode::Normalized).as_deref(),
            Some("a b 1PX url(\"value\") [x 2PX]")
        );
        assert_eq!(
            serialize(&values, ComponentSerializationMode::PreserveNumericSource).as_deref(),
            Some("a b 1.00PX url(\"value\") [x 2PX]")
        );
        assert_eq!(
            serialize(&values, ComponentSerializationMode::Retokenize).as_deref(),
            Some("a b 1.00PX url(\"value\") [x 2.00PX]")
        );

        let mut adjacent_identifiers = parse("a b");
        adjacent_identifiers.remove(1);
        assert_eq!(
            serialize(&adjacent_identifiers, ComponentSerializationMode::Normalized).as_deref(),
            Some("a/**/b")
        );

        let blocks = parse("(12/**/0) [12/**/0]");
        assert_eq!(blocks[0].original_source_text.to_vec(), utf16("(12/**/0)").as_ref());
        assert_eq!(blocks[2].original_source_text.to_vec(), utf16("[12/**/0]").as_ref());
    }

    #[test]
    fn retokenization_requires_complete_nested_source() {
        let values = parse("function([value");
        assert_eq!(
            serialize(&values, ComponentSerializationMode::Normalized).as_deref(),
            Some("function([value])")
        );
        assert_eq!(serialize(&values, ComponentSerializationMode::Retokenize), None);
    }
}
