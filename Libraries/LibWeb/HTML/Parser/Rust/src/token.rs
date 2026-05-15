/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Source position in the input.
#[derive(Clone, Copy, Debug, Default)]
pub struct Position {
    pub line: u64,
    pub column: u64,
}

/// A single attribute on a start or end tag token.
///
/// If `local_name_id` is non-zero it is an index into
/// `interned_names::INTERNED_ATTR_NAMES` and `local_name` is unused.
/// Otherwise `local_name` holds the owned bytes.
#[derive(Clone, Debug, Default)]
pub struct Attribute {
    pub local_name: String,
    pub local_name_id: u16,
    pub value: String,
    pub name_start_position: Position,
    pub name_end_position: Position,
    pub value_start_position: Position,
    pub value_end_position: Position,
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
/// If `tag_name_id` is non-zero it is an index into
/// `interned_names::INTERNED_TAG_NAMES` and `tag_name` is unused.
/// Otherwise `tag_name` holds the owned bytes.
#[derive(Clone, Debug, Default)]
pub enum TokenPayload {
    #[default]
    None,
    Tag {
        tag_name: String,
        tag_name_id: u16,
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

    /// Return the tag name as a &str. For interned names this resolves
    /// through the interned name table; for un-interned names it returns
    /// the owned String's contents.
    #[inline(always)]
    pub fn tag_name(&self) -> &str {
        match &self.payload {
            TokenPayload::Tag {
                tag_name, tag_name_id, ..
            } => {
                if *tag_name_id != 0 {
                    // SAFETY: interned names are compile-time ASCII byte
                    // literals in interned_names_generated.rs, so they are
                    // always valid UTF-8.
                    match crate::interned_names::tag_name_by_id(*tag_name_id) {
                        Some(bytes) => unsafe { std::str::from_utf8_unchecked(bytes) },
                        None => "",
                    }
                } else {
                    tag_name
                }
            }
            _ => "",
        }
    }

    #[inline(always)]
    pub fn tag_name_mut(&mut self) -> &mut String {
        match &mut self.payload {
            TokenPayload::Tag {
                tag_name, tag_name_id, ..
            } => {
                *tag_name_id = 0;
                tag_name
            }
            _ => panic!("tag_name_mut called on non-tag token"),
        }
    }

    /// Set the tag name to an interned id, clearing any previously stored
    /// owned name.
    #[inline(always)]
    pub fn set_tag_name_id(&mut self, id: u16) {
        match &mut self.payload {
            TokenPayload::Tag {
                tag_name, tag_name_id, ..
            } => {
                tag_name.clear();
                *tag_name_id = id;
            }
            _ => panic!("set_tag_name_id called on non-tag token"),
        }
    }

    /// Returns the interned tag-name id, or 0 if the name is un-interned.
    #[inline(always)]
    pub fn tag_name_id(&self) -> u16 {
        match &self.payload {
            TokenPayload::Tag { tag_name_id, .. } => *tag_name_id,
            _ => 0,
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
            let is_duplicate = (0..i).any(|seen_index| {
                attribute_local_name_bytes(&attributes[seen_index]) == attribute_local_name_bytes(&attributes[i])
            });
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

fn attribute_local_name_bytes(attribute: &Attribute) -> &[u8] {
    if attribute.local_name_id != 0 {
        crate::interned_names::attr_name_by_id(attribute.local_name_id).unwrap_or_default()
    } else {
        attribute.local_name.as_bytes()
    }
}
