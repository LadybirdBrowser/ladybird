/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The structure of a style sheet: its rules, their preludes and the declarations in their blocks. The parser reads
//! the tokens of the lexer as it goes, and holds one token. It makes no tree: it gives each rule and each declaration
//! to a sink as it finds them.
//!
//! The prelude of an at-rule is made into component values, for the grammar that reads it. The prelude of a qualified
//! rule and the value of a declaration are not: the parser finds where they end, and says where they are in the
//! source. Whoever needs their tokens or component values makes them from there.
//!
//! https://drafts.csswg.org/css-syntax-3/#parsing

use super::{
    AtRule, Declaration, FfiSyntaxDiagnostic, FfiSyntaxDiagnosticCode, Nested, OuterRuleName, QualifiedRule,
    QualifiedRuleResult, Rule, RuleContext, context_for_at_rule, equals_ascii_case_insensitive,
    is_accepted_position_try_property, is_font_feature_value_rule_name, is_margin_rule_name, starts_with_ascii,
};
use crate::css::css_tokenizer::{
    CssNumberType, ParserSource, ParserString, ParserTokenKind, SourcePosition, SourceStorage, TokenizerInput,
    filter_code_points,
};
use crate::css::parser::block::Unit;
use crate::css::parser::component_value::{
    ComponentKind, ComponentValue, ComponentValueList, MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH,
};
use crate::css::parser::lexer::{
    Escapes, Kind, Lexer, Text, Token, flag, name_of, number_of, string_of, unescape, url_of,
};
use crate::css::parser::rows::{Name, Rows, closer_of};
use crate::css::property_metadata::property_id_from_name;
use crate::css::selector_parser::StyleNestingParent;
use smallvec::SmallVec;
use std::borrow::Cow;
use std::ops::Range;
use std::sync::Arc;

/// The text of `input` as a parser reads it: preprocessed, and held for the component values that point into it.
// https://drafts.csswg.org/css-syntax-3/#input-preprocessing
pub(super) fn source_of(input: TokenizerInput<'_>) -> Arc<SourceStorage> {
    Arc::new(SourceStorage::from_input(filter_code_points(input).as_input()))
}

fn parser_string(text: Text<'_>) -> ParserString {
    fn widen<T: Copy + Into<u16>>(units: &[T]) -> ParserString {
        if units.len() <= 16 {
            ParserString::Inline(units.iter().map(|&unit| unit.into()).collect())
        } else {
            ParserString::Owned(units.iter().map(|&unit| unit.into()).collect())
        }
    }
    match text {
        Text::Source(TokenizerInput::Latin1(units)) => widen(units),
        Text::Source(TokenizerInput::Utf16(units)) => widen(units),
        Text::Unescaped(units) if units.len() <= 16 => ParserString::Inline(SmallVec::from_slice(&units)),
        Text::Unescaped(units) => ParserString::Owned(units.into_boxed_slice()),
    }
}

fn source_end(value: &ComponentValue) -> usize {
    match &value.original_source_text {
        ParserSource::Shared { range, .. } => range.end,
        _ => unreachable!("a component value of the parser is in the source"),
    }
}

/// The tokens of a source, one at a time, and the component values they make.
struct Stream<'a, U: Unit> {
    lexer: Lexer<'a, U>,
    units: &'a [U],
    storage: &'a Arc<SourceStorage>,
    /// The next token.
    token: Token,
    /// Where the token before the next one ends, which is where the whitespace and comments before the next one start.
    previous_end: usize,
    /// Where the stream ends: a token that starts here or later is the end of the source.
    limit: usize,
    /// Where the bad string and bad URL tokens are, and whether each is a URL.
    bad_tokens: Vec<(u32, u32, bool)>,
    /// The memory for the component values of the functions and blocks that are being consumed, one after the other.
    children: Vec<ComponentValue>,
    /// Where the last token that the stream has handed out ends. The stream goes back when a declaration turns out to
    /// be a rule, and reads tokens before this a second time.
    seen_end: usize,
}

impl<'a, U: Unit> Stream<'a, U> {
    fn new(units: &'a [U], storage: &'a Arc<SourceStorage>, range: Range<usize>) -> Self {
        let mut lexer = Lexer::new(units);
        lexer.set_position(range.start);
        let mut stream = Self {
            units: lexer.units(),
            lexer,
            storage,
            token: Token {
                start: range.start as u32,
                end: range.start as u32,
                part: 0,
                kind: Kind::EndOfFile,
                flags: 0,
            },
            previous_end: range.start,
            limit: range.end,
            bad_tokens: Vec::new(),
            children: Vec::new(),
            seen_end: range.start,
        };
        stream.advance();
        stream
    }

    #[inline]
    fn advance(&mut self) {
        self.previous_end = self.token.end as usize;
        let mut token = self.lexer.next();
        if token.start as usize >= self.limit {
            let limit = self.limit as u32;
            // NB: Whitespace and comments before the limit are before the token that starts there. The end of the
            //     source is such a token too, and the whitespace and comments after the limit are not in the stream.
            let flags = if self.previous_end < self.limit {
                token.flags & (flag::SPACE_BEFORE | flag::COMMENT_BEFORE)
            } else {
                0
            };
            token = Token {
                start: limit,
                end: limit,
                part: limit,
                kind: Kind::EndOfFile,
                flags,
            };
        }
        if token.end as usize > self.seen_end {
            self.seen_end = token.end as usize;
            if matches!(token.kind, Kind::BadString | Kind::BadUrl) {
                self.bad_tokens
                    .push((token.start, token.end, token.kind == Kind::BadUrl));
            }
        }
        self.token = token;
    }

    /// Where to go back to, to read the next token again.
    fn checkpoint(&self) -> usize {
        self.previous_end
    }

    fn restore(&mut self, checkpoint: usize) {
        self.lexer.set_position(checkpoint);
        self.token.end = checkpoint as u32;
        self.advance();
    }

    fn source(&self, start: usize, end: usize) -> ParserSource {
        ParserSource::shared(self.storage.clone(), start..end)
    }

    fn name(&self, token: Token) -> ParserString {
        parser_string(name_of(self.units, token))
    }

    fn kind_of(&self, token: Token) -> ParserTokenKind {
        let number = || {
            let (start, end) = token.number_range();
            let number_type = if !token.has(flag::INTEGER) {
                CssNumberType::Number
            } else if token.has(flag::SIGNED) {
                CssNumberType::IntegerWithExplicitSign
            } else {
                CssNumberType::Integer
            };
            (number_of(&self.units[start..end]), number_type)
        };
        match token.kind {
            Kind::EndOfFile => ParserTokenKind::EndOfFile,
            Kind::Ident => ParserTokenKind::Ident(self.name(token)),
            Kind::Function => ParserTokenKind::Function(self.name(token)),
            Kind::AtKeyword => ParserTokenKind::AtKeyword(self.name(token)),
            Kind::Hash => ParserTokenKind::Hash {
                value: self.name(token),
                is_id: token.has(flag::ID),
            },
            Kind::String => ParserTokenKind::String(parser_string(string_of(self.units, token))),
            Kind::BadString => ParserTokenKind::BadString,
            Kind::Url => ParserTokenKind::Url(parser_string(url_of(self.units, token))),
            Kind::BadUrl => ParserTokenKind::BadUrl,
            Kind::Delim => ParserTokenKind::Delim(self.units[token.start as usize].code()),
            Kind::Number => {
                let (value, number_type) = number();
                ParserTokenKind::Number { value, number_type }
            }
            Kind::Percentage => {
                let (value, number_type) = number();
                ParserTokenKind::Percentage { value, number_type }
            }
            Kind::Dimension => {
                let (value, number_type) = number();
                ParserTokenKind::Dimension {
                    value,
                    number_type,
                    unit: self.name(token),
                }
            }
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

    fn token_value(&self, kind: ParserTokenKind, start: usize, end: usize) -> ComponentValue {
        ComponentValue {
            kind: ComponentKind::Token(kind),
            original_source_text: self.source(start, end),
            opening_source_length: end - start,
            closing_source_length: 0,
            start_position: SourcePosition::default(),
            end_position: SourcePosition::default(),
        }
    }

    /// Appends the whitespace and comments before the next token to `values`, as the whitespace tokens that component
    /// values have for them: one for each run of whitespace, and one for each run of comments.
    ///
    /// The next token then no longer says that it follows them: a block that the source ends in has them, and what
    /// the block is in must not have them too.
    #[inline]
    fn push_gap(&mut self, values: &mut Vec<ComponentValue>) {
        if self.token.follows_a_gap() {
            self.push_gap_slow(values);
            self.token.flags &= !(flag::SPACE_BEFORE | flag::COMMENT_BEFORE);
        }
    }

    fn push_gap_slow(&self, values: &mut Vec<ComponentValue>) {
        let (start, end) = (self.previous_end, self.token.start as usize);
        if !self.token.has(flag::COMMENT_BEFORE) {
            values.push(self.token_value(ParserTokenKind::Whitespace, start, end));
            return;
        }
        let at = |index: usize| self.units[index].code();
        let mut position = start;
        while position < end {
            let run_start = position;
            if at(position) == 0x2F {
                // NB: Whitespace and comments are all that is here, so a solidus starts a comment.
                loop {
                    let mut close = position + 2;
                    while close + 1 < end && !(at(close) == 0x2A && at(close + 1) == 0x2F) {
                        close += 1;
                    }
                    position = if close + 1 < end { close + 2 } else { end };
                    if !(position + 1 < end && at(position) == 0x2F && at(position + 1) == 0x2A) {
                        break;
                    }
                }
            } else {
                while position < end && at(position) != 0x2F {
                    position += 1;
                }
            }
            values.push(self.token_value(ParserTokenKind::Whitespace, run_start, position));
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-component-value
    fn consume_component_value(&mut self, depth: usize) -> Result<ComponentValue, ()> {
        if depth > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
            return Err(());
        }
        let token = self.token;
        self.advance();
        let (start, end) = (token.start as usize, token.end as usize);
        let Some(closer) = closer_of(token.kind) else {
            return Ok(self.token_value(self.kind_of(token), start, end));
        };
        if depth + 1 > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
            return Err(());
        }
        // NB: The values of a function or block go on top of the ones of the functions and blocks that it is in, and
        //     then into memory of their own size.
        let first = self.children.len();
        let mut token_count = 0;
        loop {
            let mut children = std::mem::take(&mut self.children);
            let first_of_gap = children.len();
            self.push_gap(&mut children);
            token_count += children.len() - first_of_gap;
            self.children = children;
            if self.token.kind == closer || self.token.kind == Kind::EndOfFile {
                break;
            }
            let Ok(value) = self.consume_component_value(depth + 1) else {
                self.children.truncate(first);
                return Err(());
            };
            token_count += value.token_count();
            self.children.push(value);
        }
        let (source_end, closing_source_length) = if self.token.kind == closer {
            let closing = self.token;
            self.advance();
            (closing.end as usize, (closing.end - closing.start) as usize)
        } else {
            (self.children[first..].last().map_or(end, source_end), 0)
        };
        let values = ComponentValueList::with_token_count(
            self.children.drain(first..).collect::<Box<[ComponentValue]>>(),
            token_count,
        );
        Ok(ComponentValue {
            kind: if token.kind == Kind::Function {
                ComponentKind::Function {
                    name: self.name(token),
                    values,
                }
            } else {
                ComponentKind::SimpleBlock {
                    opening: self.kind_of(token),
                    values,
                }
            },
            original_source_text: self.source(start, source_end),
            opening_source_length: end - start,
            closing_source_length,
            start_position: SourcePosition::default(),
            end_position: SourcePosition::default(),
        })
    }

    /// Whether the name of `token` starts with two U+002D HYPHEN-MINUS (-).
    fn name_starts_with_dashes(&self, token: Token) -> bool {
        if token.has(flag::ESCAPED) {
            return starts_with_ascii(&self.name(token), b"--");
        }
        let (start, end) = token.name_range();
        end - start >= 2 && self.units[start].code() == 0x2D && self.units[start + 1].code() == 0x2D
    }

    /// Whether the name of `token` is `expected`, which is lowercase ASCII, in any case.
    fn name_is(&self, token: Token, expected: &[u8]) -> bool {
        if token.has(flag::ESCAPED) {
            return equals_ascii_case_insensitive(&self.name(token), expected);
        }
        let (start, end) = token.name_range();
        let name = &self.units[start..end];
        name.len() == expected.len()
            && name.iter().zip(expected).all(|(unit, &expected)| {
                let code = unit.code();
                let lowercase = if (0x41..=0x5A).contains(&code) {
                    code | 0x20
                } else {
                    code
                };
                lowercase == u32::from(expected)
            })
    }
}

/// The component values of the text in `range` of a source.
fn component_values_in<U: Unit>(units: &[U], storage: &Arc<SourceStorage>, range: Range<usize>) -> Vec<ComponentValue> {
    let mut values = Vec::with_capacity(4);
    append_component_values_in(units, storage, range, &mut values);
    values
}

fn append_component_values_in<U: Unit>(
    units: &[U],
    storage: &Arc<SourceStorage>,
    range: Range<usize>,
    values: &mut Vec<ComponentValue>,
) {
    thread_local! {
        static CHILDREN: std::cell::Cell<Vec<ComponentValue>> = const { std::cell::Cell::new(Vec::new()) };
    }
    let mut stream = Stream::new(units, storage, range);
    stream.children = CHILDREN.take();
    loop {
        stream.push_gap(values);
        if stream.token.kind == Kind::EndOfFile {
            break;
        }
        let Ok(value) = stream.consume_component_value(0) else {
            break;
        };
        values.push(value);
    }
    // NB: The thread keeps the memory for the next value, but not the memory of one very large value.
    if stream.children.capacity() <= 1024 {
        stream.children.clear();
        CHILDREN.set(stream.children);
    }
}

/// The value of a declaration: where it is in the source. It ends where the last component value before a
/// `!important` ends.
#[derive(Clone)]
pub(crate) struct DeclarationValue {
    source: Arc<SourceStorage>,
    /// Where the name of the declaration is, as written, and whether it has escapes.
    name: Range<u32>,
    name_is_escaped: bool,
    range: Range<u32>,
    /// Whether parsing the value may do more than produce a value, which sharing the result would skip: `attr()`
    /// records the attribute names it reads, and `random()` values have an identity of their own.
    pub(super) may_not_be_shared: bool,
}

impl DeclarationValue {
    /// The name of the declaration.
    pub(crate) fn name(&self) -> Name {
        fn name_in<U: Unit>(units: &[U], is_escaped: bool) -> Name {
            if is_escaped {
                let mut output = Name::new();
                unescape(units, Escapes::Name, &mut output);
                return output;
            }
            units.iter().map(|unit| unit.code() as u16).collect()
        }
        let range = self.name.start as usize..self.name.end as usize;
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => name_in(&units[range], self.name_is_escaped),
            SourceStorage::Utf16(units) => name_in(&units[range], self.name_is_escaped),
        }
    }

    fn range(&self) -> Range<usize> {
        self.range.start as usize..self.range.end as usize
    }

    /// The component values of the value.
    pub(crate) fn component_values(&self) -> Vec<ComponentValue> {
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => component_values_in(units, &self.source, self.range()),
            SourceStorage::Utf16(units) => component_values_in(units, &self.source, self.range()),
        }
    }

    /// Appends the component values of the value to `values`.
    pub(crate) fn append_component_values_to(&self, values: &mut Vec<ComponentValue>) {
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => append_component_values_in(units, &self.source, self.range(), values),
            SourceStorage::Utf16(units) => append_component_values_in(units, &self.source, self.range(), values),
        }
    }

    /// The text of the value.
    pub(crate) fn text(&self) -> Cow<'_, [u16]> {
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => Cow::Owned(units[self.range()].iter().copied().map(u16::from).collect()),
            SourceStorage::Utf16(units) => Cow::Borrowed(&units[self.range()]),
        }
    }

    /// The text of the value, which uses the memory of `scratch` where the source does not have it as UTF-16.
    pub(crate) fn text_in<'a>(&'a self, scratch: &'a mut Vec<u16>) -> &'a [u16] {
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => {
                scratch.clear();
                scratch.extend(units[self.range()].iter().copied().map(u16::from));
                scratch
            }
            SourceStorage::Utf16(units) => &units[self.range()],
        }
    }

    /// A hash of the declaration as written, without its `!important`.
    pub(super) fn hash_of_text(&self, hasher: &impl std::hash::BuildHasher) -> u64 {
        let name = self.name.start as usize..self.name.end as usize;
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => hasher.hash_one((&units[name], &units[self.range()])),
            SourceStorage::Utf16(units) => hasher.hash_one((&units[name], &units[self.range()])),
        }
    }

    /// Where the name and the value of the declaration are in the source.
    pub(super) fn ranges(&self) -> (Range<u32>, Range<u32>) {
        (self.name.clone(), self.range.clone())
    }

    /// Whether the declaration is written like one whose name and value are at `name` and `value` in the same source.
    pub(super) fn is_same_text_as(&self, name: &Range<u32>, value: &Range<u32>) -> bool {
        fn same<U: Unit>(units: &[U], first: &Range<u32>, second: &Range<u32>) -> bool {
            units[first.start as usize..first.end as usize] == units[second.start as usize..second.end as usize]
        }
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => same(units, &self.name, name) && same(units, &self.range, value),
            SourceStorage::Utf16(units) => same(units, &self.name, name) && same(units, &self.range, value),
        }
    }
}

impl PartialEq for DeclarationValue {
    fn eq(&self, other: &Self) -> bool {
        self.text() == other.text()
    }
}

impl std::fmt::Debug for DeclarationValue {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{:?}", String::from_utf16_lossy(&self.text()))
    }
}

/// Finds the line and column of places in a source that it is asked for in order.
#[derive(Default)]
struct LineCursor {
    offset: usize,
    line: usize,
    column: usize,
}

impl LineCursor {
    fn position<U: Unit>(&mut self, units: &[U], offset: usize) -> SourcePosition {
        if offset < self.offset {
            *self = Self::default();
        }
        // NB: The places are near each other, so this reads the units between them once.
        let units = &units[self.offset..offset];
        let mut lines = 0;
        let mut line_start = 0;
        // NB: The source has no lone surrogate, so each low surrogate is the second unit of a code point.
        let mut low_surrogates = 0;
        for (index, unit) in units.iter().enumerate() {
            let code = unit.code();
            if code == 0x0A {
                lines += 1;
                line_start = index + 1;
                low_surrogates = 0;
            } else if U::WIDE && code & 0xFC00 == 0xDC00 {
                low_surrogates += 1;
            }
        }
        if lines != 0 {
            self.line += lines;
            self.column = 0;
        }
        self.column += units.len() - line_start - low_surrogates;
        self.offset = offset;
        SourcePosition {
            line: self.line,
            column: self.column,
        }
    }
}

/// The prelude of a qualified rule: where it is in the source. It starts at its first token, and ends where the block of
/// the rule starts.
#[derive(Clone)]
pub(crate) struct Prelude {
    source: Arc<SourceStorage>,
    range: Range<u32>,
}

impl Prelude {
    pub(crate) fn range(&self) -> Range<usize> {
        self.range.start as usize..self.range.end as usize
    }

    /// The source that the prelude is in, which is preprocessed text.
    pub(crate) fn input(&self) -> TokenizerInput<'_> {
        self.source.as_input()
    }

    /// Whether the prelude has no token.
    pub(crate) fn is_empty(&self) -> bool {
        self.range.is_empty()
    }

    pub(crate) fn component_values(&self) -> Vec<ComponentValue> {
        match self.source.as_ref() {
            SourceStorage::Latin1(units) => component_values_in(units, &self.source, self.range()),
            SourceStorage::Utf16(units) => component_values_in(units, &self.source, self.range()),
        }
    }

    pub(crate) fn text(&self) -> Vec<u16> {
        let mut text = Vec::new();
        self.input().slice(self.range()).append_to(&mut text);
        text
    }
}

impl PartialEq for Prelude {
    fn eq(&self, other: &Self) -> bool {
        self.text() == other.text()
    }
}

impl std::fmt::Debug for Prelude {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "Prelude({:?})", String::from_utf16_lossy(&self.text()))
    }
}

/// What a component value at the top level of a declaration's value is to the declaration.
#[derive(Clone, Copy, PartialEq, Eq)]
enum TopLevel {
    Other,
    CurlyBlock,
    Bang,
    Important,
}

/// How deep the blocks of rules can be nested. The contents of a block that is deeper are skipped.
const MAXIMUM_RULE_NESTING_DEPTH: usize = 256;

/// What takes the rules and declarations of a source, in the order of the source.
pub(super) trait Sink {
    /// A rule starts. `rule` has no declarations and no children: they come next, up to [`Sink::end_rule`].
    /// `prelude` is the tokens of the prelude of a qualified rule that is a selector list, or none if they are nested
    /// too deep to be a list of component values.
    fn begin_rule(&mut self, rule: Rule, prelude: Option<&Rows>);
    fn end_rule(&mut self);
    /// A declaration of the list of declarations that is open, which the first declaration after a rule opens.
    fn declaration(&mut self, declaration: Declaration);
    /// The list of declarations that is open, if one is, ends here.
    fn end_declarations(&mut self);
}

/// Takes nothing.
struct NoSink;

impl Sink for NoSink {
    fn begin_rule(&mut self, _: Rule, _: Option<&Rows>) {}
    fn end_rule(&mut self) {}
    fn declaration(&mut self, _: Declaration) {}
    fn end_declarations(&mut self) {}
}

pub(super) struct Core<'a, U: Unit> {
    stream: Stream<'a, U>,
    rule_context: Vec<RuleContext>,
    rule_names: Vec<OuterRuleName>,
    lines: LineCursor,
    /// The tokens that end the blocks and functions that are open, while a component value is skipped.
    closers: Vec<Kind>,
    /// The tokens of the prelude of the qualified rule that is parsed, for the selector parser.
    prelude_rows: Rows,
}

impl<'a, U: Unit> Core<'a, U> {
    fn new(units: &'a [U], storage: &'a Arc<SourceStorage>, rule_context: Vec<RuleContext>) -> Self {
        let rule_names = rule_context.iter().copied().map(OuterRuleName::Context).collect();
        Self {
            stream: Stream::new(units, storage, 0..usize::MAX),
            rule_context,
            rule_names,
            lines: LineCursor::default(),
            closers: Vec::new(),
            prelude_rows: Rows::default(),
        }
    }

    #[inline]
    fn kind(&self) -> Kind {
        self.stream.token.kind
    }

    fn style_nesting_parent(&self) -> StyleNestingParent {
        self.rule_context
            .iter()
            .rev()
            .find_map(|context| match context {
                RuleContext::Style => Some(StyleNestingParent::Style),
                RuleContext::AtScope => Some(StyleNestingParent::Scope),
                _ => None,
            })
            .unwrap_or(StyleNestingParent::None)
    }

    /// Consumes the component value that the next token starts, without making it, and returns where it ends. Sets
    /// `may_not_be_shared` if an `attr()` or `random()` function is in it.
    #[inline]
    fn skip_component_value(&mut self, may_not_be_shared: &mut bool) -> Result<usize, ()> {
        self.skip_component_value_and::<false>(may_not_be_shared)
    }

    /// As [`Self::skip_component_value`]. With `RECORD`, appends the tokens of the component value to the rows of the
    /// prelude.
    #[inline(always)]
    fn skip_component_value_and<const RECORD: bool>(&mut self, may_not_be_shared: &mut bool) -> Result<usize, ()> {
        let token = self.stream.token;
        if RECORD {
            self.prelude_rows.push(token);
        }
        self.stream.advance();
        let Some(closer) = closer_of(token.kind) else {
            return Ok(token.end as usize);
        };
        let mut note_function = |stream: &Stream<'a, U>, token: Token| {
            if token.kind == Kind::Function && (stream.name_is(token, b"attr") || stream.name_is(token, b"random")) {
                *may_not_be_shared = true;
            }
        };
        note_function(&self.stream, token);
        self.closers.clear();
        self.closers.push(closer);
        loop {
            if self.closers.len() > MAXIMUM_COMPONENT_VALUE_NESTING_DEPTH {
                return Err(());
            }
            let token = self.stream.token;
            if token.kind == Kind::EndOfFile {
                return Ok(token.start as usize);
            }
            if RECORD {
                self.prelude_rows.push(token);
            }
            self.stream.advance();
            if self.closers.last() == Some(&token.kind) {
                self.closers.pop();
                if self.closers.is_empty() {
                    return Ok(token.end as usize);
                }
            } else if let Some(closer) = closer_of(token.kind) {
                note_function(&self.stream, token);
                self.closers.push(closer);
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-stylesheet-contents
    /// Consumes the next rule of the style sheet, and gives it to `sink`. Returns false once the input is exhausted.
    fn consume_next_stylesheet_rule<S: Sink>(&mut self, sink: &mut S) -> bool {
        loop {
            match self.kind() {
                Kind::Cdo | Kind::Cdc => self.stream.advance(),
                Kind::EndOfFile => return false,
                Kind::AtKeyword => {
                    if self.consume_at_rule(Nested::No, sink) {
                        return true;
                    }
                }
                _ => {
                    if self.consume_qualified_rule(None, Nested::No, sink).is_ok() {
                        return true;
                    }
                }
            }
        }
    }

    /// Consumes the one rule that the source is, and gives it to `sink`. Returns false if the source is not one rule,
    /// and `sink` can then have a rule that is not the source.
    fn consume_only_rule<S: Sink>(&mut self, nested: Nested, sink: &mut S) -> bool {
        let is_rule = match self.kind() {
            Kind::EndOfFile => return false,
            Kind::AtKeyword => self.consume_at_rule(nested, sink),
            _ => self.consume_qualified_rule(None, nested, sink).is_ok(),
        };
        is_rule && self.kind() == Kind::EndOfFile
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-at-rule
    /// Consumes an at-rule, and gives it to `sink`. Returns false if it is not a rule.
    fn consume_at_rule<S: Sink>(&mut self, nested: Nested, sink: &mut S) -> bool {
        debug_assert_eq!(self.kind(), Kind::AtKeyword);
        let name = self.stream.name(self.stream.token);
        self.stream.advance();
        let mut rule = AtRule {
            name,
            prelude: Vec::with_capacity(4),
            has_block: false,
            nesting_parent: self.style_nesting_parent(),
            valid_in_context: false,
            outer_rule_name: self.rule_names.last().cloned(),
        };
        loop {
            self.stream.push_gap(&mut rule.prelude);
            match self.kind() {
                Kind::EndOfFile | Kind::Semicolon => {
                    if self.kind() == Kind::Semicolon {
                        self.stream.advance();
                    }
                    break;
                }
                Kind::CloseCurly if nested == Nested::Yes => break,
                Kind::OpenCurly => {
                    rule.has_block = true;
                    rule.valid_in_context = self.at_rule_is_valid(&rule);
                    let context = context_for_at_rule(rule.name.as_ref());
                    let rule_name = OuterRuleName::At(rule.name.as_ref().into());
                    sink.begin_rule(Rule::At(rule), None);
                    self.rule_context.push(context);
                    self.rule_names.push(rule_name);
                    self.consume_block(sink);
                    self.rule_names.pop();
                    self.rule_context.pop();
                    sink.end_rule();
                    return true;
                }
                _ => {
                    let Ok(value) = self.stream.consume_component_value(0) else {
                        return false;
                    };
                    rule.prelude.push(value);
                }
            }
        }
        rule.valid_in_context = self.at_rule_is_valid(&rule);
        sink.begin_rule(Rule::At(rule), None);
        sink.end_rule();
        true
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-qualified-rule
    /// Consumes a qualified rule, and gives it to `sink`.
    fn consume_qualified_rule<S: Sink>(
        &mut self,
        stop: Option<Kind>,
        nested: Nested,
        sink: &mut S,
    ) -> Result<(), QualifiedRuleResult> {
        let qualified_context = if self.rule_context.last() == Some(&RuleContext::AtKeyframes) {
            RuleContext::Keyframe
        } else {
            RuleContext::Style
        };
        let start = self.stream.token.start;
        let is_selector = qualified_context == RuleContext::Style;
        self.prelude_rows.clear();
        let mut source_position = None;
        // Whether the first two component values are a name that starts with two dashes, and a colon.
        let mut count = 0usize;
        let mut starts_with_custom_name = false;
        let mut looks_like_custom_property = false;
        let mut may_not_be_shared = false;
        loop {
            let token = self.stream.token;
            let kind = token.kind;
            if kind == Kind::EndOfFile || stop == Some(kind) {
                return Err(QualifiedRuleResult::Invalid);
            }
            match kind {
                Kind::CloseCurly if nested == Nested::Yes => return Err(QualifiedRuleResult::Invalid),
                Kind::OpenCurly => {
                    if looks_like_custom_property {
                        if nested == Nested::Yes {
                            self.consume_bad_declaration(nested);
                        } else {
                            // NB: Nothing in the block is part of the style sheet.
                            self.consume_block(&mut NoSink);
                        }
                        return Err(QualifiedRuleResult::Invalid);
                    }
                    // NB: The declarations before the rule are a list of their own.
                    sink.end_declarations();
                    let rows_are_valid = is_selector && self.prelude_rows.finish(token, token.start as usize).is_ok();
                    let rule = Rule::Qualified(QualifiedRule {
                        prelude: Prelude {
                            source: self.stream.storage.clone(),
                            range: start..token.start,
                        },
                        prelude_is_selector: is_selector,
                        nesting_parent: self.style_nesting_parent(),
                        valid_in_context: self.qualified_rule_is_valid(),
                        outer_rule_name: self.rule_names.last().cloned(),
                        source_position,
                    });
                    sink.begin_rule(rule, rows_are_valid.then_some(&self.prelude_rows));
                    self.rule_context.push(qualified_context);
                    self.rule_names.push(OuterRuleName::Context(qualified_context));
                    self.consume_block(sink);
                    self.rule_names.pop();
                    self.rule_context.pop();
                    sink.end_rule();
                    return Ok(());
                }
                _ => {
                    if count == 0 {
                        source_position = Some(self.lines.position(self.stream.units, token.start as usize));
                        starts_with_custom_name = kind == Kind::Ident && self.stream.name_starts_with_dashes(token);
                    } else if count == 1 {
                        looks_like_custom_property = starts_with_custom_name && kind == Kind::Colon;
                    }
                    count += 1;
                    if is_selector {
                        self.skip_component_value_and::<true>(&mut may_not_be_shared)
                    } else {
                        self.skip_component_value(&mut may_not_be_shared)
                    }
                    .map_err(|()| QualifiedRuleResult::Invalid)?;
                }
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-block
    fn consume_block<S: Sink>(&mut self, sink: &mut S) {
        debug_assert_eq!(self.kind(), Kind::OpenCurly);
        // NB: The blocks of rules are consumed by recursion, so the contents of a block that is too deep are skipped.
        if self.rule_context.len() > MAXIMUM_RULE_NESTING_DEPTH {
            self.skip_block();
            return;
        }
        self.stream.advance();
        self.consume_block_contents(sink);
        if self.kind() == Kind::CloseCurly {
            self.stream.advance();
        }
    }

    /// Consumes a block and its contents, without making them.
    fn skip_block(&mut self) {
        debug_assert_eq!(self.kind(), Kind::OpenCurly);
        self.stream.advance();
        self.closers.clear();
        self.closers.push(Kind::CloseCurly);
        while !self.closers.is_empty() {
            let token = self.stream.token;
            if token.kind == Kind::EndOfFile {
                return;
            }
            self.stream.advance();
            if self.closers.last() == Some(&token.kind) {
                self.closers.pop();
            } else if let Some(closer) = closer_of(token.kind) {
                self.closers.push(closer);
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-block-contents
    /// Consumes the contents of a block, and gives the rules and the declarations in them to `sink`.
    fn consume_block_contents<S: Sink>(&mut self, sink: &mut S) {
        loop {
            match self.kind() {
                Kind::Semicolon => self.stream.advance(),
                Kind::EndOfFile | Kind::CloseCurly => {
                    sink.end_declarations();
                    return;
                }
                Kind::AtKeyword => {
                    sink.end_declarations();
                    self.consume_at_rule(Nested::Yes, sink);
                }
                kind => {
                    if kind == Kind::Ident {
                        let start = self.stream.checkpoint();
                        let name = self.stream.token;
                        self.stream.advance();
                        if self.kind() == Kind::Colon {
                            self.stream.advance();
                            if let Some(declaration) = self.consume_declaration_value(name) {
                                sink.declaration(declaration);
                                continue;
                            }
                        }
                        self.stream.restore(start);
                    }
                    if self
                        .consume_qualified_rule(Some(Kind::Semicolon), Nested::Yes, sink)
                        .is_err()
                        && self.kind() == Kind::Semicolon
                    {
                        self.stream.advance();
                    }
                }
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-declaration
    /// Consumes the rest of a declaration whose name is the token `name`, from after its colon.
    fn consume_declaration_value(&mut self, name_token: Token) -> Option<Declaration> {
        let value_start = self.stream.token.start as usize;
        let mut may_not_be_shared = false;
        // What the last two component values at the top level are, and where the last three end.
        let mut kinds = [TopLevel::Other; 2];
        let mut ends = [value_start; 3];
        let mut count = 0usize;
        let mut curly_block_count = 0usize;
        loop {
            let token = self.stream.token;
            let top_level = match token.kind {
                Kind::EndOfFile | Kind::Semicolon | Kind::CloseCurly => break,
                Kind::OpenCurly => TopLevel::CurlyBlock,
                Kind::Delim if self.stream.units[token.start as usize].code() == 0x21 => TopLevel::Bang,
                Kind::Ident if self.stream.name_is(token, b"important") => TopLevel::Important,
                _ => TopLevel::Other,
            };
            let Ok(end) = self.skip_component_value(&mut may_not_be_shared) else {
                break;
            };
            kinds = [kinds[1], top_level];
            ends = [ends[1], ends[2], end];
            count += 1;
            curly_block_count += usize::from(top_level == TopLevel::CurlyBlock);
        }
        let important = count >= 2 && kinds == [TopLevel::Bang, TopLevel::Important];
        let (value_end, count) = if important {
            (ends[0], count - 2)
        } else {
            (ends[2], count)
        };

        let is_custom_property = self.stream.name_starts_with_dashes(name_token);
        if is_custom_property {
            // NB: A name of only the two dashes is not a custom property name.
            let length = if name_token.has(flag::ESCAPED) {
                self.stream.name(name_token).as_ref().len()
            } else {
                (name_token.end - name_token.start) as usize
            };
            if length == 2 {
                return None;
            }
        }
        // INTEROP: The initial-value descriptor accepts arbitrary component values, including mixed curly blocks.
        let is_initial_value_descriptor = self.rule_context.last() == Some(&RuleContext::AtProperty)
            && self.stream.name_is(name_token, b"initial-value");
        if !is_custom_property && !is_initial_value_descriptor && curly_block_count != 0 && curly_block_count != count {
            return None;
        }
        if !self.declaration_is_valid(name_token) {
            return None;
        }
        let is_property = self.declaration_is_property();
        let rule_context = if !is_property && self.rule_context.contains(&RuleContext::AtFunction) {
            RuleContext::AtFunction
        } else {
            self.rule_context.last().copied().unwrap_or(RuleContext::Unknown)
        };
        Some(Declaration {
            value: DeclarationValue {
                source: self.stream.storage.clone(),
                name: name_token.start..name_token.end,
                name_is_escaped: name_token.has(flag::ESCAPED),
                range: value_start as u32..value_end as u32,
                may_not_be_shared,
            },
            important,
            source_position: self.lines.position(self.stream.units, name_token.start as usize),
            is_property,
            rule_context,
        })
    }

    fn consume_bad_declaration(&mut self, nested: Nested) {
        let mut may_not_be_shared = false;
        loop {
            match self.kind() {
                Kind::EndOfFile => return,
                Kind::Semicolon => {
                    self.stream.advance();
                    return;
                }
                Kind::CloseCurly if nested == Nested::Yes => return,
                _ => {
                    if self.skip_component_value(&mut may_not_be_shared).is_err() {
                        return;
                    }
                }
            }
        }
    }

    /// Whether a declaration whose name is the token `name` can be where the parser is.
    fn declaration_is_valid(&self, name: Token) -> bool {
        let Some(context) = self.rule_context.last() else {
            return false;
        };
        match context {
            RuleContext::Unknown | RuleContext::AtKeyframes => false,
            RuleContext::Keyframe => ![
                b"animation".as_slice(),
                b"animation-delay",
                b"animation-direction",
                b"animation-duration",
                b"animation-fill-mode",
                b"animation-iteration-count",
                b"animation-name",
                b"animation-play-state",
                b"animation-timeline",
            ]
            .iter()
            .any(|property| self.stream.name_is(name, property)),
            RuleContext::AtContainer | RuleContext::AtLayer | RuleContext::AtMedia | RuleContext::AtSupports => self
                .rule_context
                .iter()
                .any(|context| matches!(context, RuleContext::Style | RuleContext::AtFunction)),
            RuleContext::AtPositionTry => {
                property_id_from_name(self.stream.name(name).as_ref()).is_some_and(is_accepted_position_try_property)
            }
            _ => true,
        }
    }

    fn declaration_is_property(&self) -> bool {
        match self.rule_context.last() {
            Some(
                RuleContext::Style
                | RuleContext::Keyframe
                | RuleContext::AtScope
                | RuleContext::Margin
                | RuleContext::AtPositionTry,
            ) => true,
            Some(RuleContext::AtContainer | RuleContext::AtLayer | RuleContext::AtMedia | RuleContext::AtSupports) => {
                self.rule_context.contains(&RuleContext::Style)
            }
            _ => false,
        }
    }

    fn at_rule_is_valid(&self, rule: &AtRule) -> bool {
        let name = rule.name.as_ref();
        // NB: Margin and font feature value rules are only meaningful inside @page and
        //     @font-feature-values. The C++ converter dropped them anywhere else, so they must not
        //     count as valid rules for the top-level @import and @namespace ordering windows.
        if self.rule_context.is_empty() {
            return !is_margin_rule_name(name) && !is_font_feature_value_rule_name(name);
        }
        if self.rule_context.contains(&RuleContext::Style) {
            return [b"container".as_slice(), b"layer", b"media", b"scope", b"supports"]
                .iter()
                .any(|expected| equals_ascii_case_insensitive(name, expected));
        }
        if self.rule_context.contains(&RuleContext::AtFunction) {
            return [b"container".as_slice(), b"media", b"supports"]
                .iter()
                .any(|expected| equals_ascii_case_insensitive(name, expected));
        }
        match self.rule_context.last().unwrap() {
            RuleContext::AtContainer
            | RuleContext::AtLayer
            | RuleContext::AtMedia
            | RuleContext::AtScope
            | RuleContext::AtSupports => {
                !equals_ascii_case_insensitive(name, b"import")
                    && !equals_ascii_case_insensitive(name, b"namespace")
                    && !is_font_feature_value_rule_name(name)
            }
            RuleContext::AtPage => is_margin_rule_name(name),
            RuleContext::AtFontFeatureValues => is_font_feature_value_rule_name(name),
            _ => false,
        }
    }

    fn qualified_rule_is_valid(&self) -> bool {
        matches!(
            self.rule_context.last(),
            None | Some(
                RuleContext::Style
                    | RuleContext::AtContainer
                    | RuleContext::AtLayer
                    | RuleContext::AtMedia
                    | RuleContext::AtScope
                    | RuleContext::AtSupports
                    | RuleContext::AtKeyframes
            )
        )
    }

    /// A diagnostic for each bad string and bad URL token that the parser has read, in the order of the source.
    fn token_diagnostics(&self) -> Vec<FfiSyntaxDiagnostic> {
        let mut lines = LineCursor::default();
        self.stream
            .bad_tokens
            .iter()
            .map(|&(start, end, is_url)| {
                let start = lines.position(self.stream.units, start as usize);
                let end = lines.position(self.stream.units, end as usize);
                FfiSyntaxDiagnostic {
                    code: if is_url {
                        FfiSyntaxDiagnosticCode::BadUrl
                    } else {
                        FfiSyntaxDiagnosticCode::BadString
                    },
                    start_line: start.line,
                    start_column: start.column,
                    end_line: end.line,
                    end_column: end.column,
                    primary_offset: usize::MAX,
                    primary_length: 0,
                    secondary_offset: usize::MAX,
                    secondary_length: 0,
                    prelude_offset: usize::MAX,
                    prelude_length: 0,
                }
            })
            .collect()
    }
}

/// A parser of the structure of a source, which is 8-bit or 16-bit text.
pub(super) enum Parser<'a> {
    Narrow(Core<'a, u8>),
    Wide(Core<'a, u16>),
}

macro_rules! each_width {
    ($self:expr, $core:ident => $body:expr) => {
        match $self {
            Parser::Narrow($core) => $body,
            Parser::Wide($core) => $body,
        }
    };
}

impl<'a> Parser<'a> {
    /// A parser of `source`, which [`source_of`] made, for text in the rules that `rule_context` names.
    pub(super) fn new(source: &'a Arc<SourceStorage>, rule_context: Vec<RuleContext>) -> Self {
        match source.as_ref() {
            SourceStorage::Latin1(units) => Self::Narrow(Core::new(units, source, rule_context)),
            SourceStorage::Utf16(units) => Self::Wide(Core::new(units, source, rule_context)),
        }
    }

    pub(super) fn rule_context(&self) -> &[RuleContext] {
        each_width!(self, core => &core.rule_context)
    }

    /// Parses the source as a style sheet, and gives its rules to `sink`.
    pub(super) fn parse_stylesheet<S: Sink>(&mut self, sink: &mut S) {
        each_width!(self, core => while core.consume_next_stylesheet_rule(sink) {});
    }

    /// Parses the source as one rule, and gives it to `sink`. Returns false if the source is not one rule, and `sink`
    /// can then have a rule that is not the source.
    pub(super) fn parse_only_rule<S: Sink>(&mut self, nested: bool, sink: &mut S) -> bool {
        let nested = if nested { Nested::Yes } else { Nested::No };
        each_width!(self, core => core.consume_only_rule(nested, sink))
    }

    /// Parses the source as the contents of a block, and gives the rules and the declarations in them to `sink`.
    pub(super) fn parse_block_contents<S: Sink>(&mut self, sink: &mut S) {
        each_width!(self, core => core.consume_block_contents(sink));
    }

    /// A diagnostic for each bad string and bad URL token that the parser has read, in the order of the source.
    pub(super) fn token_diagnostics(&self) -> Vec<FfiSyntaxDiagnostic> {
        each_width!(self, core => core.token_diagnostics())
    }
}

#[cfg(test)]
mod tests {
    use super::{LineCursor, component_values_in, source_of};
    use crate::css::css_tokenizer::{ParserTokenKind, SourceStorage, tokenize_for_parser};
    use crate::css::parser::component_value::{ComponentKind, ComponentValue, consume_a_list_of_component_values};
    use std::fmt::Write;

    /// A token, with its string as text: the two tokenizers hold strings in different ways.
    fn token(kind: &ParserTokenKind) -> String {
        let text = |string: &[u16]| String::from_utf16_lossy(string);
        match kind {
            ParserTokenKind::Ident(value) => format!("ident {:?}", text(value)),
            ParserTokenKind::Function(value) => format!("function {:?}", text(value)),
            ParserTokenKind::AtKeyword(value) => format!("at-keyword {:?}", text(value)),
            ParserTokenKind::Hash { value, is_id } => format!("hash {:?} {is_id}", text(value)),
            ParserTokenKind::String(value) => format!("string {:?}", text(value)),
            ParserTokenKind::Url(value) => format!("url {:?}", text(value)),
            ParserTokenKind::Dimension {
                value,
                number_type,
                unit,
            } => format!("dimension {value} {number_type:?} {:?}", text(unit)),
            kind => format!("{kind:?}"),
        }
    }

    /// What component values are, without where they are.
    fn shape(values: &[ComponentValue], output: &mut String) {
        for value in values {
            let source = String::from_utf16_lossy(&value.original_source_text.to_vec());
            match &value.kind {
                ComponentKind::Token(kind) => write!(output, "{} {source:?}, ", token(kind)).unwrap(),
                ComponentKind::Function { name, values } => {
                    write!(output, "function {:?} {source:?} [", String::from_utf16_lossy(name)).unwrap();
                    shape(values, output);
                    write!(output, "] {} {}, ", values.token_count, value.closing_source_length).unwrap();
                }
                ComponentKind::SimpleBlock { opening, values } => {
                    write!(output, "block {} {source:?} [", token(opening)).unwrap();
                    shape(values, output);
                    write!(output, "] {} {}, ", values.token_count, value.closing_source_length).unwrap();
                }
            }
        }
    }

    fn values_of(source: &str) -> String {
        let text = if source.is_ascii() {
            source_of(source.as_bytes().into())
        } else {
            source_of(source.encode_utf16().collect::<Vec<_>>().as_slice().into())
        };
        let values = match text.as_ref() {
            SourceStorage::Latin1(units) => component_values_in(units, &text, 0..units.len()),
            SourceStorage::Utf16(units) => component_values_in(units, &text, 0..units.len()),
        };
        let mut output = String::new();
        shape(&values, &mut output);
        output
    }

    #[test]
    fn makes_the_component_values_of_the_tokenizer() {
        for source in [
            "",
            " ",
            "a",
            " a ",
            "a b",
            "a/**/b",
            "a /**/ b",
            "a/* c *//* d */b",
            "a /* c */ /* d */\n/* e */ b",
            "/**/a/**/",
            "a /* c */",
            "/***/a/**//***/b",
            "/*/ */a",
            "f(a, b) [c d] {e:f} (g)",
            "f( a , g( b ) )",
            "f(a",
            "f( a ",
            "f( g( a /**/ ",
            "@x ( a ",
            "f(",
            "[ ( { a",
            "a ) ] } b",
            "( ] ) [ ) ]",
            "1 +2 -3.5 1e3 10% 12px 1.5em #abc #1bc 'string' \"s\\\"s\" url(a) url( 'a' )",
            "a\\62 c \\31 23 f\\6fo(x)",
            "@media <!-- --> u+123 , ; : !important",
            "\"bad\nstring url(bad url) x",
            "caf\u{e9} \u{1f600}(a) '\u{1f600}'",
            "var(--a, calc(1px + 2 * (3em - 4%)))",
        ] {
            let mut expected = String::new();
            let text: Vec<u16> = source.encode_utf16().collect();
            shape(
                &consume_a_list_of_component_values(tokenize_for_parser(text.as_slice())).unwrap(),
                &mut expected,
            );
            assert_eq!(values_of(source), expected, "{source:?}");
        }
    }

    #[test]
    fn a_comment_that_the_source_ends_in_is_whitespace() {
        assert_eq!(values_of("a /* b"), values_of("a /* b*/").replace("/* b*/", "/* b"));
        assert_eq!(values_of("a/*"), "ident \"a\" \"a\", Whitespace \"/*\", ");
    }

    #[test]
    fn does_not_nest_rules_deeper_than_the_limit() {
        use super::{Declaration, MAXIMUM_RULE_NESTING_DEPTH, Parser, Rows, Rule, Sink};

        #[derive(Default)]
        struct Depths {
            open: usize,
            deepest: usize,
            rules: usize,
            declarations: usize,
        }
        impl Sink for Depths {
            fn begin_rule(&mut self, _: Rule, _: Option<&Rows>) {
                self.rules += usize::from(self.open == 0);
                self.open += 1;
                self.deepest = self.deepest.max(self.open);
            }
            fn end_rule(&mut self) {
                self.open -= 1;
            }
            fn declaration(&mut self, _: Declaration) {
                self.declarations += 1;
            }
            fn end_declarations(&mut self) {}
        }
        // NB: The parser may not use up the stack.
        let count = 100_000;
        let deep = "@media all {".repeat(count) + "a { b: c }" + &"}".repeat(count) + " d { e: f }";
        let source = source_of(deep.as_bytes().into());
        let mut depths = Depths::default();
        Parser::new(&source, Vec::new()).parse_stylesheet(&mut depths);
        assert_eq!(depths.deepest, MAXIMUM_RULE_NESTING_DEPTH + 1);
        assert_eq!((depths.open, depths.rules, depths.declarations), (0, 2, 1));

        let source = source_of(format!("{}}}", "a {".repeat(count)).as_bytes().into());
        let mut depths = Depths::default();
        Parser::new(&source, Vec::new()).parse_stylesheet(&mut depths);
        assert_eq!(depths.deepest, MAXIMUM_RULE_NESTING_DEPTH + 1);
        assert_eq!((depths.open, depths.rules, depths.declarations), (0, 1, 0));
    }

    #[test]
    fn a_value_has_no_whitespace_from_after_it() {
        use super::{Declaration, Parser, Rows, Rule, Sink, source_of};
        use crate::css::parser::syntax_parser::RuleContext;

        #[derive(Default)]
        struct Declarations(Vec<Declaration>);
        impl Sink for Declarations {
            fn begin_rule(&mut self, _: Rule, _: Option<&Rows>) {}
            fn end_rule(&mut self) {}
            fn declaration(&mut self, declaration: Declaration) {
                self.0.push(declaration);
            }
            fn end_declarations(&mut self) {}
        }
        for text in ["a: b ", "a: b /* c */", "a: b\n", "a: b !important ", "a: b ;", "a: b"] {
            let source = source_of(text.as_bytes().into());
            let mut declarations = Declarations::default();
            Parser::new(&source, vec![RuleContext::Style]).parse_block_contents(&mut declarations);
            let values = declarations.0[0].value.component_values();
            assert_eq!(values.len(), 1, "{text:?}");
            assert!(!values[0].is_whitespace(), "{text:?}");
        }
    }

    #[test]
    fn counts_lines_and_code_points() {
        let text: Vec<u16> = "a\n\u{1f600}b\u{e9}\n\nc".encode_utf16().collect();
        let mut lines = LineCursor::default();
        let mut positions = |offset: usize| {
            let position = lines.position(&text, offset);
            (position.line, position.column)
        };
        assert_eq!(positions(0), (0, 0));
        assert_eq!(positions(1), (0, 1));
        assert_eq!(positions(2), (1, 0));
        assert_eq!(positions(4), (1, 1));
        assert_eq!(positions(6), (1, 3));
        assert_eq!(positions(8), (3, 0));
        // NB: A place before the last one is found too.
        assert_eq!(positions(5), (1, 2));
        assert_eq!(positions(9), (3, 1));
    }
}
