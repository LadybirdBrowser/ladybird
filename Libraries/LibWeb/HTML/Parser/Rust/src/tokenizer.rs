/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::VecDeque;

use crate::code_units::CodeUnits;
use crate::code_units::InputRun;
use crate::entities::NamedCharacterReferenceMatcher;
use crate::scan;
use crate::token::DoctypeData;
use crate::token::HtmlName;
use crate::token::Position;
use crate::token::SourceOffset;
use crate::token::Token;
use crate::token::TokenPayload;
use crate::token::TokenType;
use ak::Utf16View;

/// Tokenizer states per the WHATWG HTML spec section 13.2.5.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    Data = 0,
    RCDATA,
    RAWTEXT,
    ScriptData,
    PLAINTEXT,
    TagOpen,
    EndTagOpen,
    TagName,
    RCDATALessThanSign,
    RCDATAEndTagOpen,
    RCDATAEndTagName,
    RAWTEXTLessThanSign,
    RAWTEXTEndTagOpen,
    RAWTEXTEndTagName,
    ScriptDataLessThanSign,
    ScriptDataEndTagOpen,
    ScriptDataEndTagName,
    ScriptDataEscapeStart,
    ScriptDataEscapeStartDash,
    ScriptDataEscaped,
    ScriptDataEscapedDash,
    ScriptDataEscapedDashDash,
    ScriptDataEscapedLessThanSign,
    ScriptDataEscapedEndTagOpen,
    ScriptDataEscapedEndTagName,
    ScriptDataDoubleEscapeStart,
    ScriptDataDoubleEscaped,
    ScriptDataDoubleEscapedDash,
    ScriptDataDoubleEscapedDashDash,
    ScriptDataDoubleEscapedLessThanSign,
    ScriptDataDoubleEscapeEnd,
    BeforeAttributeName,
    AttributeName,
    AfterAttributeName,
    BeforeAttributeValue,
    AttributeValueDoubleQuoted,
    AttributeValueSingleQuoted,
    AttributeValueUnquoted,
    AfterAttributeValueQuoted,
    SelfClosingStartTag,
    BogusComment,
    MarkupDeclarationOpen,
    CommentStart,
    CommentStartDash,
    Comment,
    CommentLessThanSign,
    CommentLessThanSignBang,
    CommentLessThanSignBangDash,
    CommentLessThanSignBangDashDash,
    CommentEndDash,
    CommentEnd,
    CommentEndBang,
    DOCTYPE,
    BeforeDOCTYPEName,
    DOCTYPEName,
    AfterDOCTYPEName,
    AfterDOCTYPEPublicKeyword,
    BeforeDOCTYPEPublicIdentifier,
    DOCTYPEPublicIdentifierDoubleQuoted,
    DOCTYPEPublicIdentifierSingleQuoted,
    AfterDOCTYPEPublicIdentifier,
    BetweenDOCTYPEPublicAndSystemIdentifiers,
    AfterDOCTYPESystemKeyword,
    BeforeDOCTYPESystemIdentifier,
    DOCTYPESystemIdentifierDoubleQuoted,
    DOCTYPESystemIdentifierSingleQuoted,
    AfterDOCTYPESystemIdentifier,
    BogusDOCTYPE,
    CDATASection,
    CDATASectionBracket,
    CDATASectionEnd,
    CharacterReference,
    NamedCharacterReference,
    AmbiguousAmpersand,
    NumericCharacterReference,
    HexadecimalCharacterReferenceStart,
    DecimalCharacterReferenceStart,
    HexadecimalCharacterReference,
    DecimalCharacterReference,
    NumericCharacterReferenceEnd,
}

impl State {
    pub(crate) fn from_ffi(value: u8) -> Option<Self> {
        Some(match value {
            value if value == Self::Data as u8 => Self::Data,
            value if value == Self::RCDATA as u8 => Self::RCDATA,
            value if value == Self::RAWTEXT as u8 => Self::RAWTEXT,
            value if value == Self::ScriptData as u8 => Self::ScriptData,
            value if value == Self::PLAINTEXT as u8 => Self::PLAINTEXT,
            value if value == Self::TagOpen as u8 => Self::TagOpen,
            value if value == Self::EndTagOpen as u8 => Self::EndTagOpen,
            value if value == Self::TagName as u8 => Self::TagName,
            value if value == Self::RCDATALessThanSign as u8 => Self::RCDATALessThanSign,
            value if value == Self::RCDATAEndTagOpen as u8 => Self::RCDATAEndTagOpen,
            value if value == Self::RCDATAEndTagName as u8 => Self::RCDATAEndTagName,
            value if value == Self::RAWTEXTLessThanSign as u8 => Self::RAWTEXTLessThanSign,
            value if value == Self::RAWTEXTEndTagOpen as u8 => Self::RAWTEXTEndTagOpen,
            value if value == Self::RAWTEXTEndTagName as u8 => Self::RAWTEXTEndTagName,
            value if value == Self::ScriptDataLessThanSign as u8 => Self::ScriptDataLessThanSign,
            value if value == Self::ScriptDataEndTagOpen as u8 => Self::ScriptDataEndTagOpen,
            value if value == Self::ScriptDataEndTagName as u8 => Self::ScriptDataEndTagName,
            value if value == Self::ScriptDataEscapeStart as u8 => Self::ScriptDataEscapeStart,
            value if value == Self::ScriptDataEscapeStartDash as u8 => Self::ScriptDataEscapeStartDash,
            value if value == Self::ScriptDataEscaped as u8 => Self::ScriptDataEscaped,
            value if value == Self::ScriptDataEscapedDash as u8 => Self::ScriptDataEscapedDash,
            value if value == Self::ScriptDataEscapedDashDash as u8 => Self::ScriptDataEscapedDashDash,
            value if value == Self::ScriptDataEscapedLessThanSign as u8 => Self::ScriptDataEscapedLessThanSign,
            value if value == Self::ScriptDataEscapedEndTagOpen as u8 => Self::ScriptDataEscapedEndTagOpen,
            value if value == Self::ScriptDataEscapedEndTagName as u8 => Self::ScriptDataEscapedEndTagName,
            value if value == Self::ScriptDataDoubleEscapeStart as u8 => Self::ScriptDataDoubleEscapeStart,
            value if value == Self::ScriptDataDoubleEscaped as u8 => Self::ScriptDataDoubleEscaped,
            value if value == Self::ScriptDataDoubleEscapedDash as u8 => Self::ScriptDataDoubleEscapedDash,
            value if value == Self::ScriptDataDoubleEscapedDashDash as u8 => Self::ScriptDataDoubleEscapedDashDash,
            value if value == Self::ScriptDataDoubleEscapedLessThanSign as u8 => {
                Self::ScriptDataDoubleEscapedLessThanSign
            }
            value if value == Self::ScriptDataDoubleEscapeEnd as u8 => Self::ScriptDataDoubleEscapeEnd,
            value if value == Self::BeforeAttributeName as u8 => Self::BeforeAttributeName,
            value if value == Self::AttributeName as u8 => Self::AttributeName,
            value if value == Self::AfterAttributeName as u8 => Self::AfterAttributeName,
            value if value == Self::BeforeAttributeValue as u8 => Self::BeforeAttributeValue,
            value if value == Self::AttributeValueDoubleQuoted as u8 => Self::AttributeValueDoubleQuoted,
            value if value == Self::AttributeValueSingleQuoted as u8 => Self::AttributeValueSingleQuoted,
            value if value == Self::AttributeValueUnquoted as u8 => Self::AttributeValueUnquoted,
            value if value == Self::AfterAttributeValueQuoted as u8 => Self::AfterAttributeValueQuoted,
            value if value == Self::SelfClosingStartTag as u8 => Self::SelfClosingStartTag,
            value if value == Self::BogusComment as u8 => Self::BogusComment,
            value if value == Self::MarkupDeclarationOpen as u8 => Self::MarkupDeclarationOpen,
            value if value == Self::CommentStart as u8 => Self::CommentStart,
            value if value == Self::CommentStartDash as u8 => Self::CommentStartDash,
            value if value == Self::Comment as u8 => Self::Comment,
            value if value == Self::CommentLessThanSign as u8 => Self::CommentLessThanSign,
            value if value == Self::CommentLessThanSignBang as u8 => Self::CommentLessThanSignBang,
            value if value == Self::CommentLessThanSignBangDash as u8 => Self::CommentLessThanSignBangDash,
            value if value == Self::CommentLessThanSignBangDashDash as u8 => Self::CommentLessThanSignBangDashDash,
            value if value == Self::CommentEndDash as u8 => Self::CommentEndDash,
            value if value == Self::CommentEnd as u8 => Self::CommentEnd,
            value if value == Self::CommentEndBang as u8 => Self::CommentEndBang,
            value if value == Self::DOCTYPE as u8 => Self::DOCTYPE,
            value if value == Self::BeforeDOCTYPEName as u8 => Self::BeforeDOCTYPEName,
            value if value == Self::DOCTYPEName as u8 => Self::DOCTYPEName,
            value if value == Self::AfterDOCTYPEName as u8 => Self::AfterDOCTYPEName,
            value if value == Self::AfterDOCTYPEPublicKeyword as u8 => Self::AfterDOCTYPEPublicKeyword,
            value if value == Self::BeforeDOCTYPEPublicIdentifier as u8 => Self::BeforeDOCTYPEPublicIdentifier,
            value if value == Self::DOCTYPEPublicIdentifierDoubleQuoted as u8 => {
                Self::DOCTYPEPublicIdentifierDoubleQuoted
            }
            value if value == Self::DOCTYPEPublicIdentifierSingleQuoted as u8 => {
                Self::DOCTYPEPublicIdentifierSingleQuoted
            }
            value if value == Self::AfterDOCTYPEPublicIdentifier as u8 => Self::AfterDOCTYPEPublicIdentifier,
            value if value == Self::BetweenDOCTYPEPublicAndSystemIdentifiers as u8 => {
                Self::BetweenDOCTYPEPublicAndSystemIdentifiers
            }
            value if value == Self::AfterDOCTYPESystemKeyword as u8 => Self::AfterDOCTYPESystemKeyword,
            value if value == Self::BeforeDOCTYPESystemIdentifier as u8 => Self::BeforeDOCTYPESystemIdentifier,
            value if value == Self::DOCTYPESystemIdentifierDoubleQuoted as u8 => {
                Self::DOCTYPESystemIdentifierDoubleQuoted
            }
            value if value == Self::DOCTYPESystemIdentifierSingleQuoted as u8 => {
                Self::DOCTYPESystemIdentifierSingleQuoted
            }
            value if value == Self::AfterDOCTYPESystemIdentifier as u8 => Self::AfterDOCTYPESystemIdentifier,
            value if value == Self::BogusDOCTYPE as u8 => Self::BogusDOCTYPE,
            value if value == Self::CDATASection as u8 => Self::CDATASection,
            value if value == Self::CDATASectionBracket as u8 => Self::CDATASectionBracket,
            value if value == Self::CDATASectionEnd as u8 => Self::CDATASectionEnd,
            value if value == Self::CharacterReference as u8 => Self::CharacterReference,
            value if value == Self::NamedCharacterReference as u8 => Self::NamedCharacterReference,
            value if value == Self::AmbiguousAmpersand as u8 => Self::AmbiguousAmpersand,
            value if value == Self::NumericCharacterReference as u8 => Self::NumericCharacterReference,
            value if value == Self::HexadecimalCharacterReferenceStart as u8 => {
                Self::HexadecimalCharacterReferenceStart
            }
            value if value == Self::DecimalCharacterReferenceStart as u8 => Self::DecimalCharacterReferenceStart,
            value if value == Self::HexadecimalCharacterReference as u8 => Self::HexadecimalCharacterReference,
            value if value == Self::DecimalCharacterReference as u8 => Self::DecimalCharacterReference,
            value if value == Self::NumericCharacterReferenceEnd as u8 => Self::NumericCharacterReferenceEnd,
            _ => return None,
        })
    }
}

/// The lines and columns of offsets into the input, which are only worked out when something asks for them.
#[derive(Default)]
struct SourcePositions {
    /// The position of the start of the input buffer, which stops being the start of the input once consumed input
    /// is discarded.
    start: Position,
    /// The last offset a position was worked out for, and that position. Offsets are mostly asked about in
    /// increasing order, so the next one is usually found by going on from here.
    known_offset: usize,
    known_position: Position,
}

/// The HTML tokenizer state machine.
///
/// Implements the WHATWG HTML tokenizer specification (section 13.2.5).
pub struct HtmlTokenizer {
    pub state: State,
    return_state: State,
    pub input: CodeUnits,
    pub current_offset: usize,
    prev_offset: usize,
    current_token: Token,
    current_builder: CodeUnits,
    pub queued_tokens: VecDeque<Token>,
    temporary_buffer: Vec<u32>,
    character_reference_code: u32,
    last_emitted_start_tag_name: Option<HtmlName>,
    source_positions: SourcePositions,
    has_emitted_eof: bool,
    aborted: bool,
    insertion_point: Option<usize>,
    old_insertion_points: Vec<Option<usize>>,
    input_stream_closed: bool,
    stop_at_insertion_point: bool,
    cdata_allowed: bool,
    entity_matcher: NamedCharacterReferenceMatcher,
    // Where the input that a speculative HTML parser's preload scanner has been given ends, while it's still a place
    // the scanner can go on from.
    speculative_scan_end: Option<usize>,
}

#[inline]
fn is_ascii_alpha(c: u32) -> bool {
    matches!(c, 0x41..=0x5A | 0x61..=0x7A)
}

#[inline]
fn is_ascii_upper_alpha(c: u32) -> bool {
    matches!(c, 0x41..=0x5A)
}

#[inline]
fn is_ascii_digit(c: u32) -> bool {
    matches!(c, 0x30..=0x39)
}

#[inline]
fn is_ascii_hex_digit(c: u32) -> bool {
    matches!(c, 0x30..=0x39 | 0x41..=0x46 | 0x61..=0x66)
}

#[inline]
fn is_ascii_alphanumeric(c: u32) -> bool {
    is_ascii_alpha(c) || is_ascii_digit(c)
}

#[inline]
fn to_ascii_lowercase(c: u32) -> u32 {
    if is_ascii_upper_alpha(c) { c + 0x20 } else { c }
}

/// Reads the tag whose "<" ends right before `offset` into `token`, and returns the offset after its ">", if the tag
/// is as plain as `HtmlTokenizer::try_scan_tag` describes and all of it is in `input`.
fn scan_tag<T: scan::CodeUnit>(input: &[T], mut offset: usize, token: &mut Token) -> Option<usize> {
    // NB: U+0000 NULL, which nothing here takes, stands in for the end of the input.
    let at = |offset: usize| -> u16 { input.get(offset).map_or(0, |&code_unit| code_unit.into()) };
    let is_tag_whitespace = |code_unit: u16| matches!(code_unit, 0x09 | 0x0A | 0x0C | 0x20);
    let is_tag_end = |code_unit: u16| matches!(code_unit, 0x09 | 0x0A | 0x0C | 0x20 | 0x2F | 0x3E);

    let is_end_tag = at(offset) == 0x2F;
    if is_end_tag {
        offset += 1;
    }

    let name_start = offset;
    if !matches!(at(offset), 0x61..=0x7A) {
        return None;
    }
    offset += 1;
    while matches!(at(offset), 0x61..=0x7A | 0x30..=0x39 | 0x2D) {
        offset += 1;
    }
    if !is_tag_end(at(offset)) {
        return None;
    }
    let mut name_buffer = [0; 64];
    let name = Utf16View::Latin1(scan::ascii_name_bytes(&input[name_start..offset], &mut name_buffer)?);
    *token = Token {
        token_type: if is_end_tag {
            TokenType::EndTag
        } else {
            TokenType::StartTag
        },
        code_point: 0,
        payload: TokenPayload::Tag {
            tag_name: HtmlName::from_code_units(name),
            tag_id: crate::token::TagId::for_code_units(name),
            self_closing: false,
            had_duplicate_attribute: false,
            attributes: crate::token::AttributeList::new(),
        },
        start_offset: SourceOffset::new(name_start),
        end_offset: SourceOffset::NONE,
    };
    let TokenPayload::Tag {
        attributes,
        self_closing,
        ..
    } = &mut token.payload
    else {
        unreachable!();
    };

    loop {
        let after_whitespace = offset;
        while is_tag_whitespace(at(offset)) {
            offset += 1;
        }
        match at(offset) {
            0x3E => return Some(offset + 1),
            0x2F if !is_end_tag && at(offset + 1) == 0x3E => {
                *self_closing = true;
                return Some(offset + 2);
            }
            _ => {}
        }
        // NB: An attribute comes after whitespace, and an end tag with attributes is a parse error.
        if is_end_tag || offset == after_whitespace {
            return None;
        }

        let name_start = offset;
        while matches!(at(offset), 0x61..=0x7A | 0x30..=0x39 | 0x2D | 0x2E | 0x3A | 0x5F) {
            offset += 1;
        }
        if offset == name_start || !(is_tag_end(at(offset)) || at(offset) == 0x3D) {
            return None;
        }
        let name = Utf16View::Latin1(scan::ascii_name_bytes(&input[name_start..offset], &mut name_buffer)?);
        attributes.start_new_attribute();
        let attribute = attributes.last_mut().unwrap();
        attribute.local_name = HtmlName::from_code_units(name);
        attribute.name_start_offset = SourceOffset::new(name_start);
        attribute.name_end_offset = SourceOffset::new(offset);

        let mut after_name = offset;
        while is_tag_whitespace(at(after_name)) {
            after_name += 1;
        }
        if at(after_name) != 0x3D {
            // NB: The attribute has no value, and the whitespace after it is that before what comes next.
            continue;
        }
        offset = after_name + 1;
        while is_tag_whitespace(at(offset)) {
            offset += 1;
        }

        let quote = at(offset);
        attribute.value_start_offset = SourceOffset::new(offset);
        if quote == 0x22 || quote == 0x27 {
            offset += 1;
            let length = scan::attribute_value_run(&input[offset..], quote as u8);
            if at(offset + length) != quote {
                return None;
            }
            attribute.value.push_slice(T::view(&input[offset..offset + length]));
            offset += length + 1;
            if !is_tag_end(at(offset)) {
                return None;
            }
        } else {
            let value_start = offset;
            loop {
                match at(offset) {
                    0x09 | 0x0A | 0x0C | 0x20 | 0x3E => break,
                    0x00 | 0x0D | 0x22 | 0x26 | 0x27 | 0x3C | 0x3D | 0x60 | 0xD800..=0xDFFF => return None,
                    _ => offset += 1,
                }
            }
            if offset == value_start {
                return None;
            }
            attribute.value.push_slice(T::view(&input[value_start..offset]));
        }
        attribute.value_end_offset = SourceOffset::new(offset);
    }
}

#[inline]
fn is_whitespace(c: u32) -> bool {
    matches!(c, 0x09 | 0x0A | 0x0C | 0x20)
}

/// Numeric character reference replacement table.
/// https://html.spec.whatwg.org/multipage/parsing.html#table-charref-overrides
fn numeric_char_ref_replacement(code: u32) -> Option<u32> {
    match code {
        0x80 => Some(0x20AC),
        0x82 => Some(0x201A),
        0x83 => Some(0x0192),
        0x84 => Some(0x201E),
        0x85 => Some(0x2026),
        0x86 => Some(0x2020),
        0x87 => Some(0x2021),
        0x88 => Some(0x02C6),
        0x89 => Some(0x2030),
        0x8A => Some(0x0160),
        0x8B => Some(0x2039),
        0x8C => Some(0x0152),
        0x8E => Some(0x017D),
        0x91 => Some(0x2018),
        0x92 => Some(0x2019),
        0x93 => Some(0x201C),
        0x94 => Some(0x201D),
        0x95 => Some(0x2022),
        0x96 => Some(0x2013),
        0x97 => Some(0x2014),
        0x98 => Some(0x02DC),
        0x99 => Some(0x2122),
        0x9A => Some(0x0161),
        0x9B => Some(0x203A),
        0x9C => Some(0x0153),
        0x9E => Some(0x017E),
        0x9F => Some(0x0178),
        _ => None,
    }
}

/// Check if a code point is a Unicode surrogate.
fn is_surrogate(c: u32) -> bool {
    (0xD800..=0xDFFF).contains(&c)
}

#[inline]
fn is_high_surrogate(code_unit: u16) -> bool {
    (0xD800..=0xDBFF).contains(&code_unit)
}

/// Check if a code point is a Unicode noncharacter.
fn is_noncharacter(c: u32) -> bool {
    (0xFDD0..=0xFDEF).contains(&c) || matches!(c & 0xFFFF, 0xFFFE | 0xFFFF)
}

#[inline]
fn is_ascii_lower_alpha(c: u32) -> bool {
    matches!(c, 0x61..=0x7A)
}

/// What ends a run of text that a state emits as it is, besides U+0000 NULL, U+000D CR and lone surrogates.
#[derive(Clone, Copy, Eq, PartialEq)]
enum TextRunStops {
    /// U+003C (<).
    Markup,
    /// U+003C (<) and U+0026 (&).
    MarkupAndCharacterReferences,
}

impl HtmlTokenizer {
    /// Create a new tokenizer for the given input.
    pub fn new(input: impl Into<CodeUnits>) -> Self {
        HtmlTokenizer {
            state: State::Data,
            return_state: State::Data,
            input: input.into(),
            current_offset: 0,
            prev_offset: 0,
            current_token: Token::default(),
            current_builder: CodeUnits::new(),
            queued_tokens: VecDeque::new(),
            temporary_buffer: Vec::new(),
            character_reference_code: 0,
            last_emitted_start_tag_name: None,
            source_positions: SourcePositions::default(),
            has_emitted_eof: false,
            aborted: false,
            insertion_point: None,
            old_insertion_points: Vec::new(),
            input_stream_closed: true,
            stop_at_insertion_point: false,
            cdata_allowed: false,
            entity_matcher: NamedCharacterReferenceMatcher::new(),
            speculative_scan_end: None,
        }
    }

    /// Set the tokenizer state.
    pub fn switch_to(&mut self, state: State) {
        self.state = state;
    }

    // -- Insertion point management --

    pub fn store_insertion_point(&mut self) {
        self.old_insertion_points.push(self.insertion_point);
    }

    pub fn restore_insertion_point(&mut self) {
        self.insertion_point = self.old_insertion_points.pop().unwrap_or(None);
    }

    pub fn update_insertion_point(&mut self) {
        self.insertion_point = Some(self.current_offset);
    }

    pub fn undefine_insertion_point(&mut self) {
        self.insertion_point = None;
    }

    /// Undefine the insertion point along with every old insertion point that is waiting to be restored.
    pub fn discard_insertion_points(&mut self) {
        self.insertion_point = None;
        self.old_insertion_points.fill(None);
    }

    pub fn is_insertion_point_defined(&self) -> bool {
        self.insertion_point.is_some()
    }

    pub fn is_insertion_point_reached(&self) -> bool {
        self.insertion_point
            .is_some_and(|insertion_point| self.current_offset >= insertion_point)
    }

    pub fn append_input(&mut self, code_units: Utf16View<'_>) {
        self.input.append(code_units);
    }

    pub fn insert_input_at_insertion_point(&mut self, code_units: Utf16View<'_>) {
        if let Some(ip) = self.insertion_point {
            let ip = ip.min(self.input.len());
            self.input.insert(ip, code_units);
            self.insertion_point = Some(ip + code_units.len());
            self.forget_speculative_scan_end_before(ip);
            // NB: A line feed inserted right after a carriage return becomes part of its line end.
            if ip <= self.source_positions.known_offset {
                self.source_positions.known_offset = 0;
                self.source_positions.known_position = self.source_positions.start;
            }
            for old_insertion_point in &mut self.old_insertion_points {
                if let Some(old_ip) = old_insertion_point
                    && ip <= *old_ip
                {
                    *old_ip += code_units.len();
                }
            }
        }
    }

    pub fn unparsed_input(&self) -> Utf16View<'_> {
        self.input.slice_from(self.current_offset)
    }

    /// The input a speculative HTML parser's preload scanner hasn't been given yet, and whether the scanner can go on
    /// from where it stopped. It can't once this tokenizer has got past that point, or input was inserted before it;
    /// the input then runs from the current position, and the scanner has to start over there.
    pub fn input_for_speculative_scan(&mut self) -> (Utf16View<'_>, bool) {
        let (start, continues) = match self.speculative_scan_end {
            Some(end) if end >= self.current_offset => (end, true),
            _ => (self.current_offset, false),
        };
        self.speculative_scan_end = Some(self.input.len());
        (self.input.slice_from(start), continues)
    }

    /// Makes the next speculative scan start over at the current position.
    pub fn forget_speculative_scan_end(&mut self) {
        self.speculative_scan_end = None;
    }

    fn forget_speculative_scan_end_before(&mut self, offset: usize) {
        if self.speculative_scan_end.is_some_and(|end| offset < end) {
            self.speculative_scan_end = None;
        }
    }

    fn can_discard_consumed_input(&self) -> bool {
        if self.current_offset == 0 || self.current_offset != self.input.len() {
            return false;
        }

        // Tokens say where they are with offsets into the input.
        if !self.queued_tokens.is_empty() {
            return false;
        }

        // A token that isn't finished has such offsets too, and the states that lead up to one work out where it
        // starts from where they are, so the input stays until the tokenizer is back between tokens.
        let is_between_tokens = matches!(
            self.state,
            State::Data
                | State::RCDATA
                | State::RAWTEXT
                | State::ScriptData
                | State::PLAINTEXT
                | State::ScriptDataEscaped
                | State::ScriptDataDoubleEscaped
                | State::CDATASection
        );
        if !is_between_tokens {
            return false;
        }

        if self.insertion_point.is_some_and(|insertion_point| insertion_point != 0)
            || !self.old_insertion_points.is_empty()
        {
            return false;
        }

        true
    }

    pub fn parser_did_run(&mut self) {
        if !self.can_discard_consumed_input() {
            return;
        }

        // All of the input is consumed, so a preload scanner that has been given all of it can still go on from there.
        self.speculative_scan_end = self
            .speculative_scan_end
            .filter(|end| *end == self.input.len())
            .map(|_| 0);
        let end_position = self.position_at(SourceOffset::new(self.input.len()));
        self.source_positions = SourcePositions {
            start: end_position,
            known_offset: 0,
            known_position: end_position,
        };
        self.input = CodeUnits::default();
        self.current_offset = 0;
        self.prev_offset = 0;
    }

    pub fn insert_eof(&mut self) {
        self.input_stream_closed = true;
    }

    pub fn set_input_stream_closed(&mut self, closed: bool) {
        self.input_stream_closed = closed;
    }

    pub fn abort(&mut self) {
        self.aborted = true;
    }

    // -- Input helpers --

    #[inline]
    fn peek_code_point(&self, offset: isize) -> Option<u32> {
        let idx = self.current_offset as isize + offset;
        if idx < 0 || idx as usize >= self.input.len() {
            return None;
        }
        if self.stop_at_insertion_point
            && let Some(ip) = self.insertion_point
            && idx as usize >= ip
        {
            return None;
        }
        self.code_point_at(idx as usize).map(|(code_point, _)| code_point)
    }

    #[inline(always)]
    fn code_point_at(&self, offset: usize) -> Option<(u32, usize)> {
        let limit = self.fast_scan_limit();
        let first = self.input.get(offset)?;
        if is_high_surrogate(first) {
            if offset + 1 < limit {
                let second = self.input.code_unit_at(offset + 1);
                if (0xDC00..=0xDFFF).contains(&second) {
                    let code_point = 0x10000 + ((u32::from(first) - 0xD800) << 10) + (u32::from(second) - 0xDC00);
                    return Some((code_point, 2));
                }
            }
            return Some((0xFFFD, 1));
        }
        if (0xDC00..=0xDFFF).contains(&first) {
            return Some((0xFFFD, 1));
        }
        Some((u32::from(first), 1))
    }

    #[inline(always)]
    fn advance(&mut self, code_unit_count: usize) {
        self.prev_offset = self.current_offset;
        self.current_offset += code_unit_count;
    }

    #[inline(always)]
    fn skip_ascii(&mut self, count: usize) {
        debug_assert!(
            (self.current_offset..self.current_offset + count).all(|offset| self.input.code_unit_at(offset) < 0x80)
        );
        self.current_offset += count;
        self.prev_offset = self.current_offset - 1;
    }

    #[inline(always)]
    fn next_code_point(&mut self) -> Option<u32> {
        if self.current_offset >= self.input.len() {
            self.prev_offset = self.current_offset;
            return None;
        }
        let (cp, code_unit_count) = self.code_point_at(self.current_offset)?;
        if cp != 0x0D {
            self.advance(code_unit_count);
            return Some(cp);
        }
        // Slow path: CR normalization
        let next = self.peek_code_point(1).unwrap_or(0);
        if next == 0x0A {
            self.advance(2);
        } else {
            self.advance(1);
        }
        Some(0x0A)
    }

    /// Where the code point that was consumed last starts.
    #[inline(always)]
    fn offset_before_last_code_point(&self) -> SourceOffset {
        let offset = self.current_offset;
        if offset < 2 {
            return SourceOffset::new(0);
        }
        let last = self.input.code_unit_at(offset - 1);
        let second_to_last = self.input.code_unit_at(offset - 2);
        // NB: A CR and the LF after it are consumed as one line feed, and a surrogate pair is one code point.
        let is_one_code_point = (second_to_last == 0x0D && last == 0x0A)
            || (is_high_surrogate(second_to_last) && (0xDC00..=0xDFFF).contains(&last));
        SourceOffset::new(offset - if is_one_code_point { 2 } else { 1 })
    }

    #[inline(always)]
    fn offset(&self) -> SourceOffset {
        SourceOffset::new(self.current_offset)
    }

    /// The line and column of an offset into the input, which must not be in the middle of a code point.
    pub fn position_at(&mut self, offset: SourceOffset) -> Position {
        let Some(offset) = offset.get() else {
            return Position::default();
        };
        let offset = offset.min(self.input.len());
        let positions = &mut self.source_positions;
        if offset < positions.known_offset {
            positions.known_offset = 0;
            positions.known_position = positions.start;
        }
        let range = positions.known_offset..offset;
        positions.known_position = match &self.input {
            CodeUnits::Latin1(code_units) => scan::position_after(positions.known_position, &code_units[range]),
            CodeUnits::Utf16(code_units) => scan::position_after(positions.known_position, &code_units[range]),
        };
        positions.known_offset = offset;
        positions.known_position
    }

    /// Fast-path character emission from the Data state: if the tokenizer
    /// is positioned at a plain-text code point (not `<`, `&`, NUL, CR,
    /// or any non-ASCII character) and the queue is empty, advance one
    /// code point and return it, skipping the
    /// full `next_token` state-machine dispatch and Token construction.
    /// Returns None if the slow path is required.
    #[inline(always)]
    pub fn try_fast_data_char(&mut self) -> Option<u32> {
        if self.aborted {
            return None;
        }
        if self.state as u8 != State::Data as u8 {
            return None;
        }
        if !self.queued_tokens.is_empty() {
            return None;
        }
        if self.current_offset >= self.input.len() {
            return None;
        }
        let cp = u32::from(self.input.code_unit_at(self.current_offset));
        if cp == 0x3C || cp == 0x26 || cp == 0x00 || cp == 0x0D || cp >= 0x80 {
            return None;
        }
        self.prev_offset = self.current_offset;
        self.current_offset += 1;
        Some(cp)
    }

    /// Consume the run of input at the current position that the Data state would emit unchanged, one character
    /// token per code point, and return where in the input it is. The caller can take the text straight from the
    /// input instead of having a `Token` constructed and dispatched for each code point.
    #[inline(always)]
    pub fn try_fast_data_run(&mut self) -> Option<InputRun> {
        if !self.has_fast_data_run() {
            return None;
        }
        Some(self.take_text_run(TextRunStops::MarkupAndCharacterReferences))
    }

    /// Whether `try_fast_data_run()` would return a run.
    #[inline(always)]
    pub fn has_fast_data_run(&self) -> bool {
        self.state == State::Data && self.has_text_run(TextRunStops::MarkupAndCharacterReferences)
    }

    /// Like `try_fast_data_run()`, for the text of an element whose content isn't markup: what the RCDATA, RAWTEXT
    /// and script data states would emit unchanged.
    #[inline(always)]
    pub fn try_fast_element_text_run(&mut self) -> Option<InputRun> {
        let stops = self.element_text_run_stops()?;
        if !self.has_text_run(stops) {
            return None;
        }
        Some(self.take_text_run(stops))
    }

    #[inline(always)]
    fn element_text_run_stops(&self) -> Option<TextRunStops> {
        match self.state {
            State::RCDATA => Some(TextRunStops::MarkupAndCharacterReferences),
            State::RAWTEXT | State::ScriptData => Some(TextRunStops::Markup),
            _ => None,
        }
    }

    #[inline(always)]
    fn has_text_run(&self, stops: TextRunStops) -> bool {
        if self.aborted || !self.queued_tokens.is_empty() {
            return false;
        }
        if self.current_offset >= self.fast_scan_limit() {
            return false;
        }
        // NB: A surrogate pair can be inside a run, but a run that starts with one is left to the slow path, which
        //     knows what to do when the surrogate turns out not to have its other half.
        match self.input.code_unit_at(self.current_offset) {
            0x3C | 0x00 | 0x0D | 0xD800..=0xDFFF => false,
            0x26 => stops == TextRunStops::Markup,
            _ => true,
        }
    }

    fn take_text_run(&mut self, stops: TextRunStops) -> InputRun {
        let start_offset = self.current_offset;
        let limit = self.fast_scan_limit();
        let scan::Run { length, is_ascii } = match (&self.input, stops) {
            (CodeUnits::Latin1(code_units), TextRunStops::MarkupAndCharacterReferences) => {
                scan::data_run(&code_units[start_offset..limit])
            }
            (CodeUnits::Utf16(code_units), TextRunStops::MarkupAndCharacterReferences) => {
                scan::data_run(&code_units[start_offset..limit])
            }
            (CodeUnits::Latin1(code_units), TextRunStops::Markup) => {
                scan::raw_text_run(&code_units[start_offset..limit])
            }
            (CodeUnits::Utf16(code_units), TextRunStops::Markup) => {
                scan::raw_text_run(&code_units[start_offset..limit])
            }
        };
        debug_assert!(length > 0);
        let end_offset = start_offset + length;
        let ends_with_surrogate_pair = (0xDC00..=0xDFFF).contains(&self.input.code_unit_at(end_offset - 1));
        self.prev_offset = end_offset - if ends_with_surrogate_pair { 2 } else { 1 };
        self.current_offset = end_offset;
        InputRun {
            range: start_offset..end_offset,
            is_ascii,
        }
    }

    /// Appends the code units that come next to the attribute value being read, as far as a value in quotes takes
    /// them as they are. This is what the attribute value states do with them one at a time.
    fn append_attribute_value_run(&mut self, quote: u8) {
        let start_offset = self.current_offset;
        let limit = self.fast_scan_limit();
        if start_offset >= limit {
            return;
        }
        let length = match &self.input {
            CodeUnits::Latin1(code_units) => scan::attribute_value_run(&code_units[start_offset..limit], quote),
            CodeUnits::Utf16(code_units) => scan::attribute_value_run(&code_units[start_offset..limit], quote),
        };
        if length == 0 {
            return;
        }
        let end_offset = start_offset + length;
        self.current_builder
            .push_slice(self.input.slice(start_offset..end_offset));
        self.prev_offset = end_offset - 1;
        self.current_offset = end_offset;
    }

    fn restore_to(&mut self, offset: usize) {
        self.current_offset = offset;
    }

    /// OPTIMIZATION: Most tags are written the same plain way: a lowercase name, attributes with lowercase names and
    ///               values that hold no character references, and nothing out of place. Such a tag is read here in
    ///               one go, right after its "<" in the data state, into the token that the tag open state and the
    ///               states after it would have made of it one code point at a time. Any other tag is left to them,
    ///               with nothing consumed.
    fn try_scan_tag(&mut self) -> bool {
        let limit = self.fast_scan_limit();
        let offset = self.current_offset;
        let end = match &self.input {
            CodeUnits::Latin1(code_units) => scan_tag(&code_units[..limit], offset, &mut self.current_token),
            CodeUnits::Utf16(code_units) => scan_tag(&code_units[..limit], offset, &mut self.current_token),
        };
        let Some(end) = end else {
            self.current_token = Token::default();
            return false;
        };
        self.prev_offset = end - 1;
        self.current_offset = end;
        true
    }

    fn consume_current_builder(&mut self) -> CodeUnits {
        self.current_builder.take()
    }

    /// Commit `current_builder` into `current_token.tag_name`.
    #[inline]
    fn commit_current_builder_as_tag_name(&mut self) {
        self.current_token.set_tag_name(self.current_builder.as_slice());
        self.current_builder.clear();
    }

    fn create_new_token(&mut self, token_type: TokenType) {
        let payload = match token_type {
            TokenType::StartTag | TokenType::EndTag => TokenPayload::Tag {
                tag_name: Default::default(),
                tag_id: Default::default(),
                self_closing: false,
                had_duplicate_attribute: false,
                attributes: crate::token::AttributeList::new(),
            },
            TokenType::Comment => TokenPayload::Comment(CodeUnits::new()),
            TokenType::Doctype => TokenPayload::Doctype(Box::default()),
            _ => TokenPayload::None,
        };
        let start_offset = match token_type {
            TokenType::StartTag | TokenType::EndTag => self.offset_before_last_code_point(),
            _ => self.offset(),
        };
        self.current_token = Token {
            token_type,
            code_point: 0,
            payload,
            start_offset,
            end_offset: SourceOffset::NONE,
        };
    }

    fn current_end_tag_token_is_appropriate(&self) -> bool {
        if self.current_token.token_type != TokenType::EndTag {
            return false;
        }
        match &self.last_emitted_start_tag_name {
            Some(name) => self.current_token.tag_name() == name,
            None => false,
        }
    }

    fn consumed_as_part_of_an_attribute(&self) -> bool {
        matches!(
            self.return_state,
            State::AttributeValueDoubleQuoted | State::AttributeValueSingleQuoted | State::AttributeValueUnquoted
        )
    }

    fn will_emit(&mut self, token_idx: usize) {
        // token_idx: 0 = current_token, 1+ = queued_tokens index
        // For simplicity, we handle setting the end offset here.
        if token_idx == 0 {
            if self.current_token.token_type == TokenType::StartTag
                || self.current_token.token_type == TokenType::EndTag
            {
                self.current_token.normalize_attributes();
            }
            if self.current_token.token_type == TokenType::StartTag {
                self.last_emitted_start_tag_name = Some(self.current_token.tag_name().clone());
            }
            let is_start_or_end_tag = self.current_token.token_type == TokenType::StartTag
                || self.current_token.token_type == TokenType::EndTag;
            self.current_token.end_offset = if is_start_or_end_tag {
                self.offset_before_last_code_point()
            } else {
                self.offset()
            };
        }
    }

    fn emit_current_token(&mut self) {
        self.will_emit(0);
        let token = std::mem::take(&mut self.current_token);
        self.queued_tokens.push_back(token);
    }

    /// Moves the token at the front of the queue to `out`, if there is one.
    #[inline]
    fn take_queued_token(&mut self, out: &mut Token) -> bool {
        match self.queued_tokens.pop_front() {
            Some(token) => {
                *out = token;
                true
            }
            None => false,
        }
    }

    /// Fast-path version of `emit_current_token(); self.take_queued_token(out)`.
    /// When the queue is empty we skip the push/pop round trip entirely,
    /// which removes two VecDeque ops and a Token move per emitted token
    /// for the overwhelmingly common single-emit case.
    #[inline]
    fn emit_current_token_direct(&mut self, out: &mut Token) -> bool {
        self.will_emit(0);
        let token = std::mem::take(&mut self.current_token);
        if self.queued_tokens.is_empty() {
            *out = token;
            return true;
        }
        self.queued_tokens.push_back(token);
        self.take_queued_token(out)
    }

    #[inline(always)]
    fn return_character_token(&mut self, out: &mut Token, code_point: u32) -> bool {
        let mut token = Token::new_character(code_point);
        token.start_offset = self.offset();
        if self.queued_tokens.is_empty() {
            *out = token;
            return true;
        }
        self.queued_tokens.push_back(token);
        self.take_queued_token(out)
    }

    fn emit_eof(&mut self) {
        if self.has_emitted_eof {
            return;
        }
        self.has_emitted_eof = true;
        self.create_new_token(TokenType::EndOfFile);
        self.emit_current_token();
    }

    fn emit_current_token_followed_by_eof(&mut self) {
        self.emit_current_token();
        self.has_emitted_eof = true;
        self.create_new_token(TokenType::EndOfFile);
        self.emit_current_token();
    }

    fn flush_codepoints_consumed_as_character_reference(&mut self) {
        for i in 0..self.temporary_buffer.len() {
            let code_point = self.temporary_buffer[i];
            if self.consumed_as_part_of_an_attribute() {
                self.current_builder.push_code_point(code_point);
            } else {
                let mut token = Token::new_character(code_point);
                token.start_offset = self.offset();
                self.queued_tokens.push_back(token);
            }
        }
    }

    fn can_run_out_at(&self, idx: usize) -> bool {
        if self.stop_at_insertion_point
            && let Some(ip) = self.insertion_point
            && idx >= ip
        {
            return true;
        }
        !self.input_stream_closed && idx >= self.input.len()
    }

    #[inline]
    fn fast_scan_limit(&self) -> usize {
        if self.stop_at_insertion_point
            && let Some(ip) = self.insertion_point
        {
            return ip.min(self.input.len());
        }
        self.input.len()
    }

    fn should_pause_before_next_input_character(&self) -> bool {
        if self.stop_at_insertion_point
            && let Some(ip) = self.insertion_point
        {
            if self.current_offset >= ip {
                return true;
            }

            if self.current_offset < self.input.len()
                && (self.input.code_unit_at(self.current_offset) == 0x0D
                    || is_high_surrogate(self.input.code_unit_at(self.current_offset)))
                && self.current_offset + 1 == ip
            {
                return true;
            }
        }

        if self.input_stream_closed {
            return false;
        }

        if self.current_offset >= self.input.len() {
            return true;
        }

        (self.input.code_unit_at(self.current_offset) == 0x0D
            || is_high_surrogate(self.input.code_unit_at(self.current_offset)))
            && self.current_offset + 1 >= self.input.len()
    }

    /// Case-insensitive match of upcoming input against a string.
    /// Returns Some(true) if matched and consumed, Some(false) if no match,
    /// None if ran out of characters at the insertion point.
    fn consume_next_if_match(&mut self, s: &str) -> Option<bool> {
        for (i, b) in s.bytes().enumerate() {
            match self.peek_code_point(i as isize) {
                None => {
                    if self.can_run_out_at(self.current_offset + i) {
                        return None;
                    }
                    return Some(false);
                }
                Some(cp) => {
                    if to_ascii_lowercase(cp) != to_ascii_lowercase(b as u32) {
                        return Some(false);
                    }
                }
            }
        }
        // All matched, consume them
        self.skip_ascii(s.len());
        Some(true)
    }

    /// Case-sensitive match of upcoming input against a string.
    fn consume_next_if_match_exact(&mut self, s: &str) -> Option<bool> {
        for (i, b) in s.bytes().enumerate() {
            match self.peek_code_point(i as isize) {
                None => {
                    if self.can_run_out_at(self.current_offset + i) {
                        return None;
                    }
                    return Some(false);
                }
                Some(cp) => {
                    if cp != b as u32 {
                        return Some(false);
                    }
                }
            }
        }
        self.skip_ascii(s.len());
        Some(true)
    }

    /// Get the next token from the tokenizer.
    pub fn next_token(&mut self, stop_at_insertion_point: bool, cdata_allowed: bool) -> Option<Token> {
        let mut token = Token::default();
        self.next_token_into(&mut token, stop_at_insertion_point, cdata_allowed)
            .then_some(token)
    }

    /// Puts the next token in `out`, and returns whether there was one. What `out` held before is dropped only then.
    pub fn next_token_into(&mut self, out: &mut Token, stop_at_insertion_point: bool, cdata_allowed: bool) -> bool {
        self.stop_at_insertion_point = stop_at_insertion_point;
        self.cdata_allowed = cdata_allowed;

        // Return queued tokens first.
        if self.take_queued_token(out) {
            return true;
        }

        if self.aborted {
            return false;
        }

        loop {
            // Check insertion point before consuming.
            if self.should_pause_before_next_input_character() {
                return false;
            }

            let current_input_character = self.next_code_point();

            match self.state {
                // 13.2.5.1 Data state
                State::Data => {
                    match current_input_character {
                        Some(0x26) => {
                            // '&'
                            self.return_state = State::Data;
                            self.state = State::CharacterReference;
                            continue;
                        }
                        Some(0x3C) => {
                            // '<'
                            if self.try_scan_tag() {
                                return self.emit_current_token_direct(out);
                            }
                            self.state = State::TagOpen;
                            continue;
                        }
                        Some(0x00) => {
                            // NULL - parse error
                            return self.return_character_token(out, 0x00);
                        }
                        None => {
                            self.emit_eof();
                            return self.take_queued_token(out);
                        }
                        Some(cp) => {
                            return self.return_character_token(out, cp);
                        }
                    }
                }

                // 13.2.5.2 RCDATA state
                State::RCDATA => match current_input_character {
                    Some(0x26) => {
                        self.return_state = State::RCDATA;
                        self.state = State::CharacterReference;
                        continue;
                    }
                    Some(0x3C) => {
                        self.state = State::RCDATALessThanSign;
                        continue;
                    }
                    Some(0x00) => {
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // 13.2.5.3 RAWTEXT state
                State::RAWTEXT => match current_input_character {
                    Some(0x3C) => {
                        self.state = State::RAWTEXTLessThanSign;
                        continue;
                    }
                    Some(0x00) => {
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // 13.2.5.4 Script data state
                State::ScriptData => match current_input_character {
                    Some(0x3C) => {
                        self.state = State::ScriptDataLessThanSign;
                        continue;
                    }
                    Some(0x00) => {
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // 13.2.5.5 PLAINTEXT state
                State::PLAINTEXT => match current_input_character {
                    Some(0x00) => {
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // 13.2.5.6 Tag open state
                State::TagOpen => match current_input_character {
                    Some(0x21) => {
                        self.state = State::MarkupDeclarationOpen;
                        continue;
                    }
                    Some(0x2F) => {
                        self.state = State::EndTagOpen;
                        continue;
                    }
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.create_new_token(TokenType::StartTag);
                        self.reconsume(State::TagName);
                        continue;
                    }
                    Some(0x3F) => {
                        // FIXME: Set the temporary buffer to the empty string. Switch to the processing instruction open state.
                        // NB: Until processing instructions are supported, retain the bogus-comment fallback.
                        self.create_new_token(TokenType::Comment);
                        // NB: The comment starts at the "<" of the "<?" that was just consumed.
                        self.current_token.start_offset = SourceOffset::new(self.current_offset - 2);
                        self.reconsume(State::BogusComment);
                        continue;
                    }
                    None => {
                        // parse error
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        // parse error
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.reconsume(State::Data);
                        continue;
                    }
                },

                // 13.2.5.7 End tag open state
                State::EndTagOpen => match current_input_character {
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.create_new_token(TokenType::EndTag);
                        self.reconsume(State::TagName);
                        continue;
                    }
                    Some(0x3E) => {
                        // '>' - parse error
                        self.state = State::Data;
                        continue;
                    }
                    None => {
                        // parse error
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.queued_tokens.push_back(self.make_character_token(0x2F));
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        // parse error
                        self.create_new_token(TokenType::Comment);
                        self.reconsume(State::BogusComment);
                        continue;
                    }
                },

                // 13.2.5.8 Tag name state
                State::TagName => {
                    // Fast-path run: bulk-scan consecutive lowercase
                    // ASCII characters and append them to current_builder.
                    // This handles the common case where a tag name is
                    // a plain `div`, `span`, `a` etc. and avoids paying
                    // the per-character state-machine dispatch cost.
                    // We keep the first character the outer state loop
                    // already consumed, then extend the builder by
                    // scanning from current_offset until we see a byte
                    // that needs special handling.
                    if let Some(cp) = current_input_character
                        && cp >= b'a' as u32
                        && cp <= b'z' as u32
                    {
                        // The inner loop below advances current_offset
                        // directly, one code unit of the name at a time.
                        self.current_builder.push_ascii(cp as u8);
                        let start_offset = self.current_offset;
                        let mut off = start_offset;
                        let input_len = self.fast_scan_limit();
                        while off < input_len {
                            let next = self.input.code_unit_at(off);
                            if next < b'a' as u16 || next > b'z' as u16 {
                                break;
                            }
                            self.current_builder.push_ascii(next as u8);
                            off += 1;
                        }
                        let consumed = off - start_offset;
                        if consumed > 0 {
                            self.prev_offset = off - 1;
                            self.current_offset = off;
                        }
                        continue;
                    }
                    match current_input_character {
                        Some(cp) if is_whitespace(cp) => {
                            self.commit_current_builder_as_tag_name();
                            self.state = State::BeforeAttributeName;
                            continue;
                        }
                        Some(0x2F) => {
                            self.commit_current_builder_as_tag_name();
                            self.state = State::SelfClosingStartTag;
                            continue;
                        }
                        Some(0x3E) => {
                            self.commit_current_builder_as_tag_name();
                            self.state = State::Data;
                            return self.emit_current_token_direct(out);
                        }
                        Some(cp) if is_ascii_upper_alpha(cp) => {
                            self.current_builder.push_code_point(to_ascii_lowercase(cp));
                            continue;
                        }
                        Some(0x00) => {
                            self.current_builder.push_code_point(0xFFFD);
                            continue;
                        }
                        None => {
                            self.emit_eof();
                            return self.take_queued_token(out);
                        }
                        Some(cp) => {
                            self.current_builder.push_code_point(cp);
                            continue;
                        }
                    }
                }

                // 13.2.5.32 Before attribute name state
                State::BeforeAttributeName => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        continue;
                    }
                    Some(0x2F) | Some(0x3E) | None => {
                        self.reconsume(State::AfterAttributeName);
                        continue;
                    }
                    Some(0x3D) => {
                        // '=' - parse error
                        self.current_token.start_new_attribute();
                        self.current_builder.push_ascii(b'=');
                        self.state = State::AttributeName;
                        continue;
                    }
                    Some(_) => {
                        self.current_token.start_new_attribute();
                        self.reconsume(State::AttributeName);
                        continue;
                    }
                },

                // 13.2.5.33 Attribute name state
                State::AttributeName => {
                    // Fast-path run: bulk-scan consecutive lowercase ASCII
                    // and a few other name-safe bytes (digits, '-', '_').
                    if let Some(cp) = current_input_character {
                        let is_name_char = (cp >= b'a' as u32 && cp <= b'z' as u32)
                            || (cp >= b'0' as u32 && cp <= b'9' as u32)
                            || cp == b'-' as u32
                            || cp == b'_' as u32;
                        if is_name_char {
                            self.current_builder.push_ascii(cp as u8);
                            let start_offset = self.current_offset;
                            let mut off = start_offset;
                            let input_len = self.fast_scan_limit();
                            while off < input_len {
                                let next = self.input.code_unit_at(off);
                                let ok = (next >= b'a' as u16 && next <= b'z' as u16)
                                    || (next >= b'0' as u16 && next <= b'9' as u16)
                                    || next == b'-' as u16
                                    || next == b'_' as u16;
                                if !ok {
                                    break;
                                }
                                self.current_builder.push_ascii(next as u8);
                                off += 1;
                            }
                            let consumed = off - start_offset;
                            if consumed > 0 {
                                self.prev_offset = off - 1;
                                self.current_offset = off;
                            }
                            continue;
                        }
                    }
                    match current_input_character {
                        Some(cp) if is_whitespace(cp) => {
                            self.set_attribute_name();
                            self.reconsume(State::AfterAttributeName);
                            continue;
                        }
                        Some(0x2F) | Some(0x3E) | None => {
                            self.set_attribute_name();
                            self.reconsume(State::AfterAttributeName);
                            continue;
                        }
                        Some(0x3D) => {
                            self.set_attribute_name();
                            self.state = State::BeforeAttributeValue;
                            continue;
                        }
                        Some(cp) if is_ascii_upper_alpha(cp) => {
                            self.current_builder.push_code_point(to_ascii_lowercase(cp));
                            continue;
                        }
                        Some(0x00) => {
                            self.current_builder.push_code_point(0xFFFD);
                            continue;
                        }
                        Some(cp @ (0x22 | 0x27 | 0x3C)) => {
                            // '"', '\'', '<' - parse error
                            self.current_builder.push_code_point(cp);
                            continue;
                        }
                        Some(cp) => {
                            self.current_builder.push_code_point(cp);
                            continue;
                        }
                    }
                }

                // 13.2.5.34 After attribute name state
                State::AfterAttributeName => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        continue;
                    }
                    Some(0x2F) => {
                        self.state = State::SelfClosingStartTag;
                        continue;
                    }
                    Some(0x3D) => {
                        self.state = State::BeforeAttributeValue;
                        continue;
                    }
                    Some(0x3E) => {
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        self.current_token.start_new_attribute();
                        self.reconsume(State::AttributeName);
                        continue;
                    }
                },

                // 13.2.5.35 Before attribute value state
                State::BeforeAttributeValue => {
                    let value_start = self.offset_before_last_code_point();
                    if let Some(attr) = self.current_token.attributes_mut().last_mut() {
                        attr.value_start_offset = value_start;
                    }
                    match current_input_character {
                        Some(cp) if is_whitespace(cp) => {
                            continue;
                        }
                        Some(0x22) => {
                            self.state = State::AttributeValueDoubleQuoted;
                            continue;
                        }
                        Some(0x27) => {
                            self.state = State::AttributeValueSingleQuoted;
                            continue;
                        }
                        Some(0x3E) => {
                            // parse error
                            self.state = State::Data;
                            return self.emit_current_token_direct(out);
                        }
                        _ => {
                            self.reconsume(State::AttributeValueUnquoted);
                            continue;
                        }
                    }
                }

                // 13.2.5.36 Attribute value (double-quoted) state
                State::AttributeValueDoubleQuoted => match current_input_character {
                    Some(0x22) => {
                        self.set_attribute_value();
                        self.state = State::AfterAttributeValueQuoted;
                        continue;
                    }
                    Some(0x26) => {
                        self.return_state = State::AttributeValueDoubleQuoted;
                        self.state = State::CharacterReference;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        self.append_attribute_value_run(b'"');
                        continue;
                    }
                },

                // 13.2.5.37 Attribute value (single-quoted) state
                State::AttributeValueSingleQuoted => match current_input_character {
                    Some(0x27) => {
                        self.set_attribute_value();
                        self.state = State::AfterAttributeValueQuoted;
                        continue;
                    }
                    Some(0x26) => {
                        self.return_state = State::AttributeValueSingleQuoted;
                        self.state = State::CharacterReference;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        self.append_attribute_value_run(b'\'');
                        continue;
                    }
                },

                // 13.2.5.38 Attribute value (unquoted) state
                State::AttributeValueUnquoted => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        self.set_attribute_value();
                        self.state = State::BeforeAttributeName;
                        continue;
                    }
                    Some(0x26) => {
                        self.return_state = State::AttributeValueUnquoted;
                        self.state = State::CharacterReference;
                        continue;
                    }
                    Some(0x3E) => {
                        self.set_attribute_value();
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    Some(cp @ (0x22 | 0x27 | 0x3C | 0x3D | 0x60)) => {
                        // parse error, treat as anything else
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.39 After attribute value (quoted) state
                State::AfterAttributeValueQuoted => {
                    let value_end = self.offset_before_last_code_point();
                    if let Some(attr) = self.current_token.attributes_mut().last_mut() {
                        attr.value_end_offset = value_end;
                    }
                    match current_input_character {
                        Some(cp) if is_whitespace(cp) => {
                            self.state = State::BeforeAttributeName;
                            continue;
                        }
                        Some(0x2F) => {
                            self.state = State::SelfClosingStartTag;
                            continue;
                        }
                        Some(0x3E) => {
                            self.state = State::Data;
                            return self.emit_current_token_direct(out);
                        }
                        None => {
                            self.emit_eof();
                            return self.take_queued_token(out);
                        }
                        Some(_) => {
                            // parse error
                            self.reconsume(State::BeforeAttributeName);
                            continue;
                        }
                    }
                }

                // 13.2.5.40 Self-closing start tag state
                State::SelfClosingStartTag => match current_input_character {
                    Some(0x3E) => {
                        self.current_token.set_self_closing(true);
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        // parse error
                        self.reconsume(State::BeforeAttributeName);
                        continue;
                    }
                },

                // 13.2.5.41 Bogus comment state
                State::BogusComment => match current_input_character {
                    Some(0x3E) => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.42 Markup declaration open state
                State::MarkupDeclarationOpen => {
                    // Don't consume next input character (reconsume).
                    if current_input_character.is_some() {
                        self.restore_to(self.prev_offset);
                    }

                    match self.consume_next_if_match_exact("--") {
                        Some(true) => {
                            self.create_new_token(TokenType::Comment);
                            // NB: The comment starts at the "<" of the "<!--" that was just consumed.
                            self.current_token.start_offset = SourceOffset::new(self.current_offset - 4);
                            self.state = State::CommentStart;
                            continue;
                        }
                        None => return false,
                        _ => {}
                    }
                    match self.consume_next_if_match("DOCTYPE") {
                        Some(true) => {
                            self.state = State::DOCTYPE;
                            continue;
                        }
                        None => return false,
                        _ => {}
                    }
                    match self.consume_next_if_match_exact("[CDATA[") {
                        Some(true) => {
                            if self.cdata_allowed {
                                self.state = State::CDATASection;
                            } else {
                                self.create_new_token(TokenType::Comment);
                                self.current_builder.push_ascii_str("[CDATA[");
                                self.state = State::BogusComment;
                            }
                            continue;
                        }
                        None => return false,
                        _ => {}
                    }
                    // parse error
                    self.create_new_token(TokenType::Comment);
                    self.state = State::BogusComment;
                    continue;
                }

                // 13.2.5.43 Comment start state
                State::CommentStart => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::CommentStartDash;
                        continue;
                    }
                    Some(0x3E) => {
                        // parse error
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    _ => {
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.44 Comment start dash state
                State::CommentStartDash => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::CommentEnd;
                        continue;
                    }
                    Some(0x3E) => {
                        // parse error
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        self.current_builder.push_ascii(b'-');
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.45 Comment state
                State::Comment => match current_input_character {
                    Some(0x3C) => {
                        self.current_builder.push_ascii(b'<');
                        self.state = State::CommentLessThanSign;
                        continue;
                    }
                    Some(0x2D) => {
                        self.state = State::CommentEndDash;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    None => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.46 Comment less-than sign state
                State::CommentLessThanSign => match current_input_character {
                    Some(0x21) => {
                        self.current_builder.push_ascii(b'!');
                        self.state = State::CommentLessThanSignBang;
                        continue;
                    }
                    Some(0x3C) => {
                        self.current_builder.push_ascii(b'<');
                        continue;
                    }
                    _ => {
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.47 Comment less-than sign bang state
                State::CommentLessThanSignBang => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::CommentLessThanSignBangDash;
                        continue;
                    }
                    _ => {
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.48 Comment less-than sign bang dash state
                State::CommentLessThanSignBangDash => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::CommentLessThanSignBangDashDash;
                        continue;
                    }
                    _ => {
                        self.reconsume(State::CommentEndDash);
                        continue;
                    }
                },

                // 13.2.5.49 Comment less-than sign bang dash dash state
                State::CommentLessThanSignBangDashDash => match current_input_character {
                    Some(0x3E) | None => {
                        self.reconsume(State::CommentEnd);
                        continue;
                    }
                    _ => {
                        // parse error
                        self.reconsume(State::CommentEnd);
                        continue;
                    }
                },

                // 13.2.5.50 Comment end dash state
                State::CommentEndDash => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::CommentEnd;
                        continue;
                    }
                    None => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        self.current_builder.push_ascii(b'-');
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.51 Comment end state
                State::CommentEnd => match current_input_character {
                    Some(0x3E) => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    Some(0x21) => {
                        self.state = State::CommentEndBang;
                        continue;
                    }
                    Some(0x2D) => {
                        self.current_builder.push_ascii(b'-');
                        continue;
                    }
                    None => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        self.current_builder.push_ascii_str("--");
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.52 Comment end bang state
                State::CommentEndBang => match current_input_character {
                    Some(0x2D) => {
                        self.current_builder.push_ascii_str("--!");
                        self.state = State::CommentEndDash;
                        continue;
                    }
                    Some(0x3E) => {
                        // parse error
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let data = self.consume_current_builder();
                            self.current_token.set_comment_data(data);
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        self.current_builder.push_ascii_str("--!");
                        self.reconsume(State::Comment);
                        continue;
                    }
                },

                // 13.2.5.53 DOCTYPE state
                State::DOCTYPE => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        self.state = State::BeforeDOCTYPEName;
                        continue;
                    }
                    Some(0x3E) => {
                        self.reconsume(State::BeforeDOCTYPEName);
                        continue;
                    }
                    None => {
                        self.create_new_token(TokenType::Doctype);
                        *self.current_token.doctype_data_mut() = DoctypeData {
                            force_quirks: true,
                            missing_name: true,
                            missing_public_identifier: true,
                            missing_system_identifier: true,
                            ..Default::default()
                        };
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        // parse error
                        self.reconsume(State::BeforeDOCTYPEName);
                        continue;
                    }
                },

                // 13.2.5.54 Before DOCTYPE name state
                State::BeforeDOCTYPEName => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        continue;
                    }
                    Some(cp) if is_ascii_upper_alpha(cp) => {
                        self.create_new_token(TokenType::Doctype);
                        *self.current_token.doctype_data_mut() = DoctypeData {
                            missing_name: false,
                            missing_public_identifier: true,
                            missing_system_identifier: true,
                            ..Default::default()
                        };
                        self.current_builder.push_code_point(to_ascii_lowercase(cp));
                        self.state = State::DOCTYPEName;
                        continue;
                    }
                    Some(0x00) => {
                        self.create_new_token(TokenType::Doctype);
                        *self.current_token.doctype_data_mut() = DoctypeData {
                            missing_name: false,
                            missing_public_identifier: true,
                            missing_system_identifier: true,
                            ..Default::default()
                        };
                        self.current_builder.push_code_point(0xFFFD);
                        self.state = State::DOCTYPEName;
                        continue;
                    }
                    Some(0x3E) => {
                        // parse error
                        self.create_new_token(TokenType::Doctype);
                        *self.current_token.doctype_data_mut() = DoctypeData {
                            force_quirks: true,
                            missing_name: true,
                            missing_public_identifier: true,
                            missing_system_identifier: true,
                            ..Default::default()
                        };
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        self.create_new_token(TokenType::Doctype);
                        *self.current_token.doctype_data_mut() = DoctypeData {
                            force_quirks: true,
                            missing_name: true,
                            missing_public_identifier: true,
                            missing_system_identifier: true,
                            ..Default::default()
                        };
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.create_new_token(TokenType::Doctype);
                        *self.current_token.doctype_data_mut() = DoctypeData {
                            missing_name: false,
                            missing_public_identifier: true,
                            missing_system_identifier: true,
                            ..Default::default()
                        };
                        self.current_builder.push_code_point(cp);
                        self.state = State::DOCTYPEName;
                        continue;
                    }
                },

                // 13.2.5.55 DOCTYPE name state
                State::DOCTYPEName => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        {
                            let name = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.name = name;
                            }
                        }
                        self.state = State::AfterDOCTYPEName;
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let name = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.name = name;
                            }
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    Some(cp) if is_ascii_upper_alpha(cp) => {
                        self.current_builder.push_code_point(to_ascii_lowercase(cp));
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    None => {
                        let name = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.name = name;
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.56 After DOCTYPE name state
                State::AfterDOCTYPEName => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        continue;
                    }
                    Some(0x3E) => {
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        // Reconsume and try PUBLIC/SYSTEM
                        if current_input_character.is_some() {
                            self.restore_to(self.prev_offset);
                        }
                        match self.consume_next_if_match("PUBLIC") {
                            Some(true) => {
                                self.state = State::AfterDOCTYPEPublicKeyword;
                                continue;
                            }
                            None => return false,
                            _ => {}
                        }
                        match self.consume_next_if_match("SYSTEM") {
                            Some(true) => {
                                self.state = State::AfterDOCTYPESystemKeyword;
                                continue;
                            }
                            None => return false,
                            _ => {}
                        }
                        // Re-consume the character we put back
                        let _ = self.next_code_point();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.57 After DOCTYPE public keyword state
                State::AfterDOCTYPEPublicKeyword => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        self.state = State::BeforeDOCTYPEPublicIdentifier;
                        continue;
                    }
                    Some(0x22) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.missing_public_identifier = false;
                        }
                        self.state = State::DOCTYPEPublicIdentifierDoubleQuoted;
                        continue;
                    }
                    Some(0x27) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.missing_public_identifier = false;
                        }
                        self.state = State::DOCTYPEPublicIdentifierSingleQuoted;
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.58 Before DOCTYPE public identifier state
                State::BeforeDOCTYPEPublicIdentifier => match current_input_character {
                    Some(cp) if is_whitespace(cp) => continue,
                    Some(0x22) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.missing_public_identifier = false;
                        }
                        self.state = State::DOCTYPEPublicIdentifierDoubleQuoted;
                        continue;
                    }
                    Some(0x27) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.missing_public_identifier = false;
                        }
                        self.state = State::DOCTYPEPublicIdentifierSingleQuoted;
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.59 DOCTYPE public identifier (double-quoted) state
                State::DOCTYPEPublicIdentifierDoubleQuoted => match current_input_character {
                    Some(0x22) => {
                        {
                            let val = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.public_identifier = val;
                            }
                        }
                        self.state = State::AfterDOCTYPEPublicIdentifier;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let val = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.public_identifier = val;
                                dd.force_quirks = true;
                            }
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let val = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.public_identifier = val;
                                dd.force_quirks = true;
                            }
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.60 DOCTYPE public identifier (single-quoted) state
                State::DOCTYPEPublicIdentifierSingleQuoted => match current_input_character {
                    Some(0x27) => {
                        {
                            let val = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.public_identifier = val;
                            }
                        }
                        self.state = State::AfterDOCTYPEPublicIdentifier;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let val = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.public_identifier = val;
                                dd.force_quirks = true;
                            }
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let val = self.consume_current_builder();
                            {
                                let dd = self.current_token.doctype_data_mut();
                                dd.public_identifier = val;
                                dd.force_quirks = true;
                            }
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.61 After DOCTYPE public identifier state
                State::AfterDOCTYPEPublicIdentifier => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        self.state = State::BetweenDOCTYPEPublicAndSystemIdentifiers;
                        continue;
                    }
                    Some(0x3E) => {
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    Some(0x22) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierDoubleQuoted;
                        continue;
                    }
                    Some(0x27) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierSingleQuoted;
                        continue;
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.62 Between DOCTYPE public and system identifiers state
                State::BetweenDOCTYPEPublicAndSystemIdentifiers => match current_input_character {
                    Some(cp) if is_whitespace(cp) => continue,
                    Some(0x3E) => {
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    Some(0x22) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierDoubleQuoted;
                        continue;
                    }
                    Some(0x27) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierSingleQuoted;
                        continue;
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.63 After DOCTYPE system keyword state
                State::AfterDOCTYPESystemKeyword => match current_input_character {
                    Some(cp) if is_whitespace(cp) => {
                        self.state = State::BeforeDOCTYPESystemIdentifier;
                        continue;
                    }
                    Some(0x22) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierDoubleQuoted;
                        continue;
                    }
                    Some(0x27) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierSingleQuoted;
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.64 Before DOCTYPE system identifier state
                State::BeforeDOCTYPESystemIdentifier => match current_input_character {
                    Some(cp) if is_whitespace(cp) => continue,
                    Some(0x22) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierDoubleQuoted;
                        continue;
                    }
                    Some(0x27) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = CodeUnits::new();
                            dd.missing_system_identifier = false;
                        }
                        self.state = State::DOCTYPESystemIdentifierSingleQuoted;
                        continue;
                    }
                    Some(0x3E) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.65 DOCTYPE system identifier (double-quoted) state
                State::DOCTYPESystemIdentifierDoubleQuoted => match current_input_character {
                    Some(0x22) => {
                        let sys_id = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = sys_id;
                        }
                        self.state = State::AfterDOCTYPESystemIdentifier;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    Some(0x3E) => {
                        let sys_id = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = sys_id;
                            dd.force_quirks = true;
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        let sys_id = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = sys_id;
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.66 DOCTYPE system identifier (single-quoted) state
                State::DOCTYPESystemIdentifierSingleQuoted => match current_input_character {
                    Some(0x27) => {
                        let sys_id = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = sys_id;
                        }
                        self.state = State::AfterDOCTYPESystemIdentifier;
                        continue;
                    }
                    Some(0x00) => {
                        self.current_builder.push_code_point(0xFFFD);
                        continue;
                    }
                    Some(0x3E) => {
                        let sys_id = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = sys_id;
                            dd.force_quirks = true;
                        }
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        let sys_id = self.consume_current_builder();
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.system_identifier = sys_id;
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.current_builder.push_code_point(cp);
                        continue;
                    }
                },

                // 13.2.5.67 After DOCTYPE system identifier state
                State::AfterDOCTYPESystemIdentifier => match current_input_character {
                    Some(cp) if is_whitespace(cp) => continue,
                    Some(0x3E) => {
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    None => {
                        {
                            let dd = self.current_token.doctype_data_mut();
                            dd.force_quirks = true;
                        }
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => {
                        // parse error - do NOT set force_quirks
                        self.reconsume(State::BogusDOCTYPE);
                        continue;
                    }
                },

                // 13.2.5.68 Bogus DOCTYPE state
                State::BogusDOCTYPE => match current_input_character {
                    Some(0x3E) => {
                        self.state = State::Data;
                        return self.emit_current_token_direct(out);
                    }
                    Some(0x00) => continue,
                    None => {
                        self.emit_current_token_followed_by_eof();
                        return self.take_queued_token(out);
                    }
                    Some(_) => continue,
                },

                // 13.2.5.69 CDATA section state
                State::CDATASection => match current_input_character {
                    Some(0x5D) => {
                        self.state = State::CDATASectionBracket;
                        continue;
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // 13.2.5.70 CDATA section bracket state
                State::CDATASectionBracket => match current_input_character {
                    Some(0x5D) => {
                        self.state = State::CDATASectionEnd;
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x5D));
                        self.reconsume(State::CDATASection);
                        continue;
                    }
                },

                // 13.2.5.71 CDATA section end state
                State::CDATASectionEnd => match current_input_character {
                    Some(0x5D) => {
                        return self.return_character_token(out, 0x5D);
                    }
                    Some(0x3E) => {
                        self.state = State::Data;
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x5D));
                        self.queued_tokens.push_back(self.make_character_token(0x5D));
                        self.reconsume(State::CDATASection);
                        continue;
                    }
                },

                // https://html.spec.whatwg.org/multipage/parsing.html#character-reference-state
                // Character reference state
                State::CharacterReference => {
                    self.temporary_buffer.clear();
                    self.temporary_buffer.push(0x26); // '&'
                    self.entity_matcher = NamedCharacterReferenceMatcher::new();

                    match current_input_character {
                        Some(cp) if is_ascii_alphanumeric(cp) => {
                            self.reconsume(State::NamedCharacterReference);
                            continue;
                        }
                        Some(0x23) => {
                            self.temporary_buffer.push(0x23);
                            self.state = State::NumericCharacterReference;
                            continue;
                        }
                        _ => {
                            self.flush_codepoints_consumed_as_character_reference();
                            self.reconsume_in_return_state(current_input_character);
                            if !self.queued_tokens.is_empty() {
                                return self.take_queued_token(out);
                            }
                            continue;
                        }
                    }
                }

                // https://html.spec.whatwg.org/multipage/parsing.html#named-character-reference-state
                // Named character reference state
                State::NamedCharacterReference => {
                    // Insertion-point path: feed one character at a time.
                    if self.stop_at_insertion_point && self.insertion_point.is_some() {
                        if let Some(cp) = current_input_character {
                            if self.entity_matcher.try_consume_code_point(cp) {
                                self.temporary_buffer.push(cp);
                                continue;
                            }
                            // Character not accepted by matcher. Reconsume it.
                            self.restore_to(self.prev_offset);
                        } else if self.can_run_out_at(self.current_offset) {
                            // At insertion point with no more chars -- pause.
                            return false;
                        }
                        // Fall through to resolution.
                    } else {
                        // Normal path: feed all remaining chars in a tight loop.
                        if current_input_character.is_some() {
                            self.restore_to(self.prev_offset);
                        }
                        let limit = self.input.len();
                        while self.current_offset < limit {
                            let cp = u32::from(self.input.code_unit_at(self.current_offset));
                            if !self.entity_matcher.try_consume_code_point(cp) {
                                break;
                            }
                            self.temporary_buffer.push(cp);
                            self.advance(1);
                        }
                        if !self.input_stream_closed && self.current_offset >= limit {
                            return false;
                        }
                    }

                    // Resolution: backtrack overconsumed characters.
                    let overconsumed = self.entity_matcher.overconsumed_code_points() as usize;
                    if overconsumed > 0 {
                        self.restore_to(self.current_offset - overconsumed);
                        self.temporary_buffer
                            .truncate(self.temporary_buffer.len() - overconsumed);
                    }

                    if let Some((first_cp, second_cp)) = self.entity_matcher.code_points() {
                        let ends_with_semi = self.entity_matcher.last_match_ends_with_semicolon();

                        // Check special attribute handling.
                        let next_cp = self.peek_code_point(0);
                        if self.consumed_as_part_of_an_attribute()
                            && !ends_with_semi
                            && next_cp.is_some_and(|c| c == 0x3D || is_ascii_alphanumeric(c))
                        {
                            self.flush_codepoints_consumed_as_character_reference();
                            self.state = self.return_state;
                            if !self.queued_tokens.is_empty() {
                                return self.take_queued_token(out);
                            }
                            continue;
                        }

                        if !ends_with_semi {
                            // parse error
                        }

                        self.temporary_buffer.clear();
                        self.temporary_buffer.push(first_cp);
                        if second_cp != 0 {
                            self.temporary_buffer.push(second_cp);
                        }
                        self.flush_codepoints_consumed_as_character_reference();
                        self.state = self.return_state;
                        if !self.queued_tokens.is_empty() {
                            return self.take_queued_token(out);
                        }
                        continue;
                    } else {
                        // No match found.
                        self.flush_codepoints_consumed_as_character_reference();
                        self.state = State::AmbiguousAmpersand;
                        continue;
                    }
                }

                // https://html.spec.whatwg.org/multipage/parsing.html#ambiguous-ampersand-state
                // Ambiguous ampersand state
                State::AmbiguousAmpersand => match current_input_character {
                    Some(cp) if is_ascii_alphanumeric(cp) => {
                        if self.consumed_as_part_of_an_attribute() {
                            self.current_builder.push_code_point(cp);
                            continue;
                        } else {
                            return self.return_character_token(out, cp);
                        }
                    }
                    Some(0x3B) => {
                        // parse error
                        self.reconsume_in_return_state(current_input_character);
                        if !self.queued_tokens.is_empty() {
                            return self.take_queued_token(out);
                        }
                        continue;
                    }
                    _ => {
                        self.reconsume_in_return_state(current_input_character);
                        if !self.queued_tokens.is_empty() {
                            return self.take_queued_token(out);
                        }
                        continue;
                    }
                },

                // https://html.spec.whatwg.org/multipage/parsing.html#numeric-character-reference-state
                // Numeric character reference state
                State::NumericCharacterReference => {
                    self.character_reference_code = 0;
                    match current_input_character {
                        Some(0x78) | Some(0x58) => {
                            // 'x' or 'X'
                            self.temporary_buffer.push(current_input_character.unwrap());
                            self.state = State::HexadecimalCharacterReferenceStart;
                            continue;
                        }
                        _ => {
                            // NB: DecimalCharacterReferenceStart implements the ASCII digit and anything-else cases of this state.
                            self.reconsume(State::DecimalCharacterReferenceStart);
                            continue;
                        }
                    }
                }

                // https://html.spec.whatwg.org/multipage/parsing.html#hexadecimal-character-reference-start-state
                // Hexadecimal character reference start state
                State::HexadecimalCharacterReferenceStart => match current_input_character {
                    Some(cp) if is_ascii_hex_digit(cp) => {
                        self.reconsume(State::HexadecimalCharacterReference);
                        continue;
                    }
                    _ => {
                        // parse error
                        self.flush_codepoints_consumed_as_character_reference();
                        self.reconsume_in_return_state(current_input_character);
                        if !self.queued_tokens.is_empty() {
                            return self.take_queued_token(out);
                        }
                        continue;
                    }
                },

                // NB: This implementation-only state handles the non-X/x cases of the numeric character reference state.
                State::DecimalCharacterReferenceStart => match current_input_character {
                    Some(cp) if is_ascii_digit(cp) => {
                        // Reconsume in the decimal character reference state.
                        self.reconsume(State::DecimalCharacterReference);
                        continue;
                    }
                    _ => {
                        // This is an absence-of-digits-in-numeric-character-reference parse error.
                        // Flush code points consumed as a character reference. Reconsume in the return state.
                        self.flush_codepoints_consumed_as_character_reference();
                        self.reconsume_in_return_state(current_input_character);
                        if !self.queued_tokens.is_empty() {
                            return self.take_queued_token(out);
                        }
                        continue;
                    }
                },

                // https://html.spec.whatwg.org/multipage/parsing.html#hexadecimal-character-reference-state
                // Hexadecimal character reference state
                State::HexadecimalCharacterReference => match current_input_character {
                    Some(cp) if is_ascii_digit(cp) => {
                        self.character_reference_code =
                            self.character_reference_code.wrapping_mul(16).wrapping_add(cp - 0x30);
                        continue;
                    }
                    Some(cp @ 0x41..=0x46) => {
                        self.character_reference_code =
                            self.character_reference_code.wrapping_mul(16).wrapping_add(cp - 0x37);
                        continue;
                    }
                    Some(cp @ 0x61..=0x66) => {
                        self.character_reference_code =
                            self.character_reference_code.wrapping_mul(16).wrapping_add(cp - 0x57);
                        continue;
                    }
                    Some(0x3B) => {
                        self.state = State::NumericCharacterReferenceEnd;
                        continue;
                    }
                    _ => {
                        // parse error
                        self.reconsume(State::NumericCharacterReferenceEnd);
                        continue;
                    }
                },

                // https://html.spec.whatwg.org/multipage/parsing.html#decimal-character-reference-state
                // Decimal character reference state
                State::DecimalCharacterReference => match current_input_character {
                    Some(cp) if is_ascii_digit(cp) => {
                        self.character_reference_code =
                            self.character_reference_code.wrapping_mul(10).wrapping_add(cp - 0x30);
                        continue;
                    }
                    Some(0x3B) => {
                        self.state = State::NumericCharacterReferenceEnd;
                        continue;
                    }
                    _ => {
                        // parse error
                        self.reconsume(State::NumericCharacterReferenceEnd);
                        continue;
                    }
                },

                // https://html.spec.whatwg.org/multipage/parsing.html#numeric-character-reference-end-state
                // Numeric character reference end state
                State::NumericCharacterReferenceEnd => {
                    // Don't consume
                    if current_input_character.is_some() {
                        self.restore_to(self.prev_offset);
                    }

                    let code = self.character_reference_code;
                    let code = if code == 0 || code > 0x10FFFF || is_surrogate(code) {
                        0xFFFD
                    } else if is_noncharacter(code) {
                        code // parse error but keep value
                    } else if let Some(replacement) = numeric_char_ref_replacement(code) {
                        replacement
                    } else if code == 0x0D {
                        0x000D // parse error but keep
                    } else if code != 0 && code <= 0x1F && code != 0x09 && code != 0x0A && code != 0x0C {
                        code // control char, parse error but keep
                    } else if (0x7F..=0x9F).contains(&code) {
                        if let Some(replacement) = numeric_char_ref_replacement(code) {
                            replacement
                        } else {
                            code
                        }
                    } else {
                        code
                    };

                    self.temporary_buffer.clear();
                    self.temporary_buffer.push(code);
                    self.flush_codepoints_consumed_as_character_reference();
                    self.state = self.return_state;
                    if !self.queued_tokens.is_empty() {
                        return self.take_queued_token(out);
                    }
                    continue;
                }

                // RCDATA less-than sign state
                State::RCDATALessThanSign => match current_input_character {
                    Some(0x2F) => {
                        self.temporary_buffer.clear();
                        self.state = State::RCDATAEndTagOpen;
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.reconsume(State::RCDATA);
                        continue;
                    }
                },

                // RCDATA end tag open state
                State::RCDATAEndTagOpen => match current_input_character {
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.create_new_token(TokenType::EndTag);
                        self.reconsume(State::RCDATAEndTagName);
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.queued_tokens.push_back(self.make_character_token(0x2F));
                        self.reconsume(State::RCDATA);
                        continue;
                    }
                },

                // RCDATA end tag name state
                State::RCDATAEndTagName => {
                    self.handle_rawtext_end_tag_name(current_input_character, State::RCDATA);
                    if !self.queued_tokens.is_empty() {
                        return self.take_queued_token(out);
                    }
                    continue;
                }

                // RAWTEXT less-than sign state
                State::RAWTEXTLessThanSign => match current_input_character {
                    Some(0x2F) => {
                        self.temporary_buffer.clear();
                        self.state = State::RAWTEXTEndTagOpen;
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.reconsume(State::RAWTEXT);
                        continue;
                    }
                },

                // RAWTEXT end tag open state
                State::RAWTEXTEndTagOpen => match current_input_character {
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.create_new_token(TokenType::EndTag);
                        self.reconsume(State::RAWTEXTEndTagName);
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.queued_tokens.push_back(self.make_character_token(0x2F));
                        self.reconsume(State::RAWTEXT);
                        continue;
                    }
                },

                // RAWTEXT end tag name state
                State::RAWTEXTEndTagName => {
                    self.handle_rawtext_end_tag_name(current_input_character, State::RAWTEXT);
                    if !self.queued_tokens.is_empty() {
                        return self.take_queued_token(out);
                    }
                    continue;
                }

                // Script data less-than sign state
                State::ScriptDataLessThanSign => match current_input_character {
                    Some(0x2F) => {
                        self.temporary_buffer.clear();
                        self.state = State::ScriptDataEndTagOpen;
                        continue;
                    }
                    Some(0x21) => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.queued_tokens.push_back(self.make_character_token(0x21));
                        self.state = State::ScriptDataEscapeStart;
                        return self.take_queued_token(out);
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.reconsume(State::ScriptData);
                        continue;
                    }
                },

                // Script data end tag open state
                State::ScriptDataEndTagOpen => match current_input_character {
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.create_new_token(TokenType::EndTag);
                        self.reconsume(State::ScriptDataEndTagName);
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.queued_tokens.push_back(self.make_character_token(0x2F));
                        self.reconsume(State::ScriptData);
                        continue;
                    }
                },

                // Script data end tag name state
                State::ScriptDataEndTagName => {
                    self.handle_rawtext_end_tag_name(current_input_character, State::ScriptData);
                    if !self.queued_tokens.is_empty() {
                        return self.take_queued_token(out);
                    }
                    continue;
                }

                // Script data escape start state
                State::ScriptDataEscapeStart => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::ScriptDataEscapeStartDash;
                        return self.return_character_token(out, 0x2D);
                    }
                    _ => {
                        self.reconsume(State::ScriptData);
                        continue;
                    }
                },

                // Script data escape start dash state
                State::ScriptDataEscapeStartDash => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::ScriptDataEscapedDashDash;
                        return self.return_character_token(out, 0x2D);
                    }
                    _ => {
                        self.reconsume(State::ScriptData);
                        continue;
                    }
                },

                // Script data escaped state
                State::ScriptDataEscaped => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::ScriptDataEscapedDash;
                        return self.return_character_token(out, 0x2D);
                    }
                    Some(0x3C) => {
                        self.state = State::ScriptDataEscapedLessThanSign;
                        continue;
                    }
                    Some(0x00) => {
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // Script data escaped dash state
                State::ScriptDataEscapedDash => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::ScriptDataEscapedDashDash;
                        return self.return_character_token(out, 0x2D);
                    }
                    Some(0x3C) => {
                        self.state = State::ScriptDataEscapedLessThanSign;
                        continue;
                    }
                    Some(0x00) => {
                        self.state = State::ScriptDataEscaped;
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.state = State::ScriptDataEscaped;
                        return self.return_character_token(out, cp);
                    }
                },

                // Script data escaped dash dash state
                State::ScriptDataEscapedDashDash => match current_input_character {
                    Some(0x2D) => {
                        return self.return_character_token(out, 0x2D);
                    }
                    Some(0x3C) => {
                        self.state = State::ScriptDataEscapedLessThanSign;
                        continue;
                    }
                    Some(0x3E) => {
                        self.state = State::ScriptData;
                        return self.return_character_token(out, 0x3E);
                    }
                    Some(0x00) => {
                        self.state = State::ScriptDataEscaped;
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.state = State::ScriptDataEscaped;
                        return self.return_character_token(out, cp);
                    }
                },

                // Script data escaped less-than sign state
                State::ScriptDataEscapedLessThanSign => match current_input_character {
                    Some(0x2F) => {
                        self.temporary_buffer.clear();
                        self.state = State::ScriptDataEscapedEndTagOpen;
                        continue;
                    }
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.temporary_buffer.clear();
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.reconsume(State::ScriptDataDoubleEscapeStart);
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.reconsume(State::ScriptDataEscaped);
                        continue;
                    }
                },

                // Script data escaped end tag open state
                State::ScriptDataEscapedEndTagOpen => match current_input_character {
                    Some(cp) if is_ascii_alpha(cp) => {
                        self.create_new_token(TokenType::EndTag);
                        self.reconsume(State::ScriptDataEscapedEndTagName);
                        continue;
                    }
                    _ => {
                        self.queued_tokens.push_back(self.make_character_token(0x3C));
                        self.queued_tokens.push_back(self.make_character_token(0x2F));
                        self.reconsume(State::ScriptDataEscaped);
                        continue;
                    }
                },

                // Script data escaped end tag name state
                State::ScriptDataEscapedEndTagName => {
                    self.handle_rawtext_end_tag_name(current_input_character, State::ScriptDataEscaped);
                    if !self.queued_tokens.is_empty() {
                        return self.take_queued_token(out);
                    }
                    continue;
                }

                // Script data double escape start state
                State::ScriptDataDoubleEscapeStart => match current_input_character {
                    Some(cp) if is_whitespace(cp) || cp == 0x2F || cp == 0x3E => {
                        if self.temporary_buffer_equals_script() {
                            self.state = State::ScriptDataDoubleEscaped;
                        } else {
                            self.state = State::ScriptDataEscaped;
                        }
                        return self.return_character_token(out, cp);
                    }
                    Some(cp) if is_ascii_upper_alpha(cp) => {
                        self.temporary_buffer.push(to_ascii_lowercase(cp));
                        return self.return_character_token(out, cp);
                    }
                    Some(cp) if is_ascii_lower_alpha(cp) => {
                        self.temporary_buffer.push(cp);
                        return self.return_character_token(out, cp);
                    }
                    _ => {
                        self.reconsume(State::ScriptDataEscaped);
                        continue;
                    }
                },

                // Script data double escaped state
                State::ScriptDataDoubleEscaped => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::ScriptDataDoubleEscapedDash;
                        return self.return_character_token(out, 0x2D);
                    }
                    Some(0x3C) => {
                        self.state = State::ScriptDataDoubleEscapedLessThanSign;
                        return self.return_character_token(out, 0x3C);
                    }
                    Some(0x00) => {
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        return self.return_character_token(out, cp);
                    }
                },

                // Script data double escaped dash state
                State::ScriptDataDoubleEscapedDash => match current_input_character {
                    Some(0x2D) => {
                        self.state = State::ScriptDataDoubleEscapedDashDash;
                        return self.return_character_token(out, 0x2D);
                    }
                    Some(0x3C) => {
                        self.state = State::ScriptDataDoubleEscapedLessThanSign;
                        return self.return_character_token(out, 0x3C);
                    }
                    Some(0x00) => {
                        self.state = State::ScriptDataDoubleEscaped;
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.state = State::ScriptDataDoubleEscaped;
                        return self.return_character_token(out, cp);
                    }
                },

                // Script data double escaped dash dash state
                State::ScriptDataDoubleEscapedDashDash => match current_input_character {
                    Some(0x2D) => {
                        return self.return_character_token(out, 0x2D);
                    }
                    Some(0x3C) => {
                        self.state = State::ScriptDataDoubleEscapedLessThanSign;
                        return self.return_character_token(out, 0x3C);
                    }
                    Some(0x3E) => {
                        self.state = State::ScriptData;
                        return self.return_character_token(out, 0x3E);
                    }
                    Some(0x00) => {
                        self.state = State::ScriptDataDoubleEscaped;
                        return self.return_character_token(out, 0xFFFD);
                    }
                    None => {
                        self.emit_eof();
                        return self.take_queued_token(out);
                    }
                    Some(cp) => {
                        self.state = State::ScriptDataDoubleEscaped;
                        return self.return_character_token(out, cp);
                    }
                },

                // Script data double escaped less-than sign state
                State::ScriptDataDoubleEscapedLessThanSign => match current_input_character {
                    Some(0x2F) => {
                        self.temporary_buffer.clear();
                        self.state = State::ScriptDataDoubleEscapeEnd;
                        return self.return_character_token(out, 0x2F);
                    }
                    _ => {
                        self.reconsume(State::ScriptDataDoubleEscaped);
                        continue;
                    }
                },

                // Script data double escape end state
                State::ScriptDataDoubleEscapeEnd => match current_input_character {
                    Some(cp) if is_whitespace(cp) || cp == 0x2F || cp == 0x3E => {
                        if self.temporary_buffer_equals_script() {
                            self.state = State::ScriptDataEscaped;
                        } else {
                            self.state = State::ScriptDataDoubleEscaped;
                        }
                        return self.return_character_token(out, cp);
                    }
                    Some(cp) if is_ascii_upper_alpha(cp) => {
                        self.temporary_buffer.push(to_ascii_lowercase(cp));
                        return self.return_character_token(out, cp);
                    }
                    Some(cp) if is_ascii_lower_alpha(cp) => {
                        self.temporary_buffer.push(cp);
                        return self.return_character_token(out, cp);
                    }
                    _ => {
                        self.reconsume(State::ScriptDataDoubleEscaped);
                        continue;
                    }
                },
            }
        }
    }

    // -- Helper methods --

    fn reconsume(&mut self, new_state: State) {
        self.state = new_state;
        if self.current_offset > 0 {
            self.restore_to(self.prev_offset);
        }
    }

    fn reconsume_in_return_state(&mut self, current_input_character: Option<u32>) {
        self.state = self.return_state;
        if current_input_character.is_some() {
            self.restore_to(self.prev_offset);
        }
    }

    #[inline(always)]
    fn make_character_token(&self, code_point: u32) -> Token {
        Token::new_character(code_point)
    }

    fn set_attribute_name(&mut self) {
        let name = crate::token::HtmlName::from_code_units(self.current_builder.as_slice());
        // NB: Every code point of a name is one code point of input, in as many code units.
        let name_length = self.current_builder.len();
        let name_end = self.offset_before_last_code_point();
        let name_start = SourceOffset::new(name_end.get().unwrap().saturating_sub(name_length));
        self.current_builder.clear();
        if let Some(attr) = self.current_token.attributes_mut().last_mut() {
            attr.local_name = name;
            attr.name_start_offset = name_start;
            attr.name_end_offset = name_end;
        }
    }

    fn set_attribute_value(&mut self) {
        let value_end = self.offset_before_last_code_point();
        if let Some(attr) = self.current_token.attributes_mut().last_mut() {
            // The builder continues with the attribute's spare buffer, so neither side grows again.
            std::mem::swap(&mut attr.value, &mut self.current_builder);
            attr.value_end_offset = value_end;
        }
        self.current_builder.clear();
    }

    fn temporary_buffer_equals_script(&self) -> bool {
        self.temporary_buffer == [0x73, 0x63, 0x72, 0x69, 0x70, 0x74]
    }

    /// Handle the common end-tag-name pattern shared by RCDATA, RAWTEXT,
    /// ScriptData, and ScriptDataEscaped end tag name states.
    fn handle_rawtext_end_tag_name(&mut self, current_input_character: Option<u32>, fallback_state: State) {
        match current_input_character {
            Some(cp) if is_whitespace(cp) || cp == 0x2F || cp == 0x3E => {
                self.commit_current_builder_as_tag_name();
                if self.current_end_tag_token_is_appropriate() {
                    match cp {
                        _ if is_whitespace(cp) => {
                            self.state = State::BeforeAttributeName;
                            return;
                        }
                        0x2F => {
                            self.state = State::SelfClosingStartTag;
                            return;
                        }
                        0x3E => {
                            self.state = State::Data;
                            self.emit_current_token();
                            return;
                        }
                        _ => {}
                    }
                }
                // Not appropriate - emit buffered chars
                self.queued_tokens.push_back(self.make_character_token(0x3C));
                self.queued_tokens.push_back(self.make_character_token(0x2F));
                for i in 0..self.temporary_buffer.len() {
                    let cp = self.temporary_buffer[i];
                    self.queued_tokens.push_back(self.make_character_token(cp));
                }
                self.current_builder.clear();
                self.reconsume(fallback_state);
            }
            Some(cp) if is_ascii_upper_alpha(cp) => {
                self.current_builder.push_code_point(to_ascii_lowercase(cp));
                self.temporary_buffer.push(cp);
            }
            Some(cp) if is_ascii_lower_alpha(cp) => {
                self.current_builder.push_ascii(cp as u8);
                self.temporary_buffer.push(cp);
            }
            _ => {
                self.queued_tokens.push_back(self.make_character_token(0x3C));
                self.queued_tokens.push_back(self.make_character_token(0x2F));
                for i in 0..self.temporary_buffer.len() {
                    let cp = self.temporary_buffer[i];
                    self.queued_tokens.push_back(self.make_character_token(cp));
                }
                self.current_builder.clear();
                self.reconsume(fallback_state);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn code_units(input: &str) -> Vec<u16> {
        input.encode_utf16().collect()
    }

    #[test]
    fn parser_does_not_compact_unresolved_named_character_reference() {
        let mut tokenizer = HtmlTokenizer::new(code_units("<p a=\"&tw"));
        tokenizer.set_input_stream_closed(false);

        assert!(tokenizer.next_token(false, false).is_none());
        assert_eq!(tokenizer.state, State::NamedCharacterReference);
        assert_eq!(tokenizer.current_offset, tokenizer.input.len());

        tokenizer.parser_did_run();
        assert_eq!(tokenizer.current_offset, tokenizer.input.len());
        assert_eq!(tokenizer.input.to_utf16(), code_units("<p a=\"&tw"));

        tokenizer.append_input(Utf16View::Utf16(&code_units("s-checkout-success=4.1.0\">")));
        tokenizer.set_input_stream_closed(true);

        let token = tokenizer.next_token(false, false).expect("start tag token");
        assert_eq!(token.token_type, TokenType::StartTag);

        let TokenPayload::Tag { attributes, .. } = token.payload else {
            panic!("expected tag token");
        };
        assert_eq!(attributes.len(), 1);
        assert_eq!(attributes[0].value, "&tws-checkout-success=4.1.0");
    }

    fn line_and_column(tokenizer: &mut HtmlTokenizer, offset: SourceOffset) -> (u64, u64) {
        let position = tokenizer.position_at(offset);
        (position.line, position.column)
    }

    /// The tokens of the tokenizer's input and then `second`, where the parser has run out of input in between.
    fn tokens_of_two_chunks(tokenizer: &mut HtmlTokenizer, second: &str) -> Vec<Token> {
        let mut tokens = Vec::new();
        tokenizer.set_input_stream_closed(false);
        while let Some(token) = tokenizer.next_token(false, false) {
            tokens.push(token);
        }
        tokenizer.parser_did_run();
        tokenizer.append_input(Utf16View::Utf16(&code_units(second)));
        tokenizer.set_input_stream_closed(true);
        while let Some(token) = tokenizer.next_token(false, false) {
            tokens.push(token);
        }
        tokens
    }

    #[test]
    fn parser_keeps_input_that_an_unfinished_tag_points_into() {
        let mut tokenizer = HtmlTokenizer::new(code_units("\n\n<div class=\"a\" "));
        let tokens = tokens_of_two_chunks(&mut tokenizer, "id=x>");
        let tag = tokens
            .iter()
            .find(|token| token.token_type == TokenType::StartTag)
            .expect("start tag token");
        assert_eq!(line_and_column(&mut tokenizer, tag.start_offset), (2, 1));
        let TokenPayload::Tag { attributes, .. } = &tag.payload else {
            panic!("expected tag token");
        };
        let name_start = attributes[0].name_start_offset;
        assert_eq!(line_and_column(&mut tokenizer, name_start), (2, 5));
    }

    #[test]
    fn parser_keeps_input_that_leads_up_to_a_comment() {
        for (first, second) in [("\n<!", "-- x -->"), ("\n<", "? x >")] {
            let mut tokenizer = HtmlTokenizer::new(code_units(first));
            let tokens = tokens_of_two_chunks(&mut tokenizer, second);
            let comment = tokens
                .iter()
                .find(|token| token.token_type == TokenType::Comment)
                .expect("comment token");
            assert_eq!(line_and_column(&mut tokenizer, comment.start_offset), (1, 0));
        }
    }
}
