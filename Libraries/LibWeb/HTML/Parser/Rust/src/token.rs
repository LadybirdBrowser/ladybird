/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(not(test))]
use ak::Utf16StringUnits;

/// Source position in the input.
#[derive(Clone, Copy, Debug, Default)]
pub struct Position {
    pub line: u64,
    pub column: u64,
}

/// A single attribute on a start or end tag token.
///
#[derive(Clone, Debug, Default)]
pub struct Attribute {
    pub local_name: HtmlName,
    pub value: String,
    pub name_start_position: Position,
    pub name_end_position: Position,
    pub value_start_position: Position,
    pub value_end_position: Position,
}

#[cfg(not(test))]
#[derive(Clone, Default, Eq)]
pub struct HtmlName(ak::Utf16FlyString);

// Standalone Rust test binaries have no C++ fly-string table to own these names.
#[cfg(test)]
#[derive(Clone, Default, Eq)]
pub struct HtmlName(String);

#[cfg(not(test))]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct KnownName(usize);

#[cfg(test)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum KnownName {
    Raw(usize),
    Literal(&'static str),
}

impl KnownName {
    pub const fn from_raw(raw: usize) -> Self {
        #[cfg(not(test))]
        {
            Self(raw)
        }
        #[cfg(test)]
        {
            Self::Raw(raw)
        }
    }

    #[cfg(test)]
    pub const fn from_literal(name: &'static str) -> Self {
        Self::Literal(name)
    }
}

impl HtmlName {
    pub fn from_utf8(name: &str) -> Self {
        #[cfg(not(test))]
        {
            Self(ak::Utf16FlyString::from_utf8(name))
        }
        #[cfg(test)]
        {
            Self(name.to_string())
        }
    }

    /// Borrow an existing C++ `Utf16FlyString` identity.
    ///
    /// # Safety
    /// `raw` must identify a live `Utf16FlyString` for the duration of this call.
    pub unsafe fn from_borrowed_raw(raw: usize) -> Self {
        #[cfg(not(test))]
        {
            unsafe { ak::reference_utf16_string(raw) };
            Self(unsafe { ak::Utf16FlyString::from_raw_owned(raw) })
        }
        #[cfg(test)]
        {
            let _ = raw;
            unreachable!("C++ string identities are unavailable in standalone Rust tests")
        }
    }

    pub fn raw_identity(&self) -> usize {
        #[cfg(not(test))]
        {
            self.0.raw_identity()
        }
        #[cfg(test)]
        {
            0
        }
    }

    pub fn equals(&self, other: &str) -> bool {
        #[cfg(test)]
        return self.0 == other;

        #[cfg(not(test))]
        {
            if let Some(raw) = ak::utf16_short_string_raw(other) {
                return self.raw_identity() == raw;
            }
            match self.0.as_units() {
                Utf16StringUnits::Ascii(units) => units == other.as_bytes(),
                Utf16StringUnits::Utf16(units) => units.iter().copied().eq(other.encode_utf16()),
            }
        }
    }

    pub fn is_one_of(&self, names: &[KnownName]) -> bool {
        #[cfg(not(test))]
        {
            names.contains(&KnownName::from_raw(self.raw_identity()))
        }
        #[cfg(test)]
        {
            names.iter().any(|name| self == name)
        }
    }

    pub fn eq_ignore_ascii_case(&self, other: &Self) -> bool {
        #[cfg(test)]
        return self.0.eq_ignore_ascii_case(&other.0);

        #[cfg(not(test))]
        {
            match (self.0.as_units(), other.0.as_units()) {
                (Utf16StringUnits::Ascii(left), Utf16StringUnits::Ascii(right)) => left.eq_ignore_ascii_case(right),
                (Utf16StringUnits::Utf16(left), Utf16StringUnits::Utf16(right)) => {
                    left.len() == right.len()
                        && left.iter().zip(right).all(|(left, right)| {
                            left == right
                                || (*left <= 0x7f
                                    && *right <= 0x7f
                                    && (*left as u8).eq_ignore_ascii_case(&(*right as u8)))
                        })
                }
                (Utf16StringUnits::Ascii(left), Utf16StringUnits::Utf16(right))
                | (Utf16StringUnits::Utf16(right), Utf16StringUnits::Ascii(left)) => {
                    left.len() == right.len()
                        && left.iter().zip(right).all(|(left, right)| {
                            u16::from(*left) == *right || (*right <= 0x7f && left.eq_ignore_ascii_case(&(*right as u8)))
                        })
                }
            }
        }
    }
}

impl PartialEq<KnownName> for HtmlName {
    fn eq(&self, other: &KnownName) -> bool {
        #[cfg(not(test))]
        {
            self.raw_identity() == other.0
        }
        #[cfg(test)]
        {
            match other {
                KnownName::Raw(raw) => self.raw_identity() == *raw,
                KnownName::Literal(name) => self.0 == *name,
            }
        }
    }
}

impl PartialEq<HtmlName> for KnownName {
    fn eq(&self, other: &HtmlName) -> bool {
        other == self
    }
}

impl PartialEq for HtmlName {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}

impl PartialEq<str> for HtmlName {
    fn eq(&self, other: &str) -> bool {
        self.equals(other)
    }
}

impl PartialEq<&str> for HtmlName {
    fn eq(&self, other: &&str) -> bool {
        self.equals(other)
    }
}

impl PartialEq<&HtmlName> for HtmlName {
    fn eq(&self, other: &&HtmlName) -> bool {
        self == *other
    }
}

impl std::fmt::Debug for HtmlName {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        #[cfg(not(test))]
        {
            formatter.debug_tuple("HtmlName").field(&self.0.raw_identity()).finish()
        }
        #[cfg(test)]
        {
            formatter.debug_tuple("HtmlName").field(&self.0).finish()
        }
    }
}

/// Data specific to DOCTYPE tokens.
#[derive(Clone, Debug, Default)]
pub struct DoctypeData {
    pub name: String,
    pub public_identifier: String,
    pub system_identifier: String,
    pub missing_name: bool,
    pub missing_public_identifier: bool,
    pub missing_system_identifier: bool,
    pub force_quirks: bool,
}

/// The type of an HTML token.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum TokenType {
    #[default]
    Invalid = 0,
    Doctype = 1,
    StartTag = 2,
    EndTag = 3,
    Comment = 4,
    Character = 5,
    EndOfFile = 6,
}

/// Type-specific data for an HTML token.
///
#[derive(Clone, Debug, Default)]
pub enum TokenPayload {
    #[default]
    None,
    Tag {
        tag_name: HtmlName,
        self_closing: bool,
        // AD-HOC: See AD-HOC comment on Element.m_had_duplicate_attribute_during_tokenization about why this is tracked.
        had_duplicate_attribute: bool,
        attributes: Vec<Attribute>,
    },
    Comment(String),
    Doctype(Box<DoctypeData>),
}

/// An HTML token produced by the tokenizer.
#[derive(Clone, Debug, Default)]
pub struct Token {
    pub token_type: TokenType,
    pub code_point: u32,
    pub payload: TokenPayload,
    pub start_position: Position,
    pub end_position: Position,
}

impl Token {
    pub fn new_character(code_point: u32) -> Self {
        Token {
            token_type: TokenType::Character,
            code_point,
            ..Default::default()
        }
    }

    pub fn new_eof() -> Self {
        Token {
            token_type: TokenType::EndOfFile,
            ..Default::default()
        }
    }

    #[inline(always)]
    pub fn tag_name(&self) -> &HtmlName {
        match &self.payload {
            TokenPayload::Tag { tag_name, .. } => tag_name,
            _ => panic!("tag_name called on non-tag token"),
        }
    }

    pub fn set_tag_name(&mut self, name: HtmlName) {
        match &mut self.payload {
            TokenPayload::Tag { tag_name, .. } => *tag_name = name,
            _ => panic!("set_tag_name called on non-tag token"),
        }
    }

    #[inline(always)]
    pub fn set_self_closing(&mut self, value: bool) {
        match &mut self.payload {
            TokenPayload::Tag { self_closing, .. } => *self_closing = value,
            _ => panic!("set_self_closing called on non-tag token"),
        }
    }

    #[inline(always)]
    pub fn is_self_closing(&self) -> bool {
        match &self.payload {
            TokenPayload::Tag { self_closing, .. } => *self_closing,
            _ => false,
        }
    }

    #[inline(always)]
    pub fn had_duplicate_attribute(&self) -> bool {
        match &self.payload {
            TokenPayload::Tag {
                had_duplicate_attribute,
                ..
            } => *had_duplicate_attribute,
            _ => false,
        }
    }

    #[inline(always)]
    pub fn attributes_mut(&mut self) -> &mut Vec<Attribute> {
        match &mut self.payload {
            TokenPayload::Tag { attributes, .. } => attributes,
            _ => panic!("attributes_mut called on non-tag token"),
        }
    }

    pub fn normalize_attributes(&mut self) {
        let TokenPayload::Tag {
            attributes,
            had_duplicate_attribute,
            ..
        } = &mut self.payload
        else {
            return;
        };

        let mut i = 0;
        while i < attributes.len() {
            let is_duplicate = (0..i).any(|seen_index| attributes[seen_index].local_name == attributes[i].local_name);
            if is_duplicate {
                *had_duplicate_attribute = true;
                attributes.remove(i);
            } else {
                i += 1;
            }
        }
    }

    #[inline(always)]
    pub fn set_comment_data(&mut self, data: String) {
        match &mut self.payload {
            TokenPayload::Comment(s) => *s = data,
            _ => panic!("set_comment_data called on non-comment token"),
        }
    }

    #[inline(always)]
    pub fn doctype_data_mut(&mut self) -> &mut DoctypeData {
        match &mut self.payload {
            TokenPayload::Doctype(dd) => dd,
            _ => panic!("doctype_data_mut called on non-doctype token"),
        }
    }
}
