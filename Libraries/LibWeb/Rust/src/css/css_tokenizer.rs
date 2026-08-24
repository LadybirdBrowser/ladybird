/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use smallvec::SmallVec;
use std::ffi::c_void;
use std::ops::Range;
#[cfg(test)]
use std::ptr;
use std::rc::Rc;

const REPLACEMENT_CHARACTER: u32 = 0xFFFD;
const TOKENIZER_EOF: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum CssTokenType {
    Invalid,
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
    Whitespace,
    CDO,
    CDC,
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum CssHashType {
    Id,
    Unrestricted,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub enum CssNumberType {
    Number,
    IntegerWithExplicitSign,
    Integer,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct CssSyntaxToken {
    pub token_type: CssTokenType,
    pub start_line: usize,
    pub start_column: usize,
    pub end_line: usize,
    pub end_column: usize,
}

/// # Safety
/// - `input` and `input_len` must point to a valid string
/// - `ctx` must be a valid pointer to a CallbackContext
/// - Parameters provided to `callback` must be valid pointers
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_tokenize_for_syntax_highlighting(
    ascii_input: *const u8,
    utf16_input: *const u16,
    input_len: usize,
    ctx: *mut c_void,
    callback: unsafe extern "C" fn(ctx: *mut c_void, token: *const CssSyntaxToken),
) {
    unsafe {
        crate::abort_on_panic(|| {
            let Some(input) = TokenizerInput::from_raw_parts(ascii_input, utf16_input, input_len) else {
                return;
            };

            tokenize(input, |token, _| {
                let ffi_token = token.as_syntax_ffi();
                callback(ctx, &raw const ffi_token);
            });
        });
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct SourcePosition {
    pub line: usize,
    pub column: usize,
}

#[derive(Clone, Copy)]
struct NumericValue {
    number_type: CssNumberType,
    value: f64,
}

pub(crate) struct Token {
    token_type: TokenType,
    original_source_range: Range<usize>,
    range: Range<SourcePosition>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum OwnedTokenKind {
    Ident(Vec<u16>),
    Function(Vec<u16>),
    AtKeyword,
    Hash,
    Url,
    BadUrl,
    Number,
    Percentage,
    Dimension,
    Cdc,
    Delim(u32),
    Whitespace,
    Comma,
    OpenSquare,
    CloseSquare,
    OpenParen,
    CloseParen,
    OpenCurly,
    CloseCurly,
    Other,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct OwnedToken {
    pub kind: OwnedTokenKind,
    pub source: ParserSource,
}

#[derive(Clone, Copy)]
pub(crate) enum TokenizerInput<'a> {
    Ascii(&'a [u8]),
    Utf16(&'a [u16]),
}

impl Default for TokenizerInput<'_> {
    fn default() -> Self {
        Self::Ascii(&[])
    }
}

impl TokenizerInput<'_> {
    /// # Safety
    /// Exactly one non-empty input pointer must identify `length` readable units.
    pub(crate) unsafe fn from_raw_parts(ascii: *const u8, utf16: *const u16, length: usize) -> Option<Self> {
        if length == 0 {
            return Some(Self::Ascii(&[]));
        }
        match (ascii.is_null(), utf16.is_null()) {
            (false, true) => Some(Self::Ascii(unsafe { std::slice::from_raw_parts(ascii, length) })),
            (true, false) => Some(Self::Utf16(unsafe { std::slice::from_raw_parts(utf16, length) })),
            _ => None,
        }
    }

    pub(crate) fn len(self) -> usize {
        match self {
            Self::Ascii(units) => units.len(),
            Self::Utf16(units) => units.len(),
        }
    }

    pub(crate) fn is_empty(self) -> bool {
        self.len() == 0
    }

    pub(crate) fn append_to(self, output: &mut Vec<u16>) {
        match self {
            Self::Ascii(units) => output.extend(units.iter().copied().map(u16::from)),
            Self::Utf16(units) => output.extend_from_slice(units),
        }
    }
}

impl<'a> From<ak::Utf16StringUnits<'a>> for TokenizerInput<'a> {
    fn from(value: ak::Utf16StringUnits<'a>) -> Self {
        match value {
            ak::Utf16StringUnits::Ascii(units) => Self::Ascii(units),
            ak::Utf16StringUnits::Utf16(units) => Self::Utf16(units),
        }
    }
}

impl<'a> From<&'a [u8]> for TokenizerInput<'a> {
    fn from(value: &'a [u8]) -> Self {
        assert!(value.is_ascii());
        Self::Ascii(value)
    }
}

impl<'a> From<&'a [u16]> for TokenizerInput<'a> {
    fn from(value: &'a [u16]) -> Self {
        Self::Utf16(value)
    }
}

impl<'a, const N: usize> From<&'a [u8; N]> for TokenizerInput<'a> {
    fn from(value: &'a [u8; N]) -> Self {
        Self::from(value.as_slice())
    }
}

impl<'a> From<&'a Vec<u8>> for TokenizerInput<'a> {
    fn from(value: &'a Vec<u8>) -> Self {
        Self::from(value.as_slice())
    }
}

impl<'a> From<&'a Vec<u16>> for TokenizerInput<'a> {
    fn from(value: &'a Vec<u16>) -> Self {
        Self::Utf16(value)
    }
}

#[derive(Clone, Debug)]
pub(crate) enum ParserString {
    Inline(SmallVec<[u16; 16]>),
    Owned(Box<[u16]>),
    Shared { storage: Rc<[u16]>, range: Range<usize> },
    Pending(Range<usize>),
}

impl ParserString {
    fn finish_shared(&mut self, storage: Rc<[u16]>, offset: usize) {
        let Self::Pending(range) = self else {
            return;
        };
        *self = Self::Shared {
            storage,
            range: range.start + offset..range.end + offset,
        };
    }
}

impl AsRef<[u16]> for ParserString {
    fn as_ref(&self) -> &[u16] {
        match self {
            Self::Inline(value) => value,
            Self::Owned(value) => value,
            Self::Shared { storage, range } => &storage[range.clone()],
            Self::Pending(_) => unreachable!(),
        }
    }
}

impl std::ops::Deref for ParserString {
    type Target = [u16];

    fn deref(&self) -> &Self::Target {
        self.as_ref()
    }
}

impl PartialEq for ParserString {
    fn eq(&self, other: &Self) -> bool {
        self.as_ref() == other.as_ref()
    }
}

impl From<Box<[u16]>> for ParserString {
    fn from(value: Box<[u16]>) -> Self {
        Self::Owned(value)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum ParserTokenKind {
    EndOfFile,
    Ident(ParserString),
    Function(ParserString),
    AtKeyword(ParserString),
    Hash {
        value: ParserString,
        is_id: bool,
    },
    String(ParserString),
    BadString,
    Url(ParserString),
    BadUrl,
    Delim(u32),
    Number {
        value: f64,
        number_type: CssNumberType,
    },
    Percentage {
        value: f64,
        number_type: CssNumberType,
    },
    Dimension {
        value: f64,
        number_type: CssNumberType,
        unit: ParserString,
    },
    Whitespace,
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

impl ParserTokenKind {
    fn finish_shared_strings(&mut self, storage: Rc<[u16]>, offset: usize) {
        let string = match self {
            Self::Ident(value)
            | Self::Function(value)
            | Self::AtKeyword(value)
            | Self::String(value)
            | Self::Url(value) => Some(value),
            Self::Hash { value, .. } => Some(value),
            Self::Dimension { unit, .. } => Some(unit),
            _ => None,
        };
        if let Some(string) = string {
            string.finish_shared(storage, offset);
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ParserToken {
    pub kind: ParserTokenKind,
    pub source: ParserSource,
    pub start_position: SourcePosition,
    pub end_position: SourcePosition,
}

#[derive(Clone, Debug)]
pub(crate) enum ParserSource {
    Empty,
    Owned(Box<[u16]>),
    Shared {
        storage: Rc<SourceStorage>,
        range: Range<usize>,
    },
}

#[derive(Debug)]
pub(crate) enum SourceStorage {
    Ascii(Box<[u8]>),
    Utf16(Box<[u16]>),
}

pub(crate) enum ParserSourceIter<'a> {
    Empty,
    Owned(std::slice::Iter<'a, u16>),
    Ascii(std::slice::Iter<'a, u8>),
    Utf16(std::slice::Iter<'a, u16>),
}

impl Iterator for ParserSourceIter<'_> {
    type Item = u16;

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Empty => None,
            Self::Owned(units) | Self::Utf16(units) => units.next().copied(),
            Self::Ascii(units) => units.next().copied().map(u16::from),
        }
    }
}

impl ParserSource {
    pub(crate) fn empty() -> Self {
        Self::Empty
    }

    fn shared(storage: Rc<SourceStorage>, range: Range<usize>) -> Self {
        Self::Shared { storage, range }
    }

    pub(crate) fn covering(first: &Self, last: &Self) -> Option<Self> {
        let Self::Shared {
            storage: first_storage,
            range: first_range,
        } = first
        else {
            return None;
        };
        let Self::Shared {
            storage: last_storage,
            range: last_range,
        } = last
        else {
            return None;
        };
        Rc::ptr_eq(first_storage, last_storage).then(|| Self::Shared {
            storage: first_storage.clone(),
            range: first_range.start..last_range.end,
        })
    }

    pub(crate) fn len(&self) -> usize {
        match self {
            Self::Empty => 0,
            Self::Owned(source) => source.len(),
            Self::Shared { range, .. } => range.len(),
        }
    }

    pub(crate) fn iter(&self) -> ParserSourceIter<'_> {
        match self {
            Self::Empty => ParserSourceIter::Empty,
            Self::Owned(source) => ParserSourceIter::Owned(source.iter()),
            Self::Shared { storage, range } => match storage.as_ref() {
                SourceStorage::Ascii(source) => ParserSourceIter::Ascii(source[range.clone()].iter()),
                SourceStorage::Utf16(source) => ParserSourceIter::Utf16(source[range.clone()].iter()),
            },
        }
    }

    pub(crate) fn append_to(&self, target: &mut Vec<u16>) {
        match self {
            Self::Empty => {}
            Self::Owned(source) => target.extend_from_slice(source),
            Self::Shared { storage, range } => match storage.as_ref() {
                SourceStorage::Ascii(source) => target.extend(source[range.clone()].iter().copied().map(u16::from)),
                SourceStorage::Utf16(source) => target.extend_from_slice(&source[range.clone()]),
            },
        }
    }

    pub(crate) fn equals_ascii(&self, expected: &[u8]) -> bool {
        if self.len() != expected.len() {
            return false;
        }
        match self {
            Self::Empty => expected.is_empty(),
            Self::Owned(source) => source
                .iter()
                .zip(expected)
                .all(|(&left, &right)| left == u16::from(right)),
            Self::Shared { storage, range } => match storage.as_ref() {
                SourceStorage::Ascii(source) => &source[range.clone()] == expected,
                SourceStorage::Utf16(source) => source[range.clone()]
                    .iter()
                    .zip(expected)
                    .all(|(&left, &right)| left == u16::from(right)),
            },
        }
    }

    pub(crate) fn to_vec(&self) -> Vec<u16> {
        let mut result = Vec::with_capacity(self.len());
        self.append_to(&mut result);
        result
    }
}

impl PartialEq for ParserSource {
    fn eq(&self, other: &Self) -> bool {
        self.iter().eq(other.iter())
    }
}

impl Eq for ParserSource {}

pub(crate) fn tokenize_for_parser<'a>(input: impl Into<TokenizerInput<'a>>) -> Vec<ParserToken> {
    tokenize_for_parser_internal(input, true)
}

pub(crate) fn tokenize_for_parser_without_source<'a>(input: impl Into<TokenizerInput<'a>>) -> SmallParserTokenList {
    tokenize_for_parser_without_source_internal(input)
}

pub(crate) type SmallParserTokenList = SmallVec<[ParserToken; 8]>;

fn tokenize_for_parser_without_source_internal<'a>(input: impl Into<TokenizerInput<'a>>) -> SmallParserTokenList {
    let input = input.into();
    let mut tokens = SmallParserTokenList::new();
    tokenize(input, |token, _| {
        if matches!(token.token_type, TokenType::EndOfFile) {
            return;
        }
        let string = |value: &TokenString| ParserString::Inline(value.clone());
        let kind = match &token.token_type {
            TokenType::Ident { value } => ParserTokenKind::Ident(string(value)),
            TokenType::Function { name } => ParserTokenKind::Function(string(name)),
            TokenType::AtKeyword { name } => ParserTokenKind::AtKeyword(string(name)),
            TokenType::Hash { hash_type, value } => ParserTokenKind::Hash {
                value: string(value),
                is_id: *hash_type == CssHashType::Id,
            },
            TokenType::String { value } => ParserTokenKind::String(string(value)),
            TokenType::BadString => ParserTokenKind::BadString,
            TokenType::Url { value } => ParserTokenKind::Url(string(value)),
            TokenType::BadUrl => ParserTokenKind::BadUrl,
            TokenType::Delim { value } => ParserTokenKind::Delim(*value),
            TokenType::Number { number } => ParserTokenKind::Number {
                value: number.value,
                number_type: number.number_type,
            },
            TokenType::Percentage { number } => ParserTokenKind::Percentage {
                value: number.value,
                number_type: number.number_type,
            },
            TokenType::Dimension { number, unit } => ParserTokenKind::Dimension {
                value: number.value,
                number_type: number.number_type,
                unit: string(unit),
            },
            TokenType::Whitespace => ParserTokenKind::Whitespace,
            TokenType::Cdo => ParserTokenKind::Cdo,
            TokenType::Cdc => ParserTokenKind::Cdc,
            TokenType::Colon => ParserTokenKind::Colon,
            TokenType::Semicolon => ParserTokenKind::Semicolon,
            TokenType::Comma => ParserTokenKind::Comma,
            TokenType::OpenSquare => ParserTokenKind::OpenSquare,
            TokenType::CloseSquare => ParserTokenKind::CloseSquare,
            TokenType::OpenParen => ParserTokenKind::OpenParen,
            TokenType::CloseParen => ParserTokenKind::CloseParen,
            TokenType::OpenCurly => ParserTokenKind::OpenCurly,
            TokenType::CloseCurly => ParserTokenKind::CloseCurly,
            TokenType::EndOfFile => unreachable!(),
        };
        tokens.push(ParserToken {
            kind,
            source: ParserSource::Empty,
            start_position: token.range.start,
            end_position: token.range.end,
        });
    });
    tokens
}

fn tokenize_for_parser_internal<'a>(
    input: impl Into<TokenizerInput<'a>>,
    retain_original_source: bool,
) -> Vec<ParserToken> {
    let input = input.into();
    let estimated_token_count = input.len() / 3;
    let mut tokens = Vec::with_capacity(estimated_token_count);
    let mut string_storage = Vec::with_capacity(input.len() / 2);
    let source_storage = Rc::new(match input {
        TokenizerInput::Ascii(units) => SourceStorage::Ascii(units.into()),
        TokenizerInput::Utf16(units) => SourceStorage::Utf16(units.into()),
    });
    tokenize(input, |token, _| {
        if matches!(token.token_type, TokenType::EndOfFile) {
            return;
        }
        let mut string = |value: &[u16]| {
            let start = string_storage.len();
            string_storage.extend_from_slice(value);
            ParserString::Pending(start..string_storage.len())
        };
        let kind = match &token.token_type {
            TokenType::Ident { value } => ParserTokenKind::Ident(string(value)),
            TokenType::Function { name } => ParserTokenKind::Function(string(name)),
            TokenType::AtKeyword { name } => ParserTokenKind::AtKeyword(string(name)),
            TokenType::Hash { hash_type, value } => ParserTokenKind::Hash {
                value: string(value),
                is_id: *hash_type == CssHashType::Id,
            },
            TokenType::String { value } => ParserTokenKind::String(string(value)),
            TokenType::BadString => ParserTokenKind::BadString,
            TokenType::Url { value } => ParserTokenKind::Url(string(value)),
            TokenType::BadUrl => ParserTokenKind::BadUrl,
            TokenType::Delim { value } => ParserTokenKind::Delim(*value),
            TokenType::Number { number } => ParserTokenKind::Number {
                value: number.value,
                number_type: number.number_type,
            },
            TokenType::Percentage { number } => ParserTokenKind::Percentage {
                value: number.value,
                number_type: number.number_type,
            },
            TokenType::Dimension { number, unit } => ParserTokenKind::Dimension {
                value: number.value,
                number_type: number.number_type,
                unit: string(unit),
            },
            TokenType::Whitespace => ParserTokenKind::Whitespace,
            TokenType::Cdo => ParserTokenKind::Cdo,
            TokenType::Cdc => ParserTokenKind::Cdc,
            TokenType::Colon => ParserTokenKind::Colon,
            TokenType::Semicolon => ParserTokenKind::Semicolon,
            TokenType::Comma => ParserTokenKind::Comma,
            TokenType::OpenSquare => ParserTokenKind::OpenSquare,
            TokenType::CloseSquare => ParserTokenKind::CloseSquare,
            TokenType::OpenParen => ParserTokenKind::OpenParen,
            TokenType::CloseParen => ParserTokenKind::CloseParen,
            TokenType::OpenCurly => ParserTokenKind::OpenCurly,
            TokenType::CloseCurly => ParserTokenKind::CloseCurly,
            TokenType::EndOfFile => unreachable!(),
        };
        let source = if retain_original_source {
            token.original_source_range.clone()
        } else {
            0..0
        };
        tokens.push(ParserToken {
            kind,
            source: if retain_original_source {
                ParserSource::shared(source_storage.clone(), source)
            } else {
                ParserSource::Empty
            },
            start_position: token.range.start,
            end_position: token.range.end,
        });
    });
    let string_storage: Rc<[u16]> = string_storage.into();
    for token in &mut tokens {
        token.kind.finish_shared_strings(string_storage.clone(), 0);
    }
    tokens
}

pub(crate) fn tokenize_owned<'a>(input: impl Into<TokenizerInput<'a>>) -> Vec<OwnedToken> {
    let input = input.into();
    let storage = Rc::new(match input {
        TokenizerInput::Ascii(units) => SourceStorage::Ascii(units.into()),
        TokenizerInput::Utf16(units) => SourceStorage::Utf16(units.into()),
    });
    let mut tokens = Vec::new();
    tokenize(input, |token, _| {
        if matches!(token.token_type, TokenType::EndOfFile) {
            return;
        }
        let kind = match &token.token_type {
            TokenType::Ident { value } => OwnedTokenKind::Ident(value.to_vec()),
            TokenType::Function { name } => OwnedTokenKind::Function(name.to_vec()),
            TokenType::AtKeyword { .. } => OwnedTokenKind::AtKeyword,
            TokenType::Hash { .. } => OwnedTokenKind::Hash,
            TokenType::Url { .. } => OwnedTokenKind::Url,
            TokenType::BadUrl => OwnedTokenKind::BadUrl,
            TokenType::Number { .. } => OwnedTokenKind::Number,
            TokenType::Percentage { .. } => OwnedTokenKind::Percentage,
            TokenType::Dimension { .. } => OwnedTokenKind::Dimension,
            TokenType::Cdc => OwnedTokenKind::Cdc,
            TokenType::Delim { value } => OwnedTokenKind::Delim(*value),
            TokenType::Whitespace => OwnedTokenKind::Whitespace,
            TokenType::Comma => OwnedTokenKind::Comma,
            TokenType::OpenSquare => OwnedTokenKind::OpenSquare,
            TokenType::CloseSquare => OwnedTokenKind::CloseSquare,
            TokenType::OpenParen => OwnedTokenKind::OpenParen,
            TokenType::CloseParen => OwnedTokenKind::CloseParen,
            TokenType::OpenCurly => OwnedTokenKind::OpenCurly,
            TokenType::CloseCurly => OwnedTokenKind::CloseCurly,
            _ => OwnedTokenKind::Other,
        };
        tokens.push(OwnedToken {
            kind,
            source: ParserSource::shared(storage.clone(), token.original_source_range.clone()),
        });
    });
    tokens
}

type TokenString = SmallVec<[u16; 16]>;

enum TokenType {
    EndOfFile,
    Ident { value: TokenString },
    Function { name: TokenString },
    AtKeyword { name: TokenString },
    Hash { hash_type: CssHashType, value: TokenString },
    String { value: TokenString },
    BadString,
    Url { value: TokenString },
    BadUrl,
    Delim { value: u32 },
    Number { number: NumericValue },
    Percentage { number: NumericValue },
    Dimension { number: NumericValue, unit: TokenString },
    Whitespace,
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

impl Token {
    fn create(token_type: TokenType, original_source_range: Range<usize>) -> Self {
        Self {
            token_type,
            original_source_range,
            range: SourcePosition::default()..SourcePosition::default(),
        }
    }

    fn as_syntax_ffi(&self) -> CssSyntaxToken {
        let token_type = match &self.token_type {
            TokenType::EndOfFile => CssTokenType::EndOfFile,
            TokenType::Ident { .. } => CssTokenType::Ident,
            TokenType::Function { .. } => CssTokenType::Function,
            TokenType::AtKeyword { .. } => CssTokenType::AtKeyword,
            TokenType::Hash { .. } => CssTokenType::Hash,
            TokenType::String { .. } => CssTokenType::String,
            TokenType::BadString => CssTokenType::BadString,
            TokenType::Url { .. } => CssTokenType::Url,
            TokenType::BadUrl => CssTokenType::BadUrl,
            TokenType::Delim { .. } => CssTokenType::Delim,
            TokenType::Number { .. } => CssTokenType::Number,
            TokenType::Percentage { .. } => CssTokenType::Percentage,
            TokenType::Dimension { .. } => CssTokenType::Dimension,
            TokenType::Whitespace => CssTokenType::Whitespace,
            TokenType::Cdo => CssTokenType::CDO,
            TokenType::Cdc => CssTokenType::CDC,
            TokenType::Colon => CssTokenType::Colon,
            TokenType::Semicolon => CssTokenType::Semicolon,
            TokenType::Comma => CssTokenType::Comma,
            TokenType::OpenSquare => CssTokenType::OpenSquare,
            TokenType::CloseSquare => CssTokenType::CloseSquare,
            TokenType::OpenParen => CssTokenType::OpenParen,
            TokenType::CloseParen => CssTokenType::CloseParen,
            TokenType::OpenCurly => CssTokenType::OpenCurly,
            TokenType::CloseCurly => CssTokenType::CloseCurly,
        };

        CssSyntaxToken {
            token_type,
            start_line: self.range.start.line,
            start_column: self.range.start.column,
            end_line: self.range.end.line,
            end_column: self.range.end.column,
        }
    }
}

pub(crate) fn tokenize<F>(filtered_input: TokenizerInput<'_>, callback: F)
where
    F: FnMut(&Token, TokenizerInput<'_>),
{
    Tokenizer::new(filtered_input).tokenize(callback);
}

struct Tokenizer<'a> {
    input: TokenizerInput<'a>,
    index: usize,
    prev_index: usize,
    current_code_point: u32,
    position: SourcePosition,
    prev_position: SourcePosition,
}

impl<'a> Tokenizer<'a> {
    fn new(input: TokenizerInput<'a>) -> Self {
        Self {
            input,
            index: 0,
            prev_index: 0,
            current_code_point: TOKENIZER_EOF,
            position: SourcePosition::default(),
            prev_position: SourcePosition::default(),
        }
    }

    fn tokenize<F>(mut self, mut callback: F)
    where
        F: FnMut(&Token, TokenizerInput<'a>),
    {
        loop {
            let token_start = self.position;
            let mut token = self.consume_a_token();
            token.range = token_start..self.position;
            let is_eof = matches!(token.token_type, TokenType::EndOfFile);
            callback(&token, self.input);

            if is_eof {
                return;
            }
        }
    }

    fn current_byte_offset(&self) -> usize {
        self.index
    }

    fn current_code_point(&self) -> u32 {
        self.current_code_point
    }

    fn consume_code_point(&mut self) -> u32 {
        let Some((code_point, length)) = self.code_point_at(self.index) else {
            return TOKENIZER_EOF;
        };

        self.prev_index = self.index;
        self.prev_position = self.position;
        self.current_code_point = code_point;
        self.index += length;

        if is_newline(code_point) {
            self.position.line += 1;
            self.position.column = 0;
        } else {
            self.position.column += 1;
        }

        code_point
    }

    fn peek_code_point(&self, offset: usize) -> u32 {
        let mut index = self.index;
        for _ in 0..offset {
            let Some((_, length)) = self.code_point_at(index) else {
                return TOKENIZER_EOF;
            };
            index += length;
        }
        self.code_point_at(index)
            .map(|(code_point, _)| code_point)
            .unwrap_or(TOKENIZER_EOF)
    }

    fn code_point_at(&self, index: usize) -> Option<(u32, usize)> {
        match self.input {
            TokenizerInput::Ascii(units) => {
                debug_assert!(units.is_ascii());
                units.get(index).map(|unit| (u32::from(*unit), 1))
            }
            TokenizerInput::Utf16(units) => {
                let first = *units.get(index)?;
                if (0xD800..=0xDBFF).contains(&first)
                    && let Some(&second) = units.get(index + 1)
                    && (0xDC00..=0xDFFF).contains(&second)
                {
                    return Some((
                        0x10000 + ((u32::from(first) - 0xD800) << 10) + (u32::from(second) - 0xDC00),
                        2,
                    ));
                }
                Some((u32::from(first), 1))
            }
        }
    }

    fn peek_twin(&self) -> (u32, u32) {
        (self.peek_code_point(0), self.peek_code_point(1))
    }

    fn peek_triplet(&self) -> (u32, u32, u32) {
        (
            self.peek_code_point(0),
            self.peek_code_point(1),
            self.peek_code_point(2),
        )
    }

    fn start_of_input_stream_twin(&mut self) -> (u32, u32) {
        (self.current_code_point(), self.peek_code_point(0))
    }

    fn start_of_input_stream_triplet(&mut self) -> (u32, u32, u32) {
        (
            self.current_code_point(),
            self.peek_code_point(0),
            self.peek_code_point(1),
        )
    }

    fn reconsume_current_input_code_point(&mut self) {
        self.index = self.prev_index;
        self.position = self.prev_position;
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-comment
    fn consume_comments(&mut self) {
        // This section describes how to consume comments from a stream of code points.
        // It returns nothing.

        loop {
            // If the next two input code point are U+002F SOLIDUS (/) followed by a U+002A ASTERISK (*),
            // consume them and all following code points up to and including the first U+002A ASTERISK (*)
            // followed by a U+002F SOLIDUS (/), or up to an EOF code point. Return to the start of this step.
            //
            // If the preceding paragraph ended by consuming an EOF code point, this is a parse error.
            //
            // Return nothing.
            let (first, second) = self.peek_twin();
            if !(is_solidus(first) && is_asterisk(second)) {
                return;
            }

            self.consume_code_point();
            self.consume_code_point();

            loop {
                let (first, second) = self.peek_twin();
                if is_eof(first) || is_eof(second) {
                    return;
                }

                if is_asterisk(first) && is_solidus(second) {
                    self.consume_code_point();
                    self.consume_code_point();
                    break;
                }

                self.consume_code_point();
            }
        }
    }

    fn consume_as_much_whitespace_as_possible(&mut self) {
        while is_whitespace(self.peek_code_point(0)) {
            self.consume_code_point();
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-escaped-code-point
    fn consume_escaped_code_point(&mut self) -> u32 {
        // This section describes how to consume an escaped code point.
        // It assumes that the U+005C REVERSE SOLIDUS (\) has already been consumed and that the next
        // input code point has already been verified to be part of a valid escape.
        // It will return a code point.

        // Consume the next input code point.
        let input = self.consume_code_point();

        // hex digit
        if is_hex_digit(input) {
            let mut repr = SmallVec::<[u8; 8]>::new();
            append_ascii_code_point(&mut repr, input);

            // Consume as many hex digits as possible, but no more than 5.
            // Note that this means 1-6 hex digits have been consumed in total.
            let mut counter = 0usize;
            while is_hex_digit(self.peek_code_point(0)) && counter < 5 {
                counter += 1;
                append_ascii_code_point(&mut repr, self.consume_code_point());
            }

            // If the next input code point is whitespace, consume it as well.
            if is_whitespace(self.peek_code_point(0)) {
                self.consume_code_point();
            }

            // Interpret the hex digits as a hexadecimal number.
            let unhexed = u32::from_str_radix(unsafe { std::str::from_utf8_unchecked(&repr) }, 16).unwrap_or(0);
            // If this number is zero, or is for a surrogate, or is greater than the maximum allowed
            // code point, return U+FFFD REPLACEMENT CHARACTER (�).
            if unhexed == 0 || is_unicode_surrogate(unhexed) || is_greater_than_maximum_allowed_code_point(unhexed) {
                return REPLACEMENT_CHARACTER;
            }

            // Otherwise, return the code point with that value.
            return unhexed;
        }

        // EOF
        if is_eof(input) {
            // This is a parse error. Return U+FFFD REPLACEMENT CHARACTER (�).
            return REPLACEMENT_CHARACTER;
        }

        // anything else
        // Return the current input code point.
        input
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-ident-like-token
    fn consume_an_ident_like_token(&mut self) -> Token {
        // This section describes how to consume an ident-like token from a stream of code points.
        // It returns an <ident-token>, <function-token>, <url-token>, or <bad-url-token>.

        // Consume an ident sequence, and let string be the result.
        let start_byte_offset = self.current_byte_offset();
        let string = self.consume_an_ident_sequence();

        // If string’s value is an ASCII case-insensitive match for "url", and the next input code
        // point is U+0028 LEFT PARENTHESIS ((), consume it.
        if utf16_equals_ascii_case_insensitive(&string, b"url") && is_left_paren(self.peek_code_point(0)) {
            self.consume_code_point();

            // While the next two input code points are whitespace, consume the next input code point.
            loop {
                let (first, second) = self.peek_twin();
                if !(is_whitespace(first) && is_whitespace(second)) {
                    break;
                }
                self.consume_code_point();
            }

            // If the next one or two input code points are U+0022 QUOTATION MARK ("), U+0027 APOSTROPHE ('),
            // or whitespace followed by U+0022 QUOTATION MARK (") or U+0027 APOSTROPHE ('), then create a
            // <function-token> with its value set to string and return it.
            let (first, second) = self.peek_twin();
            if is_quotation_mark(first)
                || is_apostrophe(first)
                || (is_whitespace(first) && (is_quotation_mark(second) || is_apostrophe(second)))
            {
                return Token::create(
                    TokenType::Function { name: string },
                    start_byte_offset..self.current_byte_offset(),
                );
            }

            // Otherwise, consume a url token, and return it.
            return self.consume_a_url_token(start_byte_offset);
        }

        // Otherwise, if the next input code point is U+0028 LEFT PARENTHESIS ((), consume it.
        if is_left_paren(self.peek_code_point(0)) {
            self.consume_code_point();

            // Create a <function-token> with its value set to string and return it.
            return Token::create(
                TokenType::Function { name: string },
                start_byte_offset..self.current_byte_offset(),
            );
        }

        // Otherwise, create an <ident-token> with its value set to string and return it.
        Token::create(
            TokenType::Ident { value: string },
            start_byte_offset..self.current_byte_offset(),
        )
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-number
    fn consume_a_number(&mut self) -> NumericValue {
        // This section describes how to consume a number from a stream of code points.
        // It returns a numeric value, and a type which is either "integer" or "number".
        //
        // Note: This algorithm does not do the verification of the first few code points
        // that are necessary to ensure a number can be obtained from the stream. Ensure
        // that the stream starts with a number before calling this algorithm.

        // Execute the following steps in order:

        // 1. Initially set type to "integer". Let repr be the empty string.
        let mut repr = SmallVec::<[u8; 32]>::new();
        let mut number_type = CssNumberType::Integer;

        // 2. If the next input code point is U+002B PLUS SIGN (+) or U+002D HYPHEN-MINUS (-),
        // consume it and append it to repr.
        let mut has_explicit_sign = false;
        let next_input = self.peek_code_point(0);
        if is_plus_sign(next_input) || is_hyphen_minus(next_input) {
            has_explicit_sign = true;
            append_ascii_code_point(&mut repr, self.consume_code_point());
        }

        // 3. While the next input code point is a digit, consume it and append it to repr.
        while is_digit(self.peek_code_point(0)) {
            append_ascii_code_point(&mut repr, self.consume_code_point());
        }

        // 4. If the next 2 input code points are U+002E FULL STOP (.) followed by a digit, then:
        let (first, second) = self.peek_twin();
        if is_full_stop(first) && is_digit(second) {
            // 1. Consume them.
            // 2. Append them to repr.
            append_ascii_code_point(&mut repr, self.consume_code_point());
            append_ascii_code_point(&mut repr, self.consume_code_point());

            // 3. Set type to "number".
            number_type = CssNumberType::Number;

            // 4. While the next input code point is a digit, consume it and append it to repr.
            while is_digit(self.peek_code_point(0)) {
                append_ascii_code_point(&mut repr, self.consume_code_point());
            }
        }

        // 5. If the next 2 or 3 input code points are U+0045 LATIN CAPITAL LETTER E (E) or
        // U+0065 LATIN SMALL LETTER E (e), optionally followed by U+002D HYPHEN-MINUS (-)
        // or U+002B PLUS SIGN (+), followed by a digit, then:
        let (first, second, third) = self.peek_triplet();
        if (is_e(first) || is_uppercase_e(first))
            && (((is_plus_sign(second) || is_hyphen_minus(second)) && is_digit(third)) || is_digit(second))
        {
            // 1. Consume them.
            // 2. Append them to repr.
            if (is_plus_sign(second) || is_hyphen_minus(second)) && is_digit(third) {
                append_ascii_code_point(&mut repr, self.consume_code_point());
                append_ascii_code_point(&mut repr, self.consume_code_point());
                append_ascii_code_point(&mut repr, self.consume_code_point());
            } else if is_digit(second) {
                append_ascii_code_point(&mut repr, self.consume_code_point());
                append_ascii_code_point(&mut repr, self.consume_code_point());
            }

            // 3. Set type to "number".
            number_type = CssNumberType::Number;

            // 4. While the next input code point is a digit, consume it and append it to repr.
            while is_digit(self.peek_code_point(0)) {
                append_ascii_code_point(&mut repr, self.consume_code_point());
            }
        }

        // 6. Convert repr to a number, and set the value to the returned value.
        let value = unsafe { std::str::from_utf8_unchecked(&repr) }
            .parse::<f64>()
            .unwrap()
            .clamp(f64::from(f32::MIN), f64::from(f32::MAX));

        // 7. Return value and type.
        if number_type == CssNumberType::Integer && has_explicit_sign {
            return NumericValue {
                number_type: CssNumberType::IntegerWithExplicitSign,
                value,
            };
        }

        NumericValue { number_type, value }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-name
    fn consume_an_ident_sequence(&mut self) -> TokenString {
        // This section describes how to consume an ident sequence from a stream of code points.
        // It returns a string containing the largest name that can be formed from adjacent
        // code points in the stream, starting from the first.
        //
        // Note: This algorithm does not do the verification of the first few code points that
        // are necessary to ensure the returned code points would constitute an <ident-token>.
        // If that is the intended use, ensure that the stream starts with an ident sequence before
        // calling this algorithm.

        // Let result initially be an empty string.
        let mut result = TokenString::new();

        // Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            if is_eof(input) {
                break;
            }

            // name code point
            if is_ident_code_point(input) {
                // Append the code point to result.
                append_code_point(&mut result, input);
                continue;
            }

            // the stream starts with a valid escape
            if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                // Consume an escaped code point. Append the returned code point to result.
                append_code_point(&mut result, self.consume_escaped_code_point());
                continue;
            }

            // anything else
            // Reconsume the current input code point. Return result.
            self.reconsume_current_input_code_point();
            break;
        }

        result
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-url-token
    fn consume_a_url_token(&mut self, start_byte_offset: usize) -> Token {
        // This section describes how to consume a url token from a stream of code points.
        // It returns either a <url-token> or a <bad-url-token>.
        //
        // Note: This algorithm assumes that the initial "url(" has already been consumed.
        // This algorithm also assumes that it’s being called to consume an "unquoted" value,
        // like url(foo). A quoted value, like url("foo"), is parsed as a <function-token>.
        // Consume an ident-like token automatically handles this distinction; this algorithm
        // shouldn’t be called directly otherwise.

        // 1. Initially create a <url-token> with its value set to the empty string.
        let mut value = TokenString::new();

        // 2. Consume as much whitespace as possible.
        self.consume_as_much_whitespace_as_possible();

        // 3. Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            // U+0029 RIGHT PARENTHESIS ())
            if is_right_paren(input) {
                // Return the <url-token>.
                return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
            }

            // EOF
            if is_eof(input) {
                // This is a parse error. Return the <url-token>.
                return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
            }

            // whitespace
            if is_whitespace(input) {
                // Consume as much whitespace as possible.
                self.consume_as_much_whitespace_as_possible();
                let next_input = self.peek_code_point(0);

                // If the next input code point is U+0029 RIGHT PARENTHESIS ()) or EOF, consume it
                // and return the <url-token> (if EOF was encountered, this is a parse error);
                if is_right_paren(next_input) {
                    self.consume_code_point();
                    return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
                }

                if is_eof(next_input) {
                    self.consume_code_point();
                    return Token::create(TokenType::Url { value }, start_byte_offset..self.current_byte_offset());
                }

                // otherwise, consume the remnants of a bad url, create a <bad-url-token>, and return it.
                self.consume_the_remnants_of_a_bad_url();
                return Token::create(TokenType::BadUrl, start_byte_offset..self.current_byte_offset());
            }

            // U+0022 QUOTATION MARK (")
            // U+0027 APOSTROPHE (')
            // U+0028 LEFT PARENTHESIS (()
            // non-printable code point
            if is_quotation_mark(input)
                || is_apostrophe(input)
                || is_left_paren(input)
                || is_non_printable_code_point(input)
            {
                // This is a parse error. Consume the remnants of a bad url, create a <bad-url-token>, and return it.
                self.consume_the_remnants_of_a_bad_url();
                return Token::create(TokenType::BadUrl, start_byte_offset..self.current_byte_offset());
            }

            // U+005C REVERSE SOLIDUS (\)
            if is_reverse_solidus(input) {
                // If the stream starts with a valid escape,
                if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                    // consume an escaped code point and append the returned code point to the <url-token>’s value.
                    append_code_point(&mut value, self.consume_escaped_code_point());
                    continue;
                }

                // Otherwise, this is a parse error.
                // Consume the remnants of a bad url, create a <bad-url-token>, and return it.
                self.consume_the_remnants_of_a_bad_url();
                return Token::create(TokenType::BadUrl, start_byte_offset..self.current_byte_offset());
            }

            // anything else
            // Append the current input code point to the <url-token>’s value.
            append_code_point(&mut value, input);
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-remnants-of-bad-url
    fn consume_the_remnants_of_a_bad_url(&mut self) {
        // This section describes how to consume the remnants of a bad url from a stream of code points,
        // "cleaning up" after the tokenizer realizes that it’s in the middle of a <bad-url-token> rather
        // than a <url-token>. It returns nothing; its sole use is to consume enough of the input stream
        // to reach a recovery point where normal tokenizing can resume.

        // Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            // U+0029 RIGHT PARENTHESIS ())
            // EOF
            if is_eof(input) || is_right_paren(input) {
                // Return.
                return;
            }

            // the input stream starts with a valid escape
            if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                // Consume an escaped code point.
                // This allows an escaped right parenthesis ("\)") to be encountered without ending
                // the <bad-url-token>. This is otherwise identical to the "anything else" clause.
                self.consume_escaped_code_point();
            }

            // anything else
            // Do nothing.
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-numeric-token
    fn consume_a_numeric_token(&mut self) -> Token {
        // This section describes how to consume a numeric token from a stream of code points.
        // It returns either a <number-token>, <percentage-token>, or <dimension-token>.

        let start_byte_offset = self.current_byte_offset();

        // Consume a number and let number be the result.
        let number = self.consume_a_number();

        // If the next 3 input code points would start an ident sequence, then:
        if would_start_an_ident_sequence(self.peek_triplet()) {
            // 1. Create a <dimension-token> with the same value and type flag as number,
            //    and a unit set initially to the empty string.

            // 2. Consume an ident sequence. Set the <dimension-token>’s unit to the returned value.
            let unit = self.consume_an_ident_sequence();

            // 3. Return the <dimension-token>.
            return Token::create(
                TokenType::Dimension { number, unit },
                start_byte_offset..self.current_byte_offset(),
            );
        }

        // Otherwise, if the next input code point is U+0025 PERCENTAGE SIGN (%), consume it.
        if is_percent(self.peek_code_point(0)) {
            self.consume_code_point();

            // Create a <percentage-token> with the same value as number, and return it.
            return Token::create(
                TokenType::Percentage { number },
                start_byte_offset..self.current_byte_offset(),
            );
        }

        // Otherwise, create a <number-token> with the same value and type flag as number, and return it.
        Token::create(
            TokenType::Number { number },
            start_byte_offset..self.current_byte_offset(),
        )
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-string-token
    fn consume_string_token(&mut self, ending_code_point: u32) -> Token {
        // This section describes how to consume a string token from a stream of code points.
        // It returns either a <string-token> or <bad-string-token>.
        //
        // This algorithm may be called with an ending code point, which denotes the code point
        // that ends the string. If an ending code point is not specified, the current input
        // code point is used.

        // Initially create a <string-token> with its value set to the empty string.
        let start_byte_offset = self.current_byte_offset() - 1;
        let mut value = TokenString::new();

        // Repeatedly consume the next input code point from the stream:
        loop {
            let input = self.consume_code_point();

            // ending code point
            if input == ending_code_point {
                // Return the <string-token>.
                return Token::create(
                    TokenType::String { value },
                    start_byte_offset..self.current_byte_offset(),
                );
            }

            // EOF
            if is_eof(input) {
                // This is a parse error. Return the <string-token>.
                return Token::create(
                    TokenType::String { value },
                    start_byte_offset..self.current_byte_offset(),
                );
            }

            // newline
            if is_newline(input) {
                // This is a parse error. Reconsume the current input code point, create a
                // <bad-string-token>, and return it.
                self.reconsume_current_input_code_point();
                return Token::create(TokenType::BadString, start_byte_offset..self.current_byte_offset());
            }

            // U+005C REVERSE SOLIDUS (\)
            if is_reverse_solidus(input) {
                // If the next input code point is EOF, do nothing.
                let next_input = self.peek_code_point(0);
                if is_eof(next_input) {
                    continue;
                }

                // Otherwise, if the next input code point is a newline, consume it.
                if is_newline(next_input) {
                    self.consume_code_point();
                    continue;
                }

                // Otherwise, (the stream starts with a valid escape) consume an escaped code
                // point and append the returned code point to the <string-token>’s value.
                append_code_point(&mut value, self.consume_escaped_code_point());
                continue;
            }

            // anything else
            // Append the current input code point to the <string-token>’s value.
            append_code_point(&mut value, input);
        }
    }

    // https://www.w3.org/TR/css-syntax-3/#consume-token
    fn consume_a_token(&mut self) -> Token {
        // This section describes how to consume a token from a stream of code points.
        // It will return a single token of any type.

        let start_byte_offset = self.current_byte_offset();

        // Consume comments.
        self.consume_comments();

        // AD-HOC: Preserve comments as whitespace tokens, for serializing custom properties.
        let after_comments_byte_offset = self.current_byte_offset();
        if after_comments_byte_offset != start_byte_offset {
            return Token::create(TokenType::Whitespace, start_byte_offset..self.current_byte_offset());
        }

        // Consume the next input code point.
        let input = self.consume_code_point();

        match input {
            // whitespace
            c if is_whitespace(c) => {
                // Consume as much whitespace as possible. Return a <whitespace-token>.
                self.consume_as_much_whitespace_as_possible();
                Token::create(TokenType::Whitespace, start_byte_offset..self.current_byte_offset())
            }

            // U+0022 QUOTATION MARK (")
            0x22 => {
                // Consume a string token and return it.
                self.consume_string_token(input)
            }

            // U+0023 NUMBER SIGN (#)
            0x23 => {
                // If the next input code point is an ident code point or the next two input code points
                // are a valid escape, then:
                if is_ident_code_point(self.peek_code_point(0)) || is_valid_escape_sequence(self.peek_twin()) {
                    // 1. Create a <hash-token>.
                    // 2. If the next 3 input code points would start an ident sequence, set the <hash-token>’s
                    //    type flag to "id".
                    let hash_type = if would_start_an_ident_sequence(self.peek_triplet()) {
                        CssHashType::Id
                    } else {
                        CssHashType::Unrestricted
                    };

                    // 3. Consume an ident sequence, and set the <hash-token>’s value to the returned string.
                    let value = self.consume_an_ident_sequence();

                    // 4. Return the <hash-token>.
                    Token::create(
                        TokenType::Hash { hash_type, value },
                        start_byte_offset..self.current_byte_offset(),
                    )
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+0027 APOSTROPHE (')
            0x27 => {
                // Consume a string token and return it.
                self.consume_string_token(input)
            }

            // U+0028 LEFT PARENTHESIS (()
            0x28 => {
                // Return a <(-token>.
                Token::create(TokenType::OpenParen, start_byte_offset..self.current_byte_offset())
            }

            // U+0029 RIGHT PARENTHESIS ())
            0x29 => {
                // Return a <)-token>.
                Token::create(TokenType::CloseParen, start_byte_offset..self.current_byte_offset())
            }

            // U+002B PLUS SIGN (+)
            0x2B => {
                // If the input stream starts with a number, reconsume the current input code point,
                // consume a numeric token and return it.
                if would_start_a_number(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    self.consume_a_numeric_token()
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+002C COMMA (,)
            0x2C => {
                // Return a <comma-token>.
                Token::create(TokenType::Comma, start_byte_offset..self.current_byte_offset())
            }

            // U+002D HYPHEN-MINUS (-)
            0x2D => {
                // If the input stream starts with a number, reconsume the current input code point,
                // consume a numeric token, and return it.
                if would_start_a_number(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    return self.consume_a_numeric_token();
                }

                // Otherwise, if the next 2 input code points are U+002D HYPHEN-MINUS U+003E
                // GREATER-THAN SIGN (->), consume them and return a <CDC-token>.
                let (first, second) = self.peek_twin();
                if is_hyphen_minus(first) && is_greater_than_sign(second) {
                    self.consume_code_point();
                    self.consume_code_point();
                    return Token::create(TokenType::Cdc, start_byte_offset..self.current_byte_offset());
                }

                // Otherwise, if the input stream starts with an identifier, reconsume the current
                // input code point, consume an ident-like token, and return it.
                if would_start_an_ident_sequence(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    return self.consume_an_ident_like_token();
                }

                // Otherwise, return a <delim-token> with its value set to the current input code point.
                Token::create(
                    TokenType::Delim { value: input },
                    start_byte_offset..self.current_byte_offset(),
                )
            }

            // U+002E FULL STOP (.)
            0x2E => {
                // If the input stream starts with a number, reconsume the current input code point,
                // consume a numeric token, and return it.
                if would_start_a_number(self.start_of_input_stream_triplet()) {
                    self.reconsume_current_input_code_point();
                    self.consume_a_numeric_token()
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+003A COLON (:)
            0x3A => {
                // Return a <colon-token>.
                Token::create(TokenType::Colon, start_byte_offset..self.current_byte_offset())
            }

            // U+003B SEMICOLON (;)
            0x3B => {
                // Return a <semicolon-token>.
                Token::create(TokenType::Semicolon, start_byte_offset..self.current_byte_offset())
            }

            // U+003C LESS-THAN SIGN (<)
            0x3C => {
                // If the next 3 input code points are U+0021 EXCLAMATION MARK U+002D HYPHEN-MINUS
                // U+002D HYPHEN-MINUS (!--), consume them and return a <CDO-token>.
                let (first, second, third) = self.peek_triplet();
                if is_exclamation_mark(first) && is_hyphen_minus(second) && is_hyphen_minus(third) {
                    self.consume_code_point();
                    self.consume_code_point();
                    self.consume_code_point();
                    Token::create(TokenType::Cdo, start_byte_offset..self.current_byte_offset())
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+0040 COMMERCIAL AT (@)
            0x40 => {
                // If the next 3 input code points would start an ident sequence, consume an ident sequence, create
                // an <at-keyword-token> with its value set to the returned value, and return it.
                if would_start_an_ident_sequence(self.peek_triplet()) {
                    let name = self.consume_an_ident_sequence();
                    Token::create(
                        TokenType::AtKeyword { name },
                        start_byte_offset..self.current_byte_offset(),
                    )
                } else {
                    // Otherwise, return a <delim-token> with its value set to the current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+005B LEFT SQUARE BRACKET ([)
            0x5B => {
                // Return a <[-token>.
                Token::create(TokenType::OpenSquare, start_byte_offset..self.current_byte_offset())
            }

            // U+005C REVERSE SOLIDUS (\)
            0x5C => {
                // If the input stream starts with a valid escape, reconsume the current input code point,
                // consume an ident-like token, and return it.
                if is_valid_escape_sequence(self.start_of_input_stream_twin()) {
                    self.reconsume_current_input_code_point();
                    self.consume_an_ident_like_token()
                } else {
                    // Otherwise, this is a parse error. Return a <delim-token> with its value set to the
                    // current input code point.
                    Token::create(
                        TokenType::Delim { value: input },
                        start_byte_offset..self.current_byte_offset(),
                    )
                }
            }

            // U+005D RIGHT SQUARE BRACKET (])
            0x5D => {
                // Return a <]-token>.
                Token::create(TokenType::CloseSquare, start_byte_offset..self.current_byte_offset())
            }

            // U+007B LEFT CURLY BRACKET ({)
            0x7B => {
                // Return a <{-token>.
                Token::create(TokenType::OpenCurly, start_byte_offset..self.current_byte_offset())
            }

            // U+007D RIGHT CURLY BRACKET (})
            0x7D => {
                // Return a <}-token>.
                Token::create(TokenType::CloseCurly, start_byte_offset..self.current_byte_offset())
            }

            // digit
            0x30..=0x39 => {
                // Reconsume the current input code point, consume a numeric token, and return it.
                self.reconsume_current_input_code_point();
                self.consume_a_numeric_token()
            }

            // EOF
            TOKENIZER_EOF => {
                // Return an <EOF-token>.
                Token::create(TokenType::EndOfFile, start_byte_offset..self.current_byte_offset())
            }

            // name-start code point
            c if is_ident_start_code_point(c) => {
                // Reconsume the current input code point, consume an ident-like token, and return it.
                self.reconsume_current_input_code_point();
                self.consume_an_ident_like_token()
            }

            // anything else
            _ => {
                // Return a <delim-token> with its value set to the current input code point.
                Token::create(
                    TokenType::Delim { value: input },
                    start_byte_offset..self.current_byte_offset(),
                )
            }
        }
    }
}

fn append_code_point(builder: &mut impl Extend<u16>, code_point: u32) {
    if code_point < 0x10000 {
        builder.extend(std::iter::once(code_point as u16));
        return;
    }
    let value = code_point - 0x10000;
    builder.extend([0xD800 + (value >> 10) as u16, 0xDC00 + (value & 0x3FF) as u16]);
}

fn append_ascii_code_point(builder: &mut impl Extend<u8>, code_point: u32) {
    debug_assert!(code_point <= 0x7f);
    builder.extend(std::iter::once(code_point as u8));
}

fn utf16_equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| left <= 0x7f && (left as u8).eq_ignore_ascii_case(&right))
}

fn is_eof(code_point: u32) -> bool {
    code_point == TOKENIZER_EOF
}

fn is_ascii(code_point: u32) -> bool {
    code_point <= 0x7F
}

fn is_ascii_alpha(code_point: u32) -> bool {
    (0x41..=0x5A).contains(&code_point) || (0x61..=0x7A).contains(&code_point)
}

fn is_unicode(code_point: u32) -> bool {
    code_point <= 0x10FFFF
}

fn is_unicode_surrogate(code_point: u32) -> bool {
    (0xD800..=0xDFFF).contains(&code_point)
}

fn is_digit(code_point: u32) -> bool {
    (0x30..=0x39).contains(&code_point)
}

fn is_hex_digit(code_point: u32) -> bool {
    is_digit(code_point) || (0x41..=0x46).contains(&code_point) || (0x61..=0x66).contains(&code_point)
}

fn is_ident_start_code_point(code_point: u32) -> bool {
    is_ascii_alpha(code_point) || (!is_ascii(code_point) && is_unicode(code_point)) || code_point == '_' as u32
}

fn is_ident_code_point(code_point: u32) -> bool {
    is_ident_start_code_point(code_point) || is_digit(code_point) || code_point == '-' as u32
}

fn is_non_printable_code_point(code_point: u32) -> bool {
    code_point <= 0x8 || code_point == 0xB || (0xE..=0x1F).contains(&code_point) || code_point == 0x7F
}

fn is_newline(code_point: u32) -> bool {
    code_point == 0x0A
}

fn is_whitespace(code_point: u32) -> bool {
    is_newline(code_point) || code_point == '\t' as u32 || code_point == ' ' as u32
}

fn is_greater_than_maximum_allowed_code_point(code_point: u32) -> bool {
    code_point > 0x10FFFF
}

fn is_quotation_mark(code_point: u32) -> bool {
    code_point == 0x22
}

fn is_hyphen_minus(code_point: u32) -> bool {
    code_point == 0x2D
}

fn is_reverse_solidus(code_point: u32) -> bool {
    code_point == 0x5C
}

fn is_apostrophe(code_point: u32) -> bool {
    code_point == 0x27
}

fn is_left_paren(code_point: u32) -> bool {
    code_point == 0x28
}

fn is_right_paren(code_point: u32) -> bool {
    code_point == 0x29
}

fn is_plus_sign(code_point: u32) -> bool {
    code_point == 0x2B
}

fn is_full_stop(code_point: u32) -> bool {
    code_point == 0x2E
}

fn is_asterisk(code_point: u32) -> bool {
    code_point == 0x2A
}

fn is_solidus(code_point: u32) -> bool {
    code_point == 0x2F
}

fn is_greater_than_sign(code_point: u32) -> bool {
    code_point == 0x3E
}

fn is_percent(code_point: u32) -> bool {
    code_point == 0x25
}

fn is_exclamation_mark(code_point: u32) -> bool {
    code_point == 0x21
}

fn is_e(code_point: u32) -> bool {
    code_point == 0x65
}

fn is_uppercase_e(code_point: u32) -> bool {
    code_point == 0x45
}

// https://www.w3.org/TR/css-syntax-3/#starts-with-a-valid-escape
fn is_valid_escape_sequence((first, second): (u32, u32)) -> bool {
    // This section describes how to check if two code points are a valid escape.
    // The algorithm described here can be called explicitly with two code points,
    // or can be called with the input stream itself. In the latter case, the two
    // code points in question are the current input code point and the next input
    // code point, in that order.
    //
    // Note: This algorithm will not consume any additional code point.

    // If the first code point is not U+005C REVERSE SOLIDUS (\), return false.
    if !is_reverse_solidus(first) {
        return false;
    }

    // Otherwise, if the second code point is a newline, return false.
    if is_newline(second) {
        return false;
    }

    // Otherwise, return true.
    true
}

// https://www.w3.org/TR/css-syntax-3/#would-start-an-identifier
fn would_start_an_ident_sequence((first, second, third): (u32, u32, u32)) -> bool {
    // This section describes how to check if three code points would start an ident sequence.
    // The algorithm described here can be called explicitly with three code points, or
    // can be called with the input stream itself. In the latter case, the three code
    // points in question are the current input code point and the next two input code
    // points, in that order.
    //
    // Note: This algorithm will not consume any additional code points.

    // Look at the first code point:

    // U+002D HYPHEN-MINUS
    if is_hyphen_minus(first) {
        // If the second code point is a name-start code point or a U+002D HYPHEN-MINUS,
        // or the second and third code points are a valid escape, return true.
        if is_ident_start_code_point(second) || is_hyphen_minus(second) || is_valid_escape_sequence((second, third)) {
            return true;
        }
        // Otherwise, return false.
        return false;
    }

    // name-start code point
    if is_ident_start_code_point(first) {
        // Return true.
        return true;
    }

    // U+005C REVERSE SOLIDUS (\)
    if is_reverse_solidus(first) {
        // If the first and second code points are a valid escape, return true.
        if is_valid_escape_sequence((first, second)) {
            return true;
        }
        // Otherwise, return false.
        return false;
    }

    // anything else
    // Return false.
    false
}

// https://www.w3.org/TR/css-syntax-3/#starts-with-a-number
fn would_start_a_number((first, second, third): (u32, u32, u32)) -> bool {
    // This section describes how to check if three code points would start a number.
    // The algorithm described here can be called explicitly with three code points,
    // or can be called with the input stream itself. In the latter case, the three
    // code points in question are the current input code point and the next two input
    // code points, in that order.
    //
    // Note: This algorithm will not consume any additional code points.

    // Look at the first code point:

    // U+002B PLUS SIGN (+)
    // U+002D HYPHEN-MINUS (-)
    if is_plus_sign(first) || is_hyphen_minus(first) {
        // If the second code point is a digit, return true.
        if is_digit(second) {
            return true;
        }

        // Otherwise, if the second code point is a U+002E FULL STOP (.) and the third
        // code point is a digit, return true.
        if is_full_stop(second) && is_digit(third) {
            return true;
        }

        // Otherwise, return false.
        return false;
    }

    // U+002E FULL STOP (.)
    if is_full_stop(first) {
        // If the second code point is a digit, return true. Otherwise, return false.
        return is_digit(second);
    }

    // digit
    if is_digit(first) {
        // Return true.
        return true;
    }

    // anything else
    // Return false.
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    fn debug_string(value: &[u16]) -> String {
        format!("{:?}", String::from_utf16_lossy(value))
    }

    fn debug_number(value: f64) -> String {
        if value.is_finite() && value.abs() < 1e21 && value.fract() == 0.0 {
            return format!("{value:.0}");
        }
        let value = format!("{value:?}");
        if let Some((mantissa, exponent)) = value.split_once('e')
            && !exponent.starts_with(['+', '-'])
        {
            return format!("{mantissa}e+{exponent}");
        }
        value
    }

    fn debug_token(token: &Token, input: TokenizerInput<'_>) -> String {
        let source = match input {
            TokenizerInput::Ascii(units) => units[token.original_source_range.clone()]
                .iter()
                .copied()
                .map(u16::from)
                .collect::<Vec<_>>(),
            TokenizerInput::Utf16(units) => units[token.original_source_range.clone()].to_vec(),
        };
        let (name, mut fields) = match &token.token_type {
            TokenType::EndOfFile => ("__EOF__", vec![]),
            TokenType::Ident { value } => ("Ident", vec![format!("value={}", debug_string(value))]),
            TokenType::Function { name } => ("Function", vec![format!("value={}", debug_string(name))]),
            TokenType::AtKeyword { name } => ("AtKeyword", vec![format!("value={}", debug_string(name))]),
            TokenType::Hash { hash_type, value } => (
                "Hash",
                vec![
                    format!("value={}", debug_string(value)),
                    format!("hash_type={hash_type:?}"),
                ],
            ),
            TokenType::String { value } => ("String", vec![format!("value={}", debug_string(value))]),
            TokenType::BadString => ("BadString", vec![]),
            TokenType::Url { value } => ("Url", vec![format!("value={}", debug_string(value))]),
            TokenType::BadUrl => ("BadUrl", vec![]),
            TokenType::Delim { value } => (
                "Delim",
                vec![
                    format!("value={}", debug_string(&[*value as u16])),
                    format!("code_point=U+{value:04X}"),
                ],
            ),
            TokenType::Number { number } => (
                "Number",
                vec![
                    format!("value={}", debug_number(number.value)),
                    format!("number_type={:?}", number.number_type),
                ],
            ),
            TokenType::Percentage { number } => (
                "Percentage",
                vec![
                    format!("value={}", debug_number(number.value)),
                    format!("number_type={:?}", number.number_type),
                ],
            ),
            TokenType::Dimension { number, unit } => (
                "Dimension",
                vec![
                    format!("value={}", debug_number(number.value)),
                    format!("number_type={:?}", number.number_type),
                    format!("unit={}", debug_string(unit)),
                ],
            ),
            TokenType::Whitespace => ("Whitespace", vec![]),
            TokenType::Cdo => ("CDO", vec![]),
            TokenType::Cdc => ("CDC", vec![]),
            TokenType::Colon => ("Colon", vec![]),
            TokenType::Semicolon => ("Semicolon", vec![]),
            TokenType::Comma => ("Comma", vec![]),
            TokenType::OpenSquare => ("OpenSquare", vec![]),
            TokenType::CloseSquare => ("CloseSquare", vec![]),
            TokenType::OpenParen => ("OpenParen", vec![]),
            TokenType::CloseParen => ("CloseParen", vec![]),
            TokenType::OpenCurly => ("OpenCurly", vec![]),
            TokenType::CloseCurly => ("CloseCurly", vec![]),
        };
        fields.push(format!("source={}", debug_string(&source)));
        fields.push(format!("start={}:{}", token.range.start.line, token.range.start.column));
        fields.push(format!("end={}:{}", token.range.end.line, token.range.end.column));
        format!("{name}({})", fields.join(", "))
    }

    fn normalize_utf16(input: Vec<u16>) -> Vec<u16> {
        let mut output = Vec::with_capacity(input.len());
        let mut index = 0;
        while index < input.len() {
            match input[index] {
                0x0d if input.get(index + 1) == Some(&0x0a) => {
                    output.push(0x0a);
                    index += 2;
                }
                0x0d | 0x0c => {
                    output.push(0x0a);
                    index += 1;
                }
                0 => {
                    output.push(REPLACEMENT_CHARACTER as u16);
                    index += 1;
                }
                value => {
                    output.push(value);
                    index += 1;
                }
            }
        }
        output
    }

    fn check_tokenizer_corpus(input: Vec<u16>, expected: &str) {
        let input = normalize_utf16(input);
        let mut actual = Vec::new();
        tokenize(TokenizerInput::Utf16(&input), |token, input| {
            actual.push(debug_token(token, input));
        });
        assert_eq!(actual.join("\n") + "\n", expected);
    }

    #[test]
    fn tokenizer_golden_corpus() {
        macro_rules! check_utf8 {
            ($name:literal) => {
                check_tokenizer_corpus(
                    String::from_utf8(
                        include_bytes!(concat!(
                            "../../../../../Tests/LibWeb/CSSTokenizer/input/",
                            $name,
                            ".css"
                        ))
                        .to_vec(),
                    )
                    .unwrap()
                    .encode_utf16()
                    .collect(),
                    include_str!(concat!(
                        "../../../../../Tests/LibWeb/CSSTokenizer/expected/",
                        $name,
                        ".txt"
                    )),
                );
            };
        }

        check_utf8!("at-hash-function-delim");
        check_utf8!("basic-rules");
        check_utf8!("brackets-and-cdo-cdc");
        check_utf8!("comments-and-whitespace");
        check_utf8!("malformed-bad-string-recovery");
        check_utf8!("malformed-bad-url-recovery");
        check_utf8!("malformed-invalid-escape-extra-braces");
        check_utf8!("malformed-unterminated-comment");
        check_utf8!("malformed-unterminated-string-eof");
        check_utf8!("numeric-clamping");
        check_utf8!("numeric-tokens");
        check_utf8!("strings-and-escapes");
        check_utf8!("urls-and-bad-url");

        let utf16le = include_bytes!("../../../../../Tests/LibWeb/CSSTokenizer/input/utf-16le-crlf.css");
        let utf16le = utf16le
            .chunks_exact(2)
            .map(|bytes| u16::from_le_bytes([bytes[0], bytes[1]]))
            .skip_while(|&unit| unit == 0xfeff)
            .collect();
        check_tokenizer_corpus(
            utf16le,
            include_str!("../../../../../Tests/LibWeb/CSSTokenizer/expected/utf-16le-crlf.txt"),
        );

        let windows_1252 =
            include_bytes!("../../../../../Tests/LibWeb/CSSTokenizer/input/windows-1252-curly-quotes.css")
                .iter()
                .map(|&byte| match byte {
                    0x93 => 0x201c,
                    0x94 => 0x201d,
                    _ => u16::from(byte),
                })
                .collect();
        check_tokenizer_corpus(
            windows_1252,
            include_str!("../../../../../Tests/LibWeb/CSSTokenizer/expected/windows-1252-curly-quotes.txt"),
        );
    }

    #[test]
    fn ascii_and_utf16_inputs_tokenize_equivalently() {
        let ascii = b"color: rgb(1 2 3)";
        let utf16 = ascii.iter().copied().map(u16::from).collect::<Vec<_>>();

        assert_eq!(tokenize_owned(ascii), tokenize_owned(&utf16));
    }

    #[test]
    fn token_sources_preserve_the_input_storage_representation() {
        let ascii_tokens = tokenize_owned(b"color");
        let ParserSource::Shared { storage, range } = &ascii_tokens[0].source else {
            panic!("token source should use shared storage");
        };
        assert!(matches!(storage.as_ref(), SourceStorage::Ascii(_)));
        assert_eq!(range.clone(), 0..5);

        let utf16 = "café".encode_utf16().collect::<Vec<_>>();
        let utf16_tokens = tokenize_owned(&utf16);
        let ParserSource::Shared { storage, range } = &utf16_tokens[0].source else {
            panic!("token source should use shared storage");
        };
        assert!(matches!(storage.as_ref(), SourceStorage::Utf16(_)));
        assert_eq!(range.clone(), 0..utf16.len());
        assert_eq!(utf16_tokens[0].source.to_vec(), utf16);
    }

    #[test]
    fn syntax_highlighting_tokens_only_contain_types_and_positions() {
        let input = b"color:\n rgb(1 2 3)";
        let mut tokens = Vec::<CssSyntaxToken>::new();

        unsafe extern "C" fn append_token(ctx: *mut c_void, token: *const CssSyntaxToken) {
            unsafe {
                let tokens = &mut *ctx.cast::<Vec<CssSyntaxToken>>();
                tokens.push(*token);
            }
        }

        unsafe {
            rust_css_tokenize_for_syntax_highlighting(
                input.as_ptr(),
                ptr::null(),
                input.len(),
                (&raw mut tokens).cast(),
                append_token,
            );
        }

        let tokens = tokens
            .iter()
            .map(|token| {
                (
                    token.token_type,
                    token.start_line,
                    token.start_column,
                    token.end_line,
                    token.end_column,
                )
            })
            .collect::<Vec<_>>();
        assert_eq!(
            tokens,
            vec![
                (CssTokenType::Ident, 0, 0, 0, 5),
                (CssTokenType::Colon, 0, 5, 0, 6),
                (CssTokenType::Whitespace, 0, 6, 1, 1),
                (CssTokenType::Function, 1, 1, 1, 5),
                (CssTokenType::Number, 1, 5, 1, 6),
                (CssTokenType::Whitespace, 1, 6, 1, 7),
                (CssTokenType::Number, 1, 7, 1, 8),
                (CssTokenType::Whitespace, 1, 8, 1, 9),
                (CssTokenType::Number, 1, 9, 1, 10),
                (CssTokenType::CloseParen, 1, 10, 1, 11),
                (CssTokenType::EndOfFile, 1, 11, 1, 11),
            ]
        );
    }
}
