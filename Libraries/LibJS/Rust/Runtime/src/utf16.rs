/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The operations on AK's UTF-16 strings that the runtime needs and the ak crate does not provide. The ak crate only
//! exposes a string's storage, so new strings are built here by copying code units.

use core::sync::atomic::Ordering;

use ak::{Utf16FlyString, Utf16String, Utf16StringUnits};

/// The longest string AK stores inline in its one-word representation, as ASCII.
pub const MAX_SHORT_STRING_BYTE_COUNT: usize = size_of::<usize>() - 1;

/// Mirrors AK::Detail::Utf16StringData::Flag::IsFlyString.
const IS_FLY_STRING_FLAG: u32 = 1 << 2;

/// Mirrors AK::Utf16View: a borrowed run of UTF-16 code units, stored either as ASCII bytes or as UTF-16.
#[derive(Clone, Copy, Debug)]
pub enum Utf16View<'a> {
    Ascii(&'a [u8]),
    Utf16(&'a [u16]),
}

impl<'a> Utf16View<'a> {
    pub const EMPTY: Utf16View<'static> = Utf16View::Ascii(&[]);

    pub fn from_units(units: Utf16StringUnits<'a>) -> Self {
        match units {
            Utf16StringUnits::Ascii(units) => Self::Ascii(units),
            Utf16StringUnits::Utf16(units) => Self::Utf16(units),
        }
    }

    pub fn of_string(string: &'a Utf16String) -> Self {
        Self::from_units(string.as_units())
    }

    pub fn of_fly_string(string: &'a Utf16FlyString) -> Self {
        Self::from_units(string.as_units())
    }

    pub fn length_in_code_units(self) -> usize {
        match self {
            Self::Ascii(units) => units.len(),
            Self::Utf16(units) => units.len(),
        }
    }

    pub fn is_empty(self) -> bool {
        self.length_in_code_units() == 0
    }

    pub fn has_ascii_storage(self) -> bool {
        matches!(self, Self::Ascii(_))
    }

    pub fn is_ascii(self) -> bool {
        match self {
            Self::Ascii(_) => true,
            Self::Utf16(units) => units.iter().all(|&code_unit| code_unit < 0x80),
        }
    }

    pub fn code_unit_at(self, index: usize) -> u16 {
        match self {
            Self::Ascii(units) => u16::from(units[index]),
            Self::Utf16(units) => units[index],
        }
    }

    pub fn substring_view(self, code_unit_offset: usize, code_unit_length: usize) -> Self {
        let range = code_unit_offset..code_unit_offset + code_unit_length;
        match self {
            Self::Ascii(units) => Self::Ascii(&units[range]),
            Self::Utf16(units) => Self::Utf16(&units[range]),
        }
    }

    pub fn code_units(self) -> CodeUnits<'a> {
        match self {
            Self::Ascii(units) => CodeUnits::Ascii(units.iter()),
            Self::Utf16(units) => CodeUnits::Utf16(units.iter()),
        }
    }

    /// Lends the code units to `callback` as UTF-16, widening ASCII storage on the stack when it is short.
    pub fn with_utf16_code_units<R>(self, callback: impl FnOnce(&[u16]) -> R) -> R {
        const STACK_CAPACITY: usize = 64;
        match self {
            Self::Utf16(units) => callback(units),
            Self::Ascii(units) if units.len() <= STACK_CAPACITY => {
                let mut widened = [0; STACK_CAPACITY];
                for (wide, &unit) in widened.iter_mut().zip(units) {
                    *wide = u16::from(unit);
                }
                callback(&widened[..units.len()])
            }
            Self::Ascii(units) => callback(&units.iter().map(|&unit| u16::from(unit)).collect::<Vec<_>>()),
        }
    }

    pub fn append_to(self, code_units: &mut Vec<u16>) {
        match self {
            Self::Ascii(units) => code_units.extend(units.iter().map(|&code_unit| u16::from(code_unit))),
            Self::Utf16(units) => code_units.extend_from_slice(units),
        }
    }

    pub fn to_utf16_string(self) -> Utf16String {
        match self {
            Self::Ascii(units) => Utf16String::from_utf8(ascii_as_str(units)),
            Self::Utf16(units) => Utf16String::from_utf16(units),
        }
    }

    pub fn to_utf16_fly_string(self) -> Utf16FlyString {
        match self {
            Self::Ascii(units) => Utf16FlyString::from_utf8(ascii_as_str(units)),
            Self::Utf16(units) => Utf16FlyString::from_utf16(units),
        }
    }

    /// Converts to UTF-8, replacing unpaired surrogates with U+FFFD like AK::Utf16View::to_utf8.
    pub fn to_utf8(self) -> String {
        match self {
            Self::Ascii(units) => ascii_as_str(units).to_owned(),
            Self::Utf16(units) => String::from_utf16_lossy(units),
        }
    }

    /// Appends the string to `output` as UTF-8 with each unpaired surrogate encoded as a three-byte sequence of its
    /// own (WTF-8), the bytes AK::StringBuilder produces when it appends a Utf16View.
    pub fn append_as_wtf8_to(self, output: &mut Vec<u8>) {
        let units = match self {
            Self::Ascii(units) => {
                output.extend_from_slice(units);
                return;
            }
            Self::Utf16(units) => units,
        };
        for decoded in char::decode_utf16(units.iter().copied()) {
            match decoded {
                Ok(character) => {
                    let mut buffer = [0; 4];
                    output.extend_from_slice(character.encode_utf8(&mut buffer).as_bytes());
                }
                Err(error) => {
                    let surrogate = error.unpaired_surrogate();
                    output.extend_from_slice(&[
                        0xE0 | (surrogate >> 12) as u8,
                        0x80 | ((surrogate >> 6) & 0x3F) as u8,
                        0x80 | (surrogate & 0x3F) as u8,
                    ]);
                }
            }
        }
    }

    pub fn to_wtf8(self) -> Vec<u8> {
        let mut output = Vec::with_capacity(self.length_in_code_units());
        self.append_as_wtf8_to(&mut output);
        output
    }

    /// Mirrors AK::Utf16View::is_code_unit_less_than: compares the code units in order, and a proper prefix is less.
    pub fn is_code_unit_less_than(self, other: Utf16View<'_>) -> bool {
        let common_length = self.length_in_code_units().min(other.length_in_code_units());

        for position in 0..common_length {
            let this_code_unit = self.code_unit_at(position);
            let other_code_unit = other.code_unit_at(position);

            if this_code_unit != other_code_unit {
                return this_code_unit < other_code_unit;
            }
        }

        self.length_in_code_units() < other.length_in_code_units()
    }
}

impl PartialEq for Utf16View<'_> {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Ascii(left), Self::Ascii(right)) => left == right,
            (Self::Utf16(left), Self::Utf16(right)) => left == right,
            _ => {
                self.length_in_code_units() == other.length_in_code_units() && self.code_units().eq(other.code_units())
            }
        }
    }
}

impl Eq for Utf16View<'_> {}

impl PartialEq<str> for Utf16View<'_> {
    fn eq(&self, other: &str) -> bool {
        self.code_units().eq(other.encode_utf16())
    }
}

impl PartialEq<&str> for Utf16View<'_> {
    fn eq(&self, other: &&str) -> bool {
        *self == **other
    }
}

/// The code units of a Utf16View, widened to u16.
#[derive(Clone)]
pub enum CodeUnits<'a> {
    Ascii(core::slice::Iter<'a, u8>),
    Utf16(core::slice::Iter<'a, u16>),
}

impl Iterator for CodeUnits<'_> {
    type Item = u16;

    fn next(&mut self) -> Option<u16> {
        match self {
            Self::Ascii(units) => units.next().map(|&code_unit| u16::from(code_unit)),
            Self::Utf16(units) => units.next().copied(),
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        match self {
            Self::Ascii(units) => units.size_hint(),
            Self::Utf16(units) => units.size_hint(),
        }
    }
}

impl ExactSizeIterator for CodeUnits<'_> {}

fn ascii_as_str(units: &[u8]) -> &str {
    core::str::from_utf8(units).expect("ASCII storage holds ASCII")
}

/// Mirrors AK::Utf16String::has_short_ascii_storage.
pub fn has_short_ascii_storage(string: &Utf16String) -> bool {
    string.raw_identity() & ak::SHORT_STRING_FLAG != 0
}

/// Mirrors AK::Utf16String::has_fly_string_storage: whether the string is stored as a fly string already, either
/// inline or in storage interned in AK's fly string table.
pub fn has_fly_string_storage(string: &Utf16String) -> bool {
    if has_short_ascii_storage(string) {
        return true;
    }
    // SAFETY: A string without short storage is the address of its live data, which starts with this header.
    let header = unsafe { &*core::ptr::with_exposed_provenance::<ak::Utf16StringDataHeader>(string.raw_identity()) };
    header.flags.load(Ordering::Acquire) & IS_FLY_STRING_FLAG != 0
}

/// Mirrors the AK::Utf16FlyString constructor from a Utf16String. AK interns the string's own storage when no equal
/// fly string exists yet; the ak crate cannot add existing storage to the table, so this interns a copy instead.
pub fn to_utf16_fly_string(string: &Utf16String) -> Utf16FlyString {
    if has_fly_string_storage(string) {
        // SAFETY: The string is live while the reference is added, and that reference moves to the fly string, which
        // can own the storage since it is a fly string's already.
        return unsafe {
            ak::reference_utf16_string(string.raw_identity());
            Utf16FlyString::from_raw_owned(string.raw_identity())
        };
    }
    Utf16View::of_string(string).to_utf16_fly_string()
}

/// Concatenates views into a new string, ASCII when every view is.
pub fn concatenate(views: &[Utf16View<'_>]) -> Utf16String {
    let length = views.iter().map(|view| view.length_in_code_units()).sum();
    if views.iter().all(|view| view.has_ascii_storage()) {
        let mut bytes = Vec::with_capacity(length);
        for view in views {
            if let Utf16View::Ascii(units) = view {
                bytes.extend_from_slice(units);
            }
        }
        return Utf16String::from_utf8(ascii_as_str(&bytes));
    }
    let mut code_units = Vec::with_capacity(length);
    for view in views {
        view.append_to(&mut code_units);
    }
    Utf16String::from_utf16(&code_units)
}

/// Decodes UTF-8 into UTF-16 code units the way AK's Utf16String::from_utf8() accepts it: a lone surrogate encoded as
/// a three-byte sequence (WTF-8) becomes that surrogate code unit. Returns None if `bytes` is not valid otherwise.
pub fn utf16_from_wtf8(bytes: &[u8]) -> Option<Vec<u16>> {
    let mut code_units = Vec::with_capacity(bytes.len());
    let mut remaining = bytes;
    loop {
        match core::str::from_utf8(remaining) {
            Ok(text) => {
                code_units.extend(text.encode_utf16());
                return Some(code_units);
            }
            Err(error) => {
                let (valid, rest) = remaining.split_at(error.valid_up_to());
                // SAFETY: from_utf8 validated this prefix.
                code_units.extend(unsafe { core::str::from_utf8_unchecked(valid) }.encode_utf16());
                let [0xED, second @ 0xA0..=0xBF, third @ 0x80..=0xBF, ..] = *rest else {
                    return None;
                };
                code_units.push(0xD000 | (u16::from(second & 0x3F) << 6) | u16::from(third & 0x3F));
                remaining = &rest[3..];
            }
        }
    }
}

/// Decodes UTF-8 for display as AK's String::from_utf8_with_replacement_character() does: without a leading byte
/// order mark, and with invalid sequences replaced.
pub fn string_from_utf8_with_replacement_character(bytes: &[u8]) -> String {
    let bytes = bytes.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(bytes);
    String::from_utf8_lossy(bytes).into_owned()
}
