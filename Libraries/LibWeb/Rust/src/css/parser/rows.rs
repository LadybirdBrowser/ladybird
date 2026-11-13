/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tokens of a piece of source as rows of a buffer, and views of them as component values. A grammar reads the
//! views, and no component value is made: a function or a block is the rows from its first token to the token that
//! closes it, which the first token names.
//!
//! The buffer is scratch memory. Whoever parses many pieces of source fills the same buffer for each.

use super::block::Unit;
use super::component_value::MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH;
use super::lexer::{Escapes, Kind, Lexer, Token, flag, number_of, string_range, unescape};
use crate::css::css_tokenizer::TokenizerInput;
use smallvec::SmallVec;
use std::ops::Range;

/// The text of a name or a string, as UTF-16.
pub(crate) type Name = SmallVec<[u16; 124]>;

/// The token that ends the block or function that a token of `kind` starts.
#[inline]
pub(crate) fn closer_of(kind: Kind) -> Option<Kind> {
    match kind {
        Kind::Function | Kind::OpenParen => Some(Kind::CloseParen),
        Kind::OpenSquare => Some(Kind::CloseSquare),
        Kind::OpenCurly => Some(Kind::CloseCurly),
        _ => None,
    }
}

#[derive(Default)]
pub(crate) struct Rows {
    /// The tokens, and after them one end-of-file token, which says whether whitespace or a comment ends the source.
    /// The `part` of a token that starts a function or a block is the row of the token that closes it, or the row of
    /// the end-of-file token if nothing closes it.
    tokens: Vec<Token>,
    open: Vec<u32>,
    /// Whether a token that was pushed is nested too deep, so that the rows are not a list of component values.
    is_too_deep: bool,
}

impl Rows {
    /// Removes all rows, to take the tokens of another source range.
    pub(crate) fn clear(&mut self) {
        self.tokens.clear();
        self.open.clear();
        self.is_too_deep = false;
    }

    /// Appends `token`, which follows the tokens that the rows have.
    #[inline]
    pub(crate) fn push(&mut self, token: Token) {
        let row = self.tokens.len() as u32;
        if let Some(&open) = self.open.last()
            && closer_of(self.tokens[open as usize].kind) == Some(token.kind)
        {
            self.open.pop();
            self.tokens[open as usize].part = row;
        } else if closer_of(token.kind).is_some() {
            // NB: This is the depth at which a list of component values fails to parse.
            if self.open.len() >= MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
                self.is_too_deep = true;
                return;
            }
            self.open.push(row);
        }
        self.tokens.push(token);
    }

    /// Ends the rows at `end`, where `next` is the token that follows them or the end of the source. Fails if the
    /// functions and blocks are nested too deep.
    pub(crate) fn finish(&mut self, next: Token, end: usize) -> Result<(), ()> {
        if self.is_too_deep {
            return Err(());
        }
        let row = self.tokens.len() as u32;
        // NB: Whitespace and comments before the end of the range are before the token that starts there. Those after
        //     the end of the range, which starts where the last row ends, are not in the rows.
        let end = (next.start as usize).min(end) as u32;
        let has_gap_in_range = self.tokens.last().is_none_or(|last| last.end < end);
        let flags = if has_gap_in_range {
            next.flags & (flag::SPACE_BEFORE | flag::COMMENT_BEFORE)
        } else {
            0
        };
        self.tokens.push(Token {
            start: end,
            end,
            part: 0,
            kind: Kind::EndOfFile,
            flags,
        });
        for &open in &self.open {
            self.tokens[open as usize].part = row;
        }
        Ok(())
    }

    /// Fills the rows with the tokens in `range` of `units`, which are preprocessed text. Fails if the functions and
    /// blocks are nested too deep.
    pub(crate) fn fill<U: Unit>(&mut self, units: &[U], range: Range<usize>) -> Result<(), ()> {
        self.clear();
        let mut lexer = Lexer::new(units);
        lexer.set_position(range.start);
        loop {
            let token = lexer.next();
            if token.kind == Kind::EndOfFile || token.start as usize >= range.end {
                return self.finish(token, range.end);
            }
            self.push(token);
            if self.is_too_deep {
                return Err(());
            }
        }
    }

    /// Fills the rows with the tokens of `input`, which is preprocessed text.
    pub(crate) fn fill_from(&mut self, input: TokenizerInput<'_>) -> Result<(), ()> {
        match input {
            TokenizerInput::Latin1(units) => self.fill(units, 0..units.len()),
            TokenizerInput::Utf16(units) => self.fill(units, 0..units.len()),
        }
    }

    /// The component values of the rows, which are tokens of `source`. With `skip_gap`, whitespace and comments before
    /// the first token are not part of them.
    pub(crate) fn values<'a>(&'a self, source: TokenizerInput<'a>, skip_gap: bool) -> Values<'a> {
        Values {
            tokens: &self.tokens,
            source,
            row: 0,
            end: self.tokens.len() as u32 - 1,
            gap_taken: skip_gap,
        }
    }
}

/// A list of component values, and a position in it.
///
/// Whitespace and comments are not rows. The list has one whitespace value wherever a row follows either, which is
/// what a grammar can tell about them.
#[derive(Clone, Copy)]
pub(crate) struct Values<'a> {
    tokens: &'a [Token],
    source: TokenizerInput<'a>,
    /// The row of the next value.
    row: u32,
    /// The row after the last value: the token that closes the list, or the end-of-file token.
    end: u32,
    /// Whether the whitespace before the next row is behind the position.
    gap_taken: bool,
}

impl<'a> Values<'a> {
    fn last_row(&self) -> u32 {
        self.tokens.len() as u32 - 1
    }

    #[inline]
    pub(crate) fn peek(&self) -> Option<Cv<'a>> {
        let token = self.tokens[self.row as usize];
        let is_whitespace = !self.gap_taken && token.follows_a_gap();
        if self.row == self.end && !is_whitespace {
            return None;
        }
        Some(Cv {
            tokens: self.tokens,
            source: self.source,
            row: self.row,
            is_whitespace,
        })
    }

    /// The value after the next `offset` values.
    pub(crate) fn peek_n(&self, offset: usize) -> Option<Cv<'a>> {
        let mut values = *self;
        for _ in 0..offset {
            values.next()?;
        }
        values.peek()
    }

    #[inline]
    #[allow(clippy::should_implement_trait)]
    pub(crate) fn next(&mut self) -> Option<Cv<'a>> {
        let value = self.peek()?;
        if value.is_whitespace {
            self.gap_taken = true;
            return Some(value);
        }
        let token = self.tokens[self.row as usize];
        if closer_of(token.kind).is_none() {
            self.row += 1;
            self.gap_taken = false;
        } else if token.part == self.last_row() {
            // NB: Nothing closes the value, so what ends the source is in it, and not after it.
            self.row = token.part;
            self.gap_taken = true;
        } else {
            self.row = token.part + 1;
            self.gap_taken = false;
        }
        Some(value)
    }

    /// Goes past the next value.
    #[inline]
    pub(crate) fn skip(&mut self) {
        self.next();
    }

    #[inline]
    pub(crate) fn is_empty(&self) -> bool {
        self.peek().is_none()
    }

    #[inline]
    pub(crate) fn discard_whitespace(&mut self) {
        self.gap_taken = true;
    }

    /// The values from the position up to the next comma, and goes past them and the comma. `None` once the list is
    /// behind the position and no comma was the last value.
    pub(crate) fn split_at_comma(&mut self, is_first: bool) -> Option<Values<'a>> {
        if !is_first && self.row == self.end && self.gap_taken {
            return None;
        }
        let mut rest = *self;
        loop {
            match rest.peek() {
                Some(value) if value.is_comma() => {
                    let part = Values { end: rest.row, ..*self };
                    rest.skip();
                    *self = rest;
                    return Some(part);
                }
                Some(_) => rest.skip(),
                None => {
                    let part = *self;
                    // NB: The position is the end of the list with nothing before it, which says that no part is left.
                    self.row = self.end;
                    self.gap_taken = true;
                    return if is_first || part.row != part.end || !part.gap_taken {
                        Some(part)
                    } else {
                        None
                    };
                }
            }
        }
    }

    /// Where the values from the position on are in the source, without the whitespace before and after them.
    pub(crate) fn source_range(&self) -> Range<usize> {
        let mut values = *self;
        values.discard_whitespace();
        let start = self.tokens[values.row as usize].start as usize;
        let mut end = start;
        while let Some(value) = values.next() {
            if !value.is_whitespace {
                end = value.source_end();
            }
        }
        start..end
    }

    /// The text of the source in `range`.
    pub(crate) fn source_text(&self, range: Range<usize>) -> Vec<u16> {
        let mut text = Vec::new();
        self.source.slice(range).append_to(&mut text);
        text
    }
}

/// One component value of a list.
#[derive(Clone, Copy)]
pub(crate) struct Cv<'a> {
    tokens: &'a [Token],
    source: TokenizerInput<'a>,
    row: u32,
    is_whitespace: bool,
}

impl<'a> Cv<'a> {
    #[inline]
    fn token(&self) -> Token {
        self.tokens[self.row as usize]
    }

    #[inline]
    fn kind(&self) -> Option<Kind> {
        (!self.is_whitespace).then(|| self.token().kind)
    }

    #[inline]
    fn code_at(&self, index: usize) -> u32 {
        u32::from(self.source.code_unit_at(index))
    }

    fn text(&self, range: (usize, usize), escapes: Option<Escapes>) -> Name {
        let (start, end) = range;
        match (self.source, escapes) {
            (TokenizerInput::Latin1(units), None) => units[start..end].iter().map(|&unit| u16::from(unit)).collect(),
            (TokenizerInput::Utf16(units), None) => SmallVec::from_slice(&units[start..end]),
            (source, Some(escapes)) => {
                // NB: What the escapes mean is not longer than the escapes.
                let mut output = Name::new();
                match source {
                    TokenizerInput::Latin1(units) => unescape(&units[start..end], escapes, &mut output),
                    TokenizerInput::Utf16(units) => unescape(&units[start..end], escapes, &mut output),
                }
                output
            }
        }
    }

    fn name(&self) -> Name {
        let token = self.token();
        self.text(token.name_range(), token.has(flag::ESCAPED).then_some(Escapes::Name))
    }

    /// Where the value ends in the source.
    fn source_end(&self) -> usize {
        let token = self.token();
        if closer_of(token.kind).is_none() {
            return token.end as usize;
        }
        let closing = self.tokens[token.part as usize];
        closing.end as usize
    }

    #[inline]
    pub(crate) fn is_whitespace(&self) -> bool {
        self.is_whitespace
    }

    #[inline]
    pub(crate) fn is_comma(&self) -> bool {
        self.kind() == Some(Kind::Comma)
    }

    #[inline]
    pub(crate) fn is_colon(&self) -> bool {
        self.kind() == Some(Kind::Colon)
    }

    #[inline]
    pub(crate) fn is_delim(&self, expected: u8) -> bool {
        self.kind() == Some(Kind::Delim) && self.code_at(self.token().start as usize) == u32::from(expected)
    }

    #[inline]
    pub(crate) fn is_ident(&self) -> bool {
        self.kind() == Some(Kind::Ident)
    }

    pub(crate) fn ident(&self) -> Option<Name> {
        self.is_ident().then(|| self.name())
    }

    /// The value of a hash token that is a valid identifier.
    pub(crate) fn id(&self) -> Option<Name> {
        (self.kind() == Some(Kind::Hash) && self.token().has(flag::ID)).then(|| self.name())
    }

    pub(crate) fn string(&self) -> Option<Name> {
        if self.kind() != Some(Kind::String) {
            return None;
        }
        let token = self.token();
        let (range, escapes) = match self.source {
            TokenizerInput::Latin1(units) => string_range(units, token),
            TokenizerInput::Utf16(units) => string_range(units, token),
        };
        Some(self.text((range.start, range.end), escapes))
    }

    /// The value of a number token, and whether it is written as an integer, and with a sign.
    pub(crate) fn number(&self) -> Option<(f64, bool, bool)> {
        (self.kind() == Some(Kind::Number)).then(|| self.numeric_value())
    }

    /// The value of a dimension token, whether it is written as an integer, and its unit.
    pub(crate) fn dimension(&self) -> Option<(f64, bool, Name)> {
        if self.kind() != Some(Kind::Dimension) {
            return None;
        }
        let (value, is_integer, _) = self.numeric_value();
        Some((value, is_integer, self.name()))
    }

    fn numeric_value(&self) -> (f64, bool, bool) {
        let token = self.token();
        let (start, end) = token.number_range();
        let value = match self.source {
            TokenizerInput::Latin1(units) => number_of(&units[start..end]),
            TokenizerInput::Utf16(units) => number_of(&units[start..end]),
        };
        (value, token.has(flag::INTEGER), token.has(flag::SIGNED))
    }

    fn contents(&self) -> Values<'a> {
        Values {
            tokens: self.tokens,
            source: self.source,
            row: self.row + 1,
            end: self.token().part,
            gap_taken: false,
        }
    }

    /// The name of a function, and its arguments.
    pub(crate) fn function(&self) -> Option<(Name, Values<'a>)> {
        (self.kind() == Some(Kind::Function)).then(|| (self.name(), self.contents()))
    }

    /// What is in a block in square brackets.
    pub(crate) fn square_block(&self) -> Option<Values<'a>> {
        (self.kind() == Some(Kind::OpenSquare)).then(|| self.contents())
    }
}

#[cfg(test)]
mod tests {
    use super::{Rows, Values};
    use crate::css::css_tokenizer::{ParserTokenKind, TokenizerInput, tokenize_for_parser};
    use crate::css::parser::component_value::{ComponentKind, ComponentValue, consume_a_list_of_component_values};
    use std::fmt::Write;

    fn old_shape(values: &[ComponentValue], output: &mut String) {
        let mut was_whitespace = false;
        for value in values {
            // NB: A run of whitespace and comments is one whitespace value in the rows.
            if value.is_whitespace() {
                if !was_whitespace {
                    output.push('_');
                }
                was_whitespace = true;
                continue;
            }
            was_whitespace = false;
            match &value.kind {
                ComponentKind::Function { name, values } => {
                    write!(output, "f{}(", String::from_utf16_lossy(name.as_ref())).unwrap();
                    old_shape(values, output);
                    output.push(')');
                }
                ComponentKind::SimpleBlock { opening, values } => {
                    output.push(if matches!(opening, ParserTokenKind::OpenSquare) {
                        '['
                    } else {
                        '<'
                    });
                    old_shape(values, output);
                    output.push('>');
                }
                ComponentKind::Token(ParserTokenKind::Ident(name)) => {
                    write!(output, "i{} ", String::from_utf16_lossy(name.as_ref())).unwrap();
                }
                ComponentKind::Token(ParserTokenKind::String(text)) => {
                    write!(output, "s{} ", String::from_utf16_lossy(text.as_ref())).unwrap();
                }
                ComponentKind::Token(ParserTokenKind::Comma) => output.push(','),
                ComponentKind::Token(ParserTokenKind::Colon) => output.push(':'),
                ComponentKind::Token(ParserTokenKind::Hash { value, is_id: true }) => {
                    write!(output, "#{} ", String::from_utf16_lossy(value.as_ref())).unwrap();
                }
                ComponentKind::Token(ParserTokenKind::Number { value, .. }) => write!(output, "n{value} ").unwrap(),
                ComponentKind::Token(ParserTokenKind::Dimension { value, unit, .. }) => {
                    write!(output, "d{value}{} ", String::from_utf16_lossy(unit.as_ref())).unwrap();
                }
                _ => output.push('?'),
            }
        }
    }

    fn new_shape(mut values: Values<'_>, output: &mut String) {
        while let Some(value) = values.next() {
            if value.is_whitespace() {
                output.push('_');
            } else if let Some((name, arguments)) = value.function() {
                write!(output, "f{}(", String::from_utf16_lossy(&name)).unwrap();
                new_shape(arguments, output);
                output.push(')');
            } else if let Some(contents) = value.square_block() {
                output.push('[');
                new_shape(contents, output);
                output.push('>');
            } else if value.kind() == Some(super::Kind::OpenParen) || value.kind() == Some(super::Kind::OpenCurly) {
                output.push('<');
                new_shape(value.contents(), output);
                output.push('>');
            } else if let Some(name) = value.ident() {
                write!(output, "i{} ", String::from_utf16_lossy(&name)).unwrap();
            } else if let Some(text) = value.string() {
                write!(output, "s{} ", String::from_utf16_lossy(&text)).unwrap();
            } else if value.is_comma() {
                output.push(',');
            } else if value.is_colon() {
                output.push(':');
            } else if let Some(name) = value.id() {
                write!(output, "#{} ", String::from_utf16_lossy(&name)).unwrap();
            } else if let Some((number, _, _)) = value.number() {
                write!(output, "n{number} ").unwrap();
            } else if let Some((number, _, unit)) = value.dimension() {
                write!(output, "d{number}{} ", String::from_utf16_lossy(&unit)).unwrap();
            } else {
                output.push('?');
            }
        }
    }

    #[test]
    fn rows_have_no_whitespace_from_after_their_range() {
        let units: Vec<u16> = "a  {".encode_utf16().collect();
        let source = TokenizerInput::Utf16(&units);
        let mut rows = Rows::default();
        rows.fill(&units, 0..1).unwrap();
        let mut values = rows.values(source, false);
        assert!(values.next().is_some_and(|value| value.is_ident()));
        assert!(values.is_empty());
        rows.fill(&units, 0..3).unwrap();
        let mut values = rows.values(source, false);
        assert!(values.next().is_some_and(|value| value.is_ident()));
        assert!(values.next().is_some_and(|value| value.is_whitespace()));
        assert!(values.is_empty());
    }

    #[test]
    fn has_the_component_values_of_the_tokenizer() {
        for source in [
            "",
            " ",
            "a",
            " a ",
            "a b",
            "a/**/b",
            "a /* x */ /* y */ b",
            ".a > .b:not( .c , #d ) ~ e[f = \"g\" i]",
            "a:nth-child( 2n + 1 of .b ), c",
            "a(b[c)d]e)f",
            "a ( b ] c ) d } e",
            "a(b",
            "a(b ",
            "a(b[c ",
            "a( /* x */",
            "[a",
            "a\\62 c, #\\31 23, 'x\\'y' \"unclosed",
            "#1a .5em 10px -3 +4n",
            "a,",
            ",a,,b ,",
            "{a(b}c",
        ] {
            let tokens = tokenize_for_parser(source.as_bytes());
            let old = consume_a_list_of_component_values(tokens.as_slice()).unwrap();
            let mut expected = String::new();
            old_shape(&old, &mut expected);
            for wide in [false, true] {
                let utf16 = source.encode_utf16().collect::<Vec<_>>();
                let input = if wide {
                    TokenizerInput::Utf16(&utf16)
                } else {
                    TokenizerInput::Latin1(source.as_bytes())
                };
                let mut rows = Rows::default();
                rows.fill_from(input).unwrap();
                let mut actual = String::new();
                new_shape(rows.values(input, false), &mut actual);
                assert_eq!(actual, expected, "{source:?}");
            }
        }
    }

    #[test]
    fn splits_at_commas_like_a_slice() {
        for (source, expected) in [
            ("", vec![""]),
            ("a", vec!["a"]),
            ("a,b", vec!["a", "b"]),
            ("a , b(c,d) ,", vec!["a", "b(c,d)", ""]),
            (",", vec!["", ""]),
            (" ", vec![""]),
            ("a, ", vec!["a", ""]),
        ] {
            let input = TokenizerInput::Latin1(source.as_bytes());
            let mut rows = Rows::default();
            rows.fill_from(input).unwrap();
            let mut values = rows.values(input, false);
            let mut parts = Vec::new();
            let mut is_first = true;
            while let Some(part) = values.split_at_comma(is_first) {
                is_first = false;
                parts.push(String::from_utf16(&part.source_text(part.source_range())).unwrap());
            }
            assert_eq!(parts, expected, "{source:?}");
        }
    }

    #[test]
    fn fails_where_component_values_are_too_deep() {
        let source = "(".repeat(crate::css::parser::component_value::MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH + 1);
        let tokens = tokenize_for_parser(source.as_bytes());
        assert!(consume_a_list_of_component_values(tokens.as_slice()).is_err());
        let mut rows = Rows::default();
        assert!(rows.fill_from(TokenizerInput::Latin1(source.as_bytes())).is_err());
        let source = &source[1..];
        let tokens = tokenize_for_parser(source.as_bytes());
        assert!(consume_a_list_of_component_values(tokens.as_slice()).is_ok());
        assert!(rows.fill_from(TokenizerInput::Latin1(source.as_bytes())).is_ok());
    }
}
