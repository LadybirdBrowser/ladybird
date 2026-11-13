/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tokenizer of the style sheet parser. It runs on demand, one token ahead of whoever reads it, and a token is
//! where it is in the source and what kind it is, with no value: a name, a string or a number is validated and left in
//! the source, and read from there by whoever needs it.
//!
//! Whitespace and comments are not tokens. A token says whether either came before it.
//!
//! The source is preprocessed text: it holds no U+000D, U+000C, U+0000 or lone surrogate. Then every code unit from
//! 0x80 up is an ident code point, and the lexer never decodes a surrogate pair.
//!
//! https://drafts.csswg.org/css-syntax-3/#tokenization

use super::block::{BLOCK, Unit, eq_mask, ident_mask};
use crate::css::css_tokenizer::TokenizerInput;
use std::ops::Range;

/// The code of the units past the end of the source.
const END: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum Kind {
    EndOfFile,
    Ident,
    Function,
    AtKeyword,
    Hash,
    String,
    BadString,
    Url,
    BadUrl,
    Delim,
    Number,
    Percentage,
    Dimension,
    Cdo,
    Cdc,
    Colon,
    Semicolon,
    Comma,
    OpenSquare,
    CloseSquare,
    OpenParen,
    CloseParen,
    OpenCurly,
    CloseCurly,
}

pub(crate) mod flag {
    /// Whitespace came before the token.
    pub(crate) const SPACE_BEFORE: u8 = 1;
    /// A comment came before the token.
    pub(crate) const COMMENT_BEFORE: u8 = 1 << 1;
    /// The text of the token has a U+005C REVERSE SOLIDUS (\), so its value is not its text as written.
    pub(crate) const ESCAPED: u8 = 1 << 2;
    /// A numeric token is written as an integer.
    pub(crate) const INTEGER: u8 = 1 << 3;
    /// A numeric token is written with a sign.
    pub(crate) const SIGNED: u8 = 1 << 4;
    /// A hash token's value is a valid identifier.
    pub(crate) const ID: u8 = 1 << 5;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Token {
    /// Where the token starts in the source, and where it ends.
    pub start: u32,
    pub end: u32,
    /// Where the unit of a dimension starts, or the contents of a URL.
    pub part: u32,
    pub kind: Kind,
    pub flags: u8,
}

const _: () = assert!(size_of::<Token>() == 16);

impl Token {
    pub(crate) fn has(self, flag: u8) -> bool {
        self.flags & flag != 0
    }

    /// Whether whitespace or a comment came before the token.
    pub(crate) fn follows_a_gap(self) -> bool {
        self.has(flag::SPACE_BEFORE | flag::COMMENT_BEFORE)
    }

    /// Where the name of the token is in the source: an ident as a whole, a function without its parenthesis, an
    /// at-keyword or a hash without its first character, and the unit of a dimension.
    pub(crate) fn name_range(self) -> (usize, usize) {
        let (start, end) = match self.kind {
            Kind::Function => (self.start, self.end - 1),
            Kind::AtKeyword | Kind::Hash => (self.start + 1, self.end),
            Kind::Dimension => (self.part, self.end),
            _ => (self.start, self.end),
        };
        (start as usize, end as usize)
    }

    /// Where the number of a numeric token is in the source.
    pub(crate) fn number_range(self) -> (usize, usize) {
        let end = match self.kind {
            Kind::Dimension => self.part,
            Kind::Percentage => self.end - 1,
            _ => self.end,
        };
        (self.start as usize, end as usize)
    }
}

// https://drafts.csswg.org/css-syntax-3/#whitespace
#[inline(always)]
fn is_whitespace(code: u32) -> bool {
    matches!(code, 0x20 | 0x09 | 0x0A)
}

#[inline(always)]
fn is_digit(code: u32) -> bool {
    code.wrapping_sub(0x30) < 10
}

#[inline(always)]
fn is_hex_digit(code: u32) -> bool {
    is_digit(code) || (code | 0x20).wrapping_sub(0x61) < 6
}

// https://drafts.csswg.org/css-syntax-3/#ident-start-code-point
#[inline(always)]
fn is_ident_start(code: u32) -> bool {
    (code | 0x20).wrapping_sub(0x61) < 26 || code == 0x5F || (code >= 0x80 && code != END)
}

// https://drafts.csswg.org/css-syntax-3/#ident-code-point
#[inline(always)]
fn is_ident(code: u32) -> bool {
    is_ident_start(code) || is_digit(code) || code == 0x2D
}

// https://drafts.csswg.org/css-syntax-3/#non-printable-code-point
#[inline(always)]
fn is_non_printable(code: u32) -> bool {
    code <= 0x08 || code == 0x0B || (0x0E..=0x1F).contains(&code) || code == 0x7F
}

// https://drafts.csswg.org/css-syntax-3/#starts-with-a-valid-escape
#[inline(always)]
fn is_valid_escape(first: u32, second: u32) -> bool {
    first == 0x5C && second != 0x0A
}

// https://drafts.csswg.org/css-syntax-3/#would-start-an-identifier
#[inline(always)]
fn would_start_an_ident_sequence(first: u32, second: u32, third: u32) -> bool {
    if first == 0x2D {
        return is_ident_start(second) || second == 0x2D || is_valid_escape(second, third);
    }
    is_ident_start(first) || is_valid_escape(first, second)
}

// https://drafts.csswg.org/css-syntax-3/#starts-with-a-number
#[inline(always)]
fn would_start_a_number(first: u32, second: u32, third: u32) -> bool {
    if first == 0x2B || first == 0x2D {
        return is_digit(second) || (second == 0x2E && is_digit(third));
    }
    if first == 0x2E {
        return is_digit(second);
    }
    is_digit(first)
}

pub(crate) struct Lexer<'a, U: Unit> {
    units: &'a [U],
    position: usize,
}

impl<'a, U: Unit> Lexer<'a, U> {
    /// A lexer of `units`, which are preprocessed text.
    pub(crate) fn new(units: &'a [U]) -> Self {
        // NB: Tokens hold 32-bit offsets. Nothing loads a style sheet of 4 GiB, so what is past that is not read.
        let units = &units[..units.len().min(u32::MAX as usize - 1)];
        Self { units, position: 0 }
    }

    pub(crate) fn units(&self) -> &'a [U] {
        self.units
    }

    /// Makes the lexer go on from `position`, which is where a token starts or ends.
    pub(crate) fn set_position(&mut self, position: usize) {
        self.position = position;
    }

    #[inline(always)]
    fn at(&self, index: usize) -> u32 {
        match self.units.get(index) {
            Some(unit) => unit.code(),
            None => END,
        }
    }

    #[inline(always)]
    fn block(&self, index: usize) -> Option<super::block::Block> {
        Some(U::load(self.units.get(index..)?.first_chunk::<BLOCK>()?))
    }

    #[inline(always)]
    fn token(&mut self, kind: Kind, flags: u8, start: usize, end: usize) -> Token {
        self.token_with_part(kind, flags, start, end, 0)
    }

    #[inline(always)]
    fn token_with_part(&mut self, kind: Kind, flags: u8, start: usize, end: usize, part: usize) -> Token {
        self.position = end;
        Token {
            start: start as u32,
            end: end as u32,
            part: part as u32,
            kind,
            flags,
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-token
    pub(crate) fn next(&mut self) -> Token {
        let mut flags = 0;
        let mut start = self.position;
        let code = loop {
            let code = self.at(start);
            if is_whitespace(code) {
                flags |= flag::SPACE_BEFORE;
                start += 1;
            } else if code == 0x2F && self.at(start + 1) == 0x2A {
                flags |= flag::COMMENT_BEFORE;
                start = self.skip_comment(start + 2);
            } else {
                break code;
            }
        };

        let simple = |kind| (kind, start + 1);
        let (kind, end) = match code {
            END => (Kind::EndOfFile, start),
            0x22 | 0x27 => return self.string(flags, start, code),
            0x23 => {
                let (first, second, third) = (self.at(start + 1), self.at(start + 2), self.at(start + 3));
                if !is_ident(first) && !is_valid_escape(first, second) {
                    simple(Kind::Delim)
                } else {
                    if would_start_an_ident_sequence(first, second, third) {
                        flags |= flag::ID;
                    }
                    let end = self.ident_sequence(start + 1, &mut flags);
                    (Kind::Hash, end)
                }
            }
            0x28 => simple(Kind::OpenParen),
            0x29 => simple(Kind::CloseParen),
            0x2B | 0x2E => {
                if would_start_a_number(code, self.at(start + 1), self.at(start + 2)) {
                    return self.numeric(flags, start);
                }
                simple(Kind::Delim)
            }
            0x2C => simple(Kind::Comma),
            0x2D => {
                let (second, third) = (self.at(start + 1), self.at(start + 2));
                if would_start_a_number(code, second, third) {
                    return self.numeric(flags, start);
                }
                if second == 0x2D && third == 0x3E {
                    (Kind::Cdc, start + 3)
                } else if would_start_an_ident_sequence(code, second, third) {
                    return self.ident_like(flags, start);
                } else {
                    simple(Kind::Delim)
                }
            }
            0x30..=0x39 => return self.numeric(flags, start),
            0x3A => simple(Kind::Colon),
            0x3B => simple(Kind::Semicolon),
            0x3C => {
                if self.at(start + 1) == 0x21 && self.at(start + 2) == 0x2D && self.at(start + 3) == 0x2D {
                    (Kind::Cdo, start + 4)
                } else {
                    simple(Kind::Delim)
                }
            }
            0x40 => {
                if would_start_an_ident_sequence(self.at(start + 1), self.at(start + 2), self.at(start + 3)) {
                    let end = self.ident_sequence(start + 1, &mut flags);
                    (Kind::AtKeyword, end)
                } else {
                    simple(Kind::Delim)
                }
            }
            0x5B => simple(Kind::OpenSquare),
            0x5C => {
                if is_valid_escape(code, self.at(start + 1)) {
                    return self.ident_like(flags, start);
                }
                simple(Kind::Delim)
            }
            0x5D => simple(Kind::CloseSquare),
            0x7B => simple(Kind::OpenCurly),
            0x7D => simple(Kind::CloseCurly),
            code if is_ident_start(code) => return self.ident_like(flags, start),
            _ => simple(Kind::Delim),
        };
        self.token(kind, flags, start, end)
    }

    /// Where the comment whose contents start at `position` ends.
    // https://drafts.csswg.org/css-syntax-3/#consume-comment
    fn skip_comment(&self, mut position: usize) -> usize {
        loop {
            while let Some(block) = self.block(position) {
                let asterisks = eq_mask(block, b'*');
                if asterisks != 0 {
                    position += asterisks.trailing_zeros() as usize;
                    break;
                }
                position += BLOCK;
            }
            match self.at(position) {
                END => return position,
                0x2A if self.at(position + 1) == 0x2F => return position + 2,
                _ => position += 1,
            }
        }
    }

    /// Where the escape whose U+005C REVERSE SOLIDUS (\) is before `position` ends.
    // https://drafts.csswg.org/css-syntax-3/#consume-escaped-code-point
    fn skip_escape(&self, mut position: usize) -> usize {
        let code = self.at(position);
        if code == END {
            return position;
        }
        position += 1;
        if !is_hex_digit(code) {
            return position;
        }
        let mut digits = 1;
        while digits < 6 && is_hex_digit(self.at(position)) {
            digits += 1;
            position += 1;
        }
        position + usize::from(is_whitespace(self.at(position)))
    }

    /// Where the ident sequence at `position` ends.
    // https://drafts.csswg.org/css-syntax-3/#consume-name
    #[inline(always)]
    fn ident_sequence(&self, mut position: usize, flags: &mut u8) -> usize {
        loop {
            while let Some(block) = self.block(position) {
                let others = !ident_mask(block) & 0xFFFF;
                if others != 0 {
                    position += others.trailing_zeros() as usize;
                    break;
                }
                position += BLOCK;
            }
            let code = self.at(position);
            if is_ident(code) {
                position += 1;
            } else if is_valid_escape(code, self.at(position + 1)) {
                *flags |= flag::ESCAPED;
                position = self.skip_escape(position + 1);
            } else {
                return position;
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-ident-like-token
    fn ident_like(&mut self, mut flags: u8, start: usize) -> Token {
        let end = self.ident_sequence(start, &mut flags);
        if self.at(end) != 0x28 {
            return self.token(Kind::Ident, flags, start, end);
        }
        if !self.is_url(start, end, flags) {
            return self.token(Kind::Function, flags, start, end + 1);
        }
        let mut position = end + 1;
        while is_whitespace(self.at(position)) && is_whitespace(self.at(position + 1)) {
            position += 1;
        }
        let (first, second) = (self.at(position), self.at(position + 1));
        let is_quote = |code| code == 0x22 || code == 0x27;
        if is_quote(first) || (is_whitespace(first) && is_quote(second)) {
            return self.token(Kind::Function, flags, start, end + 1);
        }
        self.url(flags, start, end + 1)
    }

    /// Whether the name from `start` to `end` is "url".
    fn is_url(&self, start: usize, end: usize, flags: u8) -> bool {
        if flags & flag::ESCAPED == 0 {
            return end - start == 3
                && self.at(start) | 0x20 == 0x75
                && self.at(start + 1) | 0x20 == 0x72
                && self.at(start + 2) | 0x20 == 0x6C;
        }
        let mut name = Vec::new();
        unescape(&self.units[start..end], Escapes::Name, &mut name);
        name.len() == 3 && name[0] | 0x20 == 0x75 && name[1] | 0x20 == 0x72 && name[2] | 0x20 == 0x6C
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-url-token
    fn url(&mut self, mut flags: u8, start: usize, mut position: usize) -> Token {
        while is_whitespace(self.at(position)) {
            position += 1;
        }
        let contents = position;
        loop {
            let code = self.at(position);
            match code {
                END => break,
                0x29 => {
                    position += 1;
                    break;
                }
                0x22 | 0x27 | 0x28 => return self.bad_url(flags, start, position + 1),
                0x5C => {
                    if !is_valid_escape(code, self.at(position + 1)) {
                        return self.bad_url(flags, start, position + 1);
                    }
                    flags |= flag::ESCAPED;
                    position = self.skip_escape(position + 1);
                }
                code if is_whitespace(code) => {
                    while is_whitespace(self.at(position)) {
                        position += 1;
                    }
                    match self.at(position) {
                        END => break,
                        0x29 => {
                            position += 1;
                            break;
                        }
                        _ => return self.bad_url(flags, start, position),
                    }
                }
                code if is_non_printable(code) => return self.bad_url(flags, start, position + 1),
                _ => position += 1,
            }
        }
        self.token_with_part(Kind::Url, flags, start, position, contents)
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-remnants-of-bad-url
    fn bad_url(&mut self, flags: u8, start: usize, mut position: usize) -> Token {
        loop {
            let code = self.at(position);
            if code == END {
                break;
            }
            position += 1;
            if code == 0x29 {
                break;
            }
            if is_valid_escape(code, self.at(position)) {
                position = self.skip_escape(position);
            }
        }
        self.token(Kind::BadUrl, flags, start, position)
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-numeric-token
    fn numeric(&mut self, mut flags: u8, start: usize) -> Token {
        // https://drafts.csswg.org/css-syntax-3/#consume-number
        let mut position = start;
        flags |= flag::INTEGER;
        if matches!(self.at(position), 0x2B | 0x2D) {
            flags |= flag::SIGNED;
            position += 1;
        }
        while is_digit(self.at(position)) {
            position += 1;
        }
        if self.at(position) == 0x2E && is_digit(self.at(position + 1)) {
            flags &= !flag::INTEGER;
            position += 2;
            while is_digit(self.at(position)) {
                position += 1;
            }
        }
        if self.at(position) | 0x20 == 0x65 {
            let second = self.at(position + 1);
            let digits = if is_digit(second) {
                position + 1
            } else if matches!(second, 0x2B | 0x2D) && is_digit(self.at(position + 2)) {
                position + 2
            } else {
                0
            };
            if digits != 0 {
                flags &= !flag::INTEGER;
                position = digits;
                while is_digit(self.at(position)) {
                    position += 1;
                }
            }
        }

        let (first, second, third) = (self.at(position), self.at(position + 1), self.at(position + 2));
        if would_start_an_ident_sequence(first, second, third) {
            let end = self.ident_sequence(position, &mut flags);
            return self.token_with_part(Kind::Dimension, flags, start, end, position);
        }
        if first == 0x25 {
            return self.token(Kind::Percentage, flags, start, position + 1);
        }
        self.token(Kind::Number, flags, start, position)
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-string-token
    fn string(&mut self, mut flags: u8, start: usize, quote: u32) -> Token {
        let mut position = start + 1;
        loop {
            while let Some(block) = self.block(position) {
                let stops = eq_mask(block, quote as u8) | eq_mask(block, b'\\') | eq_mask(block, b'\n');
                if stops != 0 {
                    position += stops.trailing_zeros() as usize;
                    break;
                }
                position += BLOCK;
            }
            let code = self.at(position);
            match code {
                END => return self.token(Kind::String, flags, start, position),
                0x0A => return self.token(Kind::BadString, flags, start, position),
                0x5C => {
                    flags |= flag::ESCAPED;
                    position = match self.at(position + 1) {
                        END => position + 1,
                        0x0A => position + 2,
                        _ => self.skip_escape(position + 1),
                    };
                }
                code if code == quote => return self.token(Kind::String, flags, start, position + 1),
                _ => position += 1,
            }
        }
    }
}

/// What the text of an escape is part of, which says where the text ends and what an escape at its end means.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Escapes {
    /// An ident sequence, which is all of the text.
    Name,
    /// A string, which ends at the first quote like this one that is not escaped.
    String(u32),
    /// The contents of a URL, which end at the first whitespace or parenthesis that is not escaped.
    Url,
}

/// What takes the code units that a text means.
pub(crate) trait UnitSink {
    fn push(&mut self, unit: u16);
}

impl UnitSink for Vec<u16> {
    #[inline]
    fn push(&mut self, unit: u16) {
        Vec::push(self, unit);
    }
}

impl<const N: usize> UnitSink for smallvec::SmallVec<[u16; N]>
where
    [u16; N]: smallvec::Array<Item = u16>,
{
    #[inline]
    fn push(&mut self, unit: u16) {
        smallvec::SmallVec::push(self, unit);
    }
}

/// Appends what `units` mean to `output`, which is not the units themselves where they have escapes.
pub(crate) fn unescape<U: Unit>(units: &[U], escapes: Escapes, output: &mut impl UnitSink) {
    let at = |index: usize| units.get(index).map_or(END, |unit| unit.code());
    let mut position = 0;
    loop {
        let code = at(position);
        if code == END {
            return;
        }
        match escapes {
            Escapes::String(quote) if code == quote => return,
            Escapes::Url if is_whitespace(code) || code == 0x29 => return,
            _ => {}
        }
        position += 1;
        if code != 0x5C {
            output.push(code as u16);
            continue;
        }
        // https://drafts.csswg.org/css-syntax-3/#consume-escaped-code-point
        let escaped = at(position);
        let is_string = matches!(escapes, Escapes::String(_));
        if escaped == END {
            if !is_string {
                output.push(0xFFFD);
            }
            return;
        }
        position += 1;
        if is_string && escaped == 0x0A {
            continue;
        }
        if !is_hex_digit(escaped) {
            output.push(escaped as u16);
            continue;
        }
        let hex_value = |code: u32| {
            if is_digit(code) {
                code - 0x30
            } else {
                (code | 0x20) - 0x61 + 10
            }
        };
        let mut value = hex_value(escaped);
        let mut digits = 1;
        while digits < 6 && is_hex_digit(at(position)) {
            value = value << 4 | hex_value(at(position));
            digits += 1;
            position += 1;
        }
        position += usize::from(is_whitespace(at(position)));
        match value {
            0 | 0xD800..=0xDFFF | 0x11_0000.. => output.push(0xFFFD),
            0..0x1_0000 => output.push(value as u16),
            _ => {
                let value = value - 0x1_0000;
                output.push(0xD800 | (value >> 10) as u16);
                output.push(0xDC00 | (value & 0x3FF) as u16);
            }
        }
    }
}

/// The text of a token, which is either the source as it is or what its escapes mean.
pub(crate) enum Text<'a> {
    Source(TokenizerInput<'a>),
    Unescaped(Vec<u16>),
}

#[cfg(test)]
impl Text<'_> {
    pub(crate) fn input(&self) -> TokenizerInput<'_> {
        match self {
            Self::Source(input) => *input,
            Self::Unescaped(units) => TokenizerInput::Utf16(units),
        }
    }
}

/// The name of `token`: see [`Token::name_range`].
pub(crate) fn name_of<U: Unit>(units: &[U], token: Token) -> Text<'_> {
    let (start, end) = token.name_range();
    let units = &units[start..end];
    if !token.has(flag::ESCAPED) {
        return Text::Source(U::input(units));
    }
    let mut output = Vec::with_capacity(units.len());
    unescape(units, Escapes::Name, &mut output);
    Text::Unescaped(output)
}

/// Where the value of `token`, which is a string, is in `units`, and the escapes to remove from it, if it has any.
pub(crate) fn string_range<U: Unit>(units: &[U], token: Token) -> (Range<usize>, Option<Escapes>) {
    debug_assert_eq!(token.kind, Kind::String);
    let (start, end) = (token.start as usize, token.end as usize);
    if token.has(flag::ESCAPED) {
        return (start + 1..end, Some(Escapes::String(units[start].code())));
    }
    // NB: Without escapes, the string is closed if it ends in its quote, which is then not the quote it opens with.
    let closed = end - start >= 2 && units[end - 1] == units[start];
    (start + 1..end - usize::from(closed), None)
}

/// The value of `token`, which is a string.
pub(crate) fn string_of<U: Unit>(units: &[U], token: Token) -> Text<'_> {
    let (range, escapes) = string_range(units, token);
    let units = &units[range];
    let Some(escapes) = escapes else {
        return Text::Source(U::input(units));
    };
    let mut output = Vec::with_capacity(units.len());
    unescape(units, escapes, &mut output);
    Text::Unescaped(output)
}

/// The value of `token`, which is a URL.
pub(crate) fn url_of<U: Unit>(units: &[U], token: Token) -> Text<'_> {
    debug_assert_eq!(token.kind, Kind::Url);
    let (start, mut end) = (token.part as usize, token.end as usize);
    if !token.has(flag::ESCAPED) {
        end -= usize::from(end > start && units[end - 1].code() == 0x29);
        while end > start && is_whitespace(units[end - 1].code()) {
            end -= 1;
        }
        return Text::Source(U::input(&units[start..end]));
    }
    let mut output = Vec::with_capacity(end - start);
    unescape(&units[start..end], Escapes::Url, &mut output);
    Text::Unescaped(output)
}

/// The value of the number written as `units`, which a numeric token is or starts with.
// https://drafts.csswg.org/css-syntax-3/#convert-string-to-number
pub(crate) fn number_of<U: Unit>(units: &[U]) -> f64 {
    // OPTIMIZATION: A number of at most 15 digits and no exponent is the quotient of two integers that a double holds
    //               exactly, and that quotient is the double nearest to the number.
    const POWERS_OF_TEN: [f64; 16] = [
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    ];
    let mut digits = units;
    let negative = match digits.first().map(|unit| unit.code()) {
        Some(0x2D) => {
            digits = &digits[1..];
            true
        }
        Some(0x2B) => {
            digits = &digits[1..];
            false
        }
        _ => false,
    };
    if digits.len() <= 16 {
        let mut mantissa = 0u64;
        let mut digit_count = 0;
        let mut fraction_digits = None;
        let mut is_simple = true;
        for unit in digits {
            let code = unit.code();
            if is_digit(code) {
                mantissa = mantissa * 10 + u64::from(code - 0x30);
                digit_count += 1;
                if let Some(fraction_digits) = &mut fraction_digits {
                    *fraction_digits += 1;
                }
            } else if code == 0x2E && fraction_digits.is_none() {
                fraction_digits = Some(0);
            } else {
                is_simple = false;
                break;
            }
        }
        if is_simple && digit_count <= 15 {
            let value = mantissa as f64 / POWERS_OF_TEN[fraction_digits.unwrap_or(0)];
            return if negative { -value } else { value };
        }
    }

    let text: String = units.iter().map(|unit| unit.code() as u8 as char).collect();
    text.parse::<f64>()
        .expect("the lexer validated the number")
        .clamp(f64::from(f32::MIN), f64::from(f32::MAX))
}

#[cfg(test)]
mod tests {
    use super::{Kind, Lexer, Text, Token, flag, name_of, number_of, string_of, url_of};
    use crate::css::css_tokenizer::{CssNumberType, ParserTokenKind, TokenizerInput, tokenize_for_parser};
    use crate::css::parser::block::Unit;

    fn text(text: Text<'_>) -> Vec<u16> {
        let mut units = Vec::new();
        text.input().append_to(&mut units);
        units
    }

    /// The token as the tokenizer the lexer replaces has it.
    fn old_kind<U: Unit>(units: &[U], token: Token) -> ParserTokenKind {
        let name = || text(name_of(units, token)).into_boxed_slice().into();
        let (start, end) = token.number_range();
        let number = || number_of(&units[start..end]);
        let number_type = if !token.has(flag::INTEGER) {
            CssNumberType::Number
        } else if token.has(flag::SIGNED) {
            CssNumberType::IntegerWithExplicitSign
        } else {
            CssNumberType::Integer
        };
        match token.kind {
            Kind::EndOfFile => ParserTokenKind::EndOfFile,
            Kind::Ident => ParserTokenKind::Ident(name()),
            Kind::Function => ParserTokenKind::Function(name()),
            Kind::AtKeyword => ParserTokenKind::AtKeyword(name()),
            Kind::Hash => ParserTokenKind::Hash {
                value: name(),
                is_id: token.has(flag::ID),
            },
            Kind::String => ParserTokenKind::String(text(string_of(units, token)).into_boxed_slice().into()),
            Kind::BadString => ParserTokenKind::BadString,
            Kind::Url => ParserTokenKind::Url(text(url_of(units, token)).into_boxed_slice().into()),
            Kind::BadUrl => ParserTokenKind::BadUrl,
            Kind::Delim => ParserTokenKind::Delim(units[token.start as usize].code()),
            Kind::Number => ParserTokenKind::Number {
                value: number(),
                number_type,
            },
            Kind::Percentage => ParserTokenKind::Percentage {
                value: number(),
                number_type,
            },
            Kind::Dimension => ParserTokenKind::Dimension {
                value: number(),
                number_type,
                unit: name(),
            },
            Kind::Cdo => ParserTokenKind::Cdo,
            Kind::Cdc => ParserTokenKind::Cdc,
            Kind::Colon => ParserTokenKind::Colon,
            Kind::Semicolon => ParserTokenKind::Semicolon,
            Kind::Comma => ParserTokenKind::Comma,
            Kind::OpenSquare => ParserTokenKind::OpenSquare,
            Kind::CloseSquare => ParserTokenKind::CloseSquare,
            Kind::OpenParen => ParserTokenKind::OpenParen,
            Kind::CloseParen => ParserTokenKind::CloseParen,
            Kind::OpenCurly => ParserTokenKind::OpenCurly,
            Kind::CloseCurly => ParserTokenKind::CloseCurly,
        }
    }

    /// Checks that the lexer finds the tokens in `units` that the tokenizer it replaces finds: the same kinds and
    /// values, the same source for each, and whitespace or a comment in the same places.
    fn check_against_the_tokenizer<U: Unit>(units: &[U]) -> Result<usize, String> {
        let expected = tokenize_for_parser(U::input(units));
        let mut expected = expected.iter().peekable();
        let mut lexer = Lexer::new(units);
        let mut count = 0;
        loop {
            let token = lexer.next();
            let mut expected_gap = false;
            while expected
                .peek()
                .is_some_and(|token| token.kind == ParserTokenKind::Whitespace)
            {
                expected_gap = true;
                expected.next();
            }
            let at = || {
                let start = token.start as usize;
                let end = (start + 60).min(units.len());
                let around: String = units[start..end]
                    .iter()
                    .map(|unit| char::from_u32(unit.code()).unwrap_or('?'))
                    .collect();
                format!("at {start}: {around:?}")
            };
            if token.follows_a_gap() != expected_gap {
                return Err(format!("a gap is {} {}", token.follows_a_gap(), at()));
            }
            let Some(expected) = expected.next() else {
                return if token.kind == Kind::EndOfFile {
                    Ok(count)
                } else {
                    Err(format!("{token:?} is past the last token {}", at()))
                };
            };
            let kind = old_kind(units, token);
            if kind != expected.kind {
                return Err(format!("{kind:?} is not {:?} {}", expected.kind, at()));
            }
            let source: Vec<u16> = expected.source.iter().collect();
            let own_source = text(Text::Source(U::input(&units[token.start as usize..token.end as usize])));
            // NB: The tokenizer in `css_tokenizer` puts the whitespace it skips after `url(` in a function token,
            //     where one space before a quote is left over. Here all of that whitespace is the gap before the
            //     string.
            let is_same_source = if token.kind == Kind::Function {
                source.starts_with(&own_source) && source[own_source.len()..].iter().all(|&unit| unit == 0x20)
            } else {
                source == own_source
            };
            if !is_same_source {
                return Err(format!("the source of {kind:?} is not that of the old token {}", at()));
            }
            count += 1;
        }
    }

    #[track_caller]
    fn check(source: &str) {
        if source.is_ascii() {
            check_against_the_tokenizer(source.as_bytes()).unwrap_or_else(|error| panic!("{source:?}: {error}"));
            // NB: Text right before the end of the source takes the paths that read no block.
            let padded = format!("{source}{}", " ".repeat(40));
            check_against_the_tokenizer(padded.as_bytes()).unwrap_or_else(|error| panic!("{padded:?}: {error}"));
        }
        let wide: Vec<u16> = source.encode_utf16().collect();
        check_against_the_tokenizer(&wide).unwrap_or_else(|error| panic!("{source:?} as UTF-16: {error}"));
    }

    #[test]
    fn finds_the_tokens_of_the_old_tokenizer() {
        for source in [
            "",
            " ",
            "a",
            "a{color:red}",
            ".class-name > #id::before { margin: -1.5em +2px 3% .5e3 1e-2 1E+2px 12e 12ex -0 +.5 }",
            "@media (min-width: 100px) and (max-width:200PX){a,b{c:d!important}}",
            "a /* comment */ b/**/c /* a */ /* b */ d/* unterminated */",
            "<!-- a --> -- -a --a -1 - -- -\\a --> <! <!-",
            "'string' \"string\" 'a\\'b' \"a\\\"b\" 'a\\\nb' 'a\\62 c' 'bad\nstring' \"unterminated",
            "'unterminated \\",
            "'\\",
            "\"",
            "url(a) url( a ) url(a b) url(a\\ b) url('a') url( 'a' ) url(  \"a\"  ) URL(a) u\\72l(a) url(a\\) url(a(b)",
            "url(a\"b) c url(a\nb) d url(\\\n) e url(a\\29 b) url(",
            "url(   ",
            "url(a   ",
            "url(a\\",
            "url(\u{1}) x",
            "url(a) url()",
            "func(a, b) calc(1px + 2px) rgb(0 0 0 / 50%) var(--x, ) translate(-10px,20px)",
            "#fff #-a #--a #1a #-1 #\\31 a # #_ #é",
            "\\66 oo \\000066oo \\0 \\110000 \\d800 \\a\\b \\",
            "a\\",
            "é ü\u{10000}x 日本語{}",
            "--custom-property: value; -webkit-thing: 1; _a -_ -",
            "1 12 1.5 1. .5 1.5.5 1e3 1e 1e+ 1e+3 1e-3 1e3e3 1x 1\\78 1-2 1--2 1- 1%% 1e3% 1.e3 +1 -1 +a 1_a",
            "123456789012345678901234567890 0.1234567890123456789 1e400 -1e400 0.000001 100000000000000000000px",
            "0.1 0.2 0.3 1.1 2.675 1234.5678 99999.99999 0.30000000000000004 3.141592653589793",
            "[a=b] [a~='b' i] a|b *|* a||b a:not(.b):nth-child(2n+1) a:nth-child(-n+3) a:nth-child(2n-1)",
            "$ % & * + , . / : ; < = > ? @ ^ ` | ~ ! @a @-a @--a @-1 @\\61",
            "a{b:c}/*",
            "a{b:c}/",
            "/***/a/****/b/* * / */c",
            "a\tb\nc\n\n\t d",
            "very-long-identifier-that-goes-past-one-block-of-code-units{another-long-property-name:another-long-value}",
            "'a string that is longer than one block of code units' 'and one with an \\'escape\\' after the first block'",
            "/* a comment that is longer than one block of code units */a/* and * one ** with * asterisks ***/b",
        ] {
            check(source);
        }
    }

    #[test]
    fn an_unterminated_comment_goes_on_to_the_end_of_the_source() {
        // NB: The tokenizer in `css_tokenizer` leaves the last code point of such a comment to be a token.
        for source in ["a/**", "a/* *", "a/* b", "a/*"] {
            let mut lexer = Lexer::new(source.as_bytes());
            assert_eq!(lexer.next().kind, Kind::Ident);
            let end = lexer.next();
            assert_eq!(end.kind, Kind::EndOfFile, "{source}");
            assert!(end.has(flag::COMMENT_BEFORE));
            assert_eq!(end.start as usize, source.len());
        }
    }

    #[test]
    fn converts_numbers_like_the_standard_library() {
        for number in [
            "0",
            "1",
            "007",
            "1.5",
            "0.1",
            "123456789.123456",
            "999999999999999",
            ".5",
            "5.0",
            "+1.25",
        ] {
            let expected: f64 = number.parse().unwrap();
            assert_eq!(number_of(number.as_bytes()).to_bits(), expected.to_bits(), "{number}");
            let negative = format!("-{}", number.trim_start_matches('+'));
            let expected: f64 = negative.parse().unwrap();
            assert_eq!(
                number_of(negative.as_bytes()).to_bits(),
                expected.to_bits(),
                "{negative}"
            );
        }
    }

    #[test]
    fn input_type_of_text_is_the_source_width() {
        let token = Lexer::new(b"abc".as_slice()).next();
        assert!(matches!(
            name_of(b"abc".as_slice(), token),
            Text::Source(TokenizerInput::Latin1(b"abc"))
        ));
    }
}
