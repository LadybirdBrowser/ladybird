/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Borrowed URL input. Offsets refer to storage units, not Unicode scalar values.
#[derive(Clone, Copy)]
pub enum UrlInput<'a> {
    Utf8(&'a str),
    Utf16(&'a [u16]),
}

#[derive(Clone)]
enum CharIndices<'a> {
    Utf8(std::str::CharIndices<'a>),
    Utf16 {
        characters: std::char::DecodeUtf16<std::iter::Copied<std::slice::Iter<'a, u16>>>,
        offset: usize,
    },
}

impl Iterator for CharIndices<'_> {
    type Item = (usize, char);

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Utf8(characters) => characters.next(),
            Self::Utf16 { characters, offset } => {
                let character = characters.next()?.unwrap_or(char::REPLACEMENT_CHARACTER);
                let start = *offset;
                *offset += character.len_utf16();
                Some((start, character))
            }
        }
    }
}

impl<'a> UrlInput<'a> {
    pub(crate) fn len(self) -> usize {
        match self {
            Self::Utf8(text) => text.len(),
            Self::Utf16(text) => text.len(),
        }
    }

    pub(crate) fn is_empty(self) -> bool {
        self.len() == 0
    }

    pub(crate) fn slice(self, range: std::ops::Range<usize>) -> Self {
        match self {
            Self::Utf8(text) => Self::Utf8(&text[range]),
            Self::Utf16(text) => Self::Utf16(&text[range]),
        }
    }

    pub(crate) fn suffix(self, offset: usize) -> Self {
        self.slice(offset..self.len())
    }

    pub(crate) fn char_length(self, character: char) -> usize {
        match self {
            Self::Utf8(_) => character.len_utf8(),
            Self::Utf16(_) => character.len_utf16(),
        }
    }

    pub(crate) fn chars(self) -> impl Iterator<Item = char> + Clone + 'a {
        self.char_indices().map(|(_, character)| character)
    }

    pub(crate) fn char_indices(self) -> impl Iterator<Item = (usize, char)> + Clone + 'a {
        match self {
            Self::Utf8(text) => CharIndices::Utf8(text.char_indices()),
            Self::Utf16(text) => CharIndices::Utf16 {
                characters: char::decode_utf16(text.iter().copied()),
                offset: 0,
            },
        }
    }

    pub(crate) fn starts_with(self, prefix: &str) -> bool {
        match self {
            Self::Utf8(text) => text.starts_with(prefix),
            Self::Utf16(text) if prefix.is_ascii() => {
                let mut units = text.iter();
                prefix.bytes().all(|byte| units.next() == Some(&u16::from(byte)))
            }
            Self::Utf16(_) => {
                let mut characters = self.chars();
                prefix.chars().all(|character| characters.next() == Some(character))
            }
        }
    }

    pub(crate) fn find(self, character: char) -> Option<usize> {
        match self {
            Self::Utf8(text) => text.find(character),
            Self::Utf16(text) if character.is_ascii() => text.iter().position(|unit| *unit == character as u16),
            Self::Utf16(_) => self
                .char_indices()
                .find_map(|(offset, candidate)| (candidate == character).then_some(offset)),
        }
    }

    pub(crate) fn ascii_prefix_length(self, predicate: impl Fn(u8) -> bool) -> usize {
        match self {
            Self::Utf8(text) => text
                .as_bytes()
                .iter()
                .position(|byte| !byte.is_ascii() || !predicate(*byte))
                .unwrap_or(text.len()),
            Self::Utf16(text) => text
                .iter()
                .position(|unit| *unit > 0x7f || !predicate(*unit as u8))
                .unwrap_or(text.len()),
        }
    }

    pub(crate) fn append_ascii_to(self, output: &mut String) {
        match self {
            Self::Utf8(text) => {
                assert!(text.is_ascii());
                output.push_str(text);
            }
            Self::Utf16(units) => {
                output.reserve(units.len());
                for &unit in units {
                    assert!(unit <= 0x7f);
                    output.push(char::from(unit as u8));
                }
            }
        }
    }

    pub(crate) fn ascii_string(self) -> Option<String> {
        let mut output = String::with_capacity(self.len());
        for character in self.chars() {
            if !character.is_ascii() {
                return None;
            }
            output.push(character);
        }
        Some(output)
    }

    pub(crate) fn is_ascii(self) -> bool {
        match self {
            Self::Utf8(text) => text.is_ascii(),
            Self::Utf16(text) => text.iter().all(|unit| *unit <= 0x7f),
        }
    }

    pub(crate) fn to_utf16(self) -> std::borrow::Cow<'a, [u16]> {
        match self {
            Self::Utf8(text) => std::borrow::Cow::Owned(text.encode_utf16().collect()),
            Self::Utf16(text) => std::borrow::Cow::Borrowed(text),
        }
    }

    pub(crate) fn trim_ascii_c0(self) -> Self {
        match self {
            Self::Utf8(text) => Self::Utf8(text.trim_matches(super::parser::is_ascii_c0_control_or_space)),
            Self::Utf16(text) => {
                let is_retained = |unit: &u16| *unit > 0x20 && *unit != 0x7f;
                let start = text.iter().position(is_retained).unwrap_or(text.len());
                let end = text.iter().rposition(is_retained).map_or(start, |index| index + 1);
                Self::Utf16(&text[start..end])
            }
        }
    }
}

impl<'a> From<&'a str> for UrlInput<'a> {
    fn from(text: &'a str) -> Self {
        Self::Utf8(text)
    }
}

impl<'a> From<&'a String> for UrlInput<'a> {
    fn from(text: &'a String) -> Self {
        Self::Utf8(text)
    }
}

impl<'a> From<&'a [u16]> for UrlInput<'a> {
    fn from(text: &'a [u16]) -> Self {
        Self::Utf16(text)
    }
}
