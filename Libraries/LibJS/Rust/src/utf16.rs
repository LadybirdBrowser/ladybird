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

/// Mirrors AK::Span::index_of(): finds where `needle` first occurs in `haystack` at or after `start_offset`, comparing
/// the whole needle only where `find_first` finds its first element. The needle must be non-empty and fit in the
/// haystack after `start_offset`.
fn find_subslice<T: Copy + PartialEq>(
    haystack: &[T],
    needle: &[T],
    start_offset: usize,
    find_first: impl Fn(&[T], T) -> Option<usize>,
) -> Option<usize> {
    let (&first, rest) = needle.split_first().expect("the needle is not empty");
    let last_possible_offset = haystack.len() - needle.len();
    let mut offset = start_offset;
    while offset <= last_possible_offset {
        offset += find_first(&haystack[offset..=last_possible_offset], first)?;
        if haystack[offset + 1..offset + needle.len()] == *rest {
            return Some(offset);
        }
        offset += 1;
    }
    None
}

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

    pub fn starts_with(self, prefix: Utf16View<'_>) -> bool {
        prefix.length_in_code_units() <= self.length_in_code_units()
            && self.substring_view(0, prefix.length_in_code_units()) == prefix
    }

    /// Mirrors AK::Utf16View::equals_ignoring_ascii_case: equal code units once ASCII letters are lowercased.
    pub fn equals_ignoring_ascii_case(self, other: Utf16View<'_>) -> bool {
        let to_ascii_lowercase = |code_unit: u16| {
            if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
                code_unit + 0x20
            } else {
                code_unit
            }
        };
        self.length_in_code_units() == other.length_in_code_units()
            && self
                .code_units()
                .zip(other.code_units())
                .all(|(this_code_unit, other_code_unit)| {
                    to_ascii_lowercase(this_code_unit) == to_ascii_lowercase(other_code_unit)
                })
    }

    pub fn ends_with_code_unit(self, code_unit: u16) -> bool {
        !self.is_empty() && self.code_unit_at(self.length_in_code_units() - 1) == code_unit
    }

    /// Mirrors AK::Utf16View::find_code_unit_offset(Utf16View const&, size_t): the first offset at or after
    /// `start_offset` where `needle` occurs, which an empty needle does at `start_offset` itself unless that is past
    /// the end.
    pub fn find_code_unit_offset(self, needle: Utf16View<'_>, start_offset: usize) -> Option<usize> {
        let needle_length = needle.length_in_code_units();
        let maximum_offset = start_offset.checked_add(needle_length)?;
        if maximum_offset > self.length_in_code_units() {
            return None;
        }

        if needle_length == 0 {
            return Some(start_offset);
        }

        let last_possible_offset = self.length_in_code_units() - needle_length;
        match (self, needle) {
            (Self::Ascii(haystack), Utf16View::Ascii(needle)) => {
                find_subslice(haystack, needle, start_offset, |candidates, first_byte| {
                    // SAFETY: The pointer and length describe the candidates slice.
                    let found =
                        unsafe { libc::memchr(candidates.as_ptr().cast(), first_byte.into(), candidates.len()) };
                    (!found.is_null()).then(|| found as usize - candidates.as_ptr() as usize)
                })
            }
            (Self::Utf16(haystack), Utf16View::Utf16(needle)) => {
                find_subslice(haystack, needle, start_offset, |candidates, first_code_unit| {
                    candidates.iter().position(|&code_unit| code_unit == first_code_unit)
                })
            }
            _ => (start_offset..=last_possible_offset)
                .find(|&offset| self.substring_view(offset, needle_length) == needle),
        }
    }

    /// Mirrors AK::Utf16View::validate(): whether the code units are well-formed UTF-16, without unpaired surrogates.
    pub fn validate(self) -> bool {
        match self {
            Self::Ascii(_) => true,
            Self::Utf16(units) => ak::validate_utf16(units),
        }
    }

    /// Mirrors AK::Utf16View::trim(): the view without the leading and/or trailing code units that are in
    /// `code_units`.
    pub fn trim(self, code_units: &[u16], mode: TrimMode) -> Self {
        let mut substring_start = 0;
        let mut substring_end = self.length_in_code_units();

        if matches!(mode, TrimMode::Left | TrimMode::Both) {
            while substring_start < substring_end && code_units.contains(&self.code_unit_at(substring_start)) {
                substring_start += 1;
            }
        }

        if matches!(mode, TrimMode::Right | TrimMode::Both) {
            while substring_end > substring_start && code_units.contains(&self.code_unit_at(substring_end - 1)) {
                substring_end -= 1;
            }
        }

        self.substring_view(substring_start, substring_end - substring_start)
    }

    /// Mirrors AK::Utf16String::to_lowercase() without a locale, which LibUnicode implements with ICU's full case
    /// mapping in the default locale.
    pub fn to_lowercase(self) -> Utf16String {
        if let Self::Ascii(units) = self {
            return Utf16String::from_ascii_with(units.len(), |storage| {
                for (byte, unit) in storage.iter_mut().zip(units) {
                    *byte = unit.to_ascii_lowercase();
                }
            });
        }
        crate::unicode::apply_case_mapping(self, crate::unicode::CaseMapping::Lowercase, None, false)
    }

    /// Mirrors AK::Utf16String::to_uppercase() without a locale, which LibUnicode implements with ICU's full case
    /// mapping in the default locale.
    pub fn to_uppercase(self) -> Utf16String {
        if let Self::Ascii(units) = self {
            return Utf16String::from_ascii_with(units.len(), |storage| {
                for (byte, unit) in storage.iter_mut().zip(units) {
                    *byte = unit.to_ascii_uppercase();
                }
            });
        }
        crate::unicode::apply_case_mapping(self, crate::unicode::CaseMapping::Uppercase, None, false)
    }
}

/// Mirrors AK::TrimMode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TrimMode {
    Left,
    Right,
    Both,
}

pub const HIGH_SURROGATE_MIN: u16 = 0xd800;
pub const HIGH_SURROGATE_MAX: u16 = 0xdbff;
pub const LOW_SURROGATE_MIN: u16 = 0xdc00;
pub const LOW_SURROGATE_MAX: u16 = 0xdfff;
pub const FIRST_SUPPLEMENTARY_PLANE_CODE_POINT: u32 = 0x10000;

pub fn is_unicode_surrogate(code_unit: u16) -> bool {
    (HIGH_SURROGATE_MIN..=LOW_SURROGATE_MAX).contains(&code_unit)
}

pub fn is_utf16_high_surrogate(code_unit: u16) -> bool {
    (HIGH_SURROGATE_MIN..=HIGH_SURROGATE_MAX).contains(&code_unit)
}

pub fn is_utf16_low_surrogate(code_unit: u16) -> bool {
    (LOW_SURROGATE_MIN..=LOW_SURROGATE_MAX).contains(&code_unit)
}

pub fn decode_utf16_surrogate_pair(high_surrogate: u16, low_surrogate: u16) -> u32 {
    (u32::from(high_surrogate - HIGH_SURROGATE_MIN) << 10)
        + u32::from(low_surrogate - LOW_SURROGATE_MIN)
        + FIRST_SUPPLEMENTARY_PLANE_CODE_POINT
}

/// Mirrors AK::Utf16StringBuilder: collects code units as ASCII bytes until one of them is not ASCII, and builds a string
/// with ASCII storage when all of them are.
pub struct Utf16StringBuilder {
    storage: BuilderStorage,
}

enum BuilderStorage {
    Ascii(Vec<u8>),
    Utf16(Vec<u16>),
}

impl Default for Utf16StringBuilder {
    fn default() -> Self {
        Self::with_capacity(0)
    }
}

impl Utf16StringBuilder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            storage: BuilderStorage::Ascii(Vec::with_capacity(capacity)),
        }
    }

    pub fn is_empty(&self) -> bool {
        match &self.storage {
            BuilderStorage::Ascii(bytes) => bytes.is_empty(),
            BuilderStorage::Utf16(code_units) => code_units.is_empty(),
        }
    }

    pub fn len(&self) -> usize {
        match &self.storage {
            BuilderStorage::Ascii(bytes) => bytes.len(),
            BuilderStorage::Utf16(code_units) => code_units.len(),
        }
    }

    pub fn truncate(&mut self, length: usize) {
        match &mut self.storage {
            BuilderStorage::Ascii(bytes) => bytes.truncate(length),
            BuilderStorage::Utf16(code_units) => code_units.truncate(length),
        }
    }

    /// The code units as UTF-16, widening the ASCII bytes collected so far the first time a code unit is not ASCII.
    fn utf16_code_units(&mut self, additional: usize) -> &mut Vec<u16> {
        if let BuilderStorage::Ascii(bytes) = &self.storage {
            let mut code_units = Vec::with_capacity((bytes.len() + additional).max(bytes.capacity()));
            code_units.extend(bytes.iter().map(|&byte| u16::from(byte)));
            self.storage = BuilderStorage::Utf16(code_units);
        }
        match &mut self.storage {
            BuilderStorage::Utf16(code_units) => code_units,
            BuilderStorage::Ascii(_) => unreachable!("the storage was just widened"),
        }
    }

    pub fn append(&mut self, view: Utf16View<'_>) {
        match (&mut self.storage, view) {
            (BuilderStorage::Ascii(bytes), Utf16View::Ascii(units)) => bytes.extend_from_slice(units),
            (BuilderStorage::Ascii(bytes), Utf16View::Utf16(units)) if units.iter().all(|&unit| unit < 0x80) => {
                bytes.extend(units.iter().map(|&unit| unit as u8));
            }
            _ => {
                let code_units = self.utf16_code_units(view.length_in_code_units());
                view.append_to(code_units);
            }
        }
    }

    pub fn append_ascii(&mut self, ascii: &str) {
        self.append_ascii_bytes(ascii.as_bytes());
    }

    pub fn append_ascii_bytes(&mut self, ascii: &[u8]) {
        debug_assert!(ascii.is_ascii());
        match &mut self.storage {
            BuilderStorage::Ascii(bytes) => bytes.extend_from_slice(ascii),
            BuilderStorage::Utf16(code_units) => code_units.extend(ascii.iter().map(|&byte| u16::from(byte))),
        }
    }

    pub fn append_repeated_ascii(&mut self, ascii: u8, count: usize) {
        debug_assert!(ascii.is_ascii());
        match &mut self.storage {
            BuilderStorage::Ascii(bytes) => bytes.extend(core::iter::repeat_n(ascii, count)),
            BuilderStorage::Utf16(code_units) => code_units.extend(core::iter::repeat_n(u16::from(ascii), count)),
        }
    }

    /// Mirrors AK::Utf16StringBuilder::append_quoted_escaped_for_json(): the string as a JSON string literal, which
    /// QuoteJSONString specifies.
    pub fn append_quoted_escaped_for_json(&mut self, string: Utf16View<'_>) {
        if let (BuilderStorage::Ascii(bytes), Utf16View::Ascii(units)) = (&mut self.storage, string)
            && !units.iter().fold(false, |needs_escaping, &byte| {
                needs_escaping | (byte < 0x20) | (byte == b'"') | (byte == b'\\')
            })
        {
            bytes.reserve(units.len() + 2);
            bytes.push(b'"');
            bytes.extend_from_slice(units);
            bytes.push(b'"');
            return;
        }

        self.append_code_unit(u16::from(b'"'));
        let length = string.length_in_code_units();
        let mut index = 0;
        while index < length {
            let code_unit = string.code_unit_at(index);
            if is_utf16_high_surrogate(code_unit)
                && index + 1 < length
                && is_utf16_low_surrogate(string.code_unit_at(index + 1))
            {
                let code_units = self.utf16_code_units(2);
                code_units.push(code_unit);
                code_units.push(string.code_unit_at(index + 1));
                index += 2;
                continue;
            }
            match code_unit {
                0x08 => self.append_ascii("\\b"),
                0x09 => self.append_ascii("\\t"),
                0x0A => self.append_ascii("\\n"),
                0x0C => self.append_ascii("\\f"),
                0x0D => self.append_ascii("\\r"),
                0x22 => self.append_ascii("\\\""),
                0x5C => self.append_ascii("\\\\"),
                _ if code_unit < 0x20 || is_unicode_surrogate(code_unit) => {
                    const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";
                    self.append_ascii_bytes(&[
                        b'\\',
                        b'u',
                        HEX_DIGITS[usize::from(code_unit >> 12)],
                        HEX_DIGITS[usize::from((code_unit >> 8) & 0xf)],
                        HEX_DIGITS[usize::from((code_unit >> 4) & 0xf)],
                        HEX_DIGITS[usize::from(code_unit & 0xf)],
                    ]);
                }
                _ => self.append_code_unit(code_unit),
            }
            index += 1;
        }
        self.append_code_unit(u16::from(b'"'));
    }

    pub fn append_utf8(&mut self, utf8: &str) {
        if utf8.is_ascii() {
            self.append_ascii(utf8);
            return;
        }
        self.utf16_code_units(utf8.len()).extend(utf8.encode_utf16());
    }

    pub fn append_code_unit(&mut self, code_unit: u16) {
        match &mut self.storage {
            BuilderStorage::Ascii(bytes) if code_unit < 0x80 => bytes.push(code_unit as u8),
            _ => self.utf16_code_units(1).push(code_unit),
        }
    }

    /// Appends UTF16EncodeCodePoint(`code_point`), as AK::UnicodeUtils::code_point_to_utf16 encodes it.
    pub fn append_code_point(&mut self, code_point: u32) {
        assert!(code_point <= 0x10ffff);
        if code_point < FIRST_SUPPLEMENTARY_PLANE_CODE_POINT {
            self.append_code_unit(code_point as u16);
            return;
        }
        let code_point = code_point - FIRST_SUPPLEMENTARY_PLANE_CODE_POINT;
        let code_units = self.utf16_code_units(2);
        code_units.push(HIGH_SURROGATE_MIN | (code_point >> 10) as u16);
        code_units.push(LOW_SURROGATE_MIN | (code_point & 0x3ff) as u16);
    }

    pub fn append_repeated(&mut self, view: Utf16View<'_>, count: usize) {
        let additional = view.length_in_code_units().saturating_mul(count);
        match &mut self.storage {
            BuilderStorage::Ascii(bytes) => bytes.reserve(additional),
            BuilderStorage::Utf16(code_units) => code_units.reserve(additional),
        }
        for _ in 0..count {
            self.append(view);
        }
    }

    pub fn to_utf16_string(&self) -> Utf16String {
        match &self.storage {
            BuilderStorage::Ascii(bytes) => Utf16String::from_ascii(bytes),
            BuilderStorage::Utf16(code_units) => Utf16String::from_utf16(code_units),
        }
    }
}

/// The UTF-16 counterpart of fmt::Display, mirroring AK::Formatter<T> as Utf16String::formatted() uses it: appends the
/// value as code units, so a string keeps the unpaired surrogates that formatting through UTF-8 would replace.
pub trait Utf16Display {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder);
}

/// Mirrors AK::Utf16String::formatted(): `format` with each `{}` replaced by the next of `arguments`.
pub fn utf16_formatted(format: &str, arguments: &[&dyn Utf16Display]) -> Utf16String {
    let mut builder = Utf16StringBuilder::new();
    let mut pieces = format.split("{}");
    builder.append_utf8(pieces.next().unwrap_or_default());
    let mut arguments = arguments.iter();
    for piece in pieces {
        let argument = arguments
            .next()
            .unwrap_or_else(|| panic!("\"{format}\" needs more arguments than it was given"));
        argument.fmt_utf16(&mut builder);
        builder.append_utf8(piece);
    }
    assert!(
        arguments.next().is_none(),
        "\"{format}\" was given more arguments than it needs"
    );
    builder.to_utf16_string()
}

impl<T: Utf16Display + ?Sized> Utf16Display for &T {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        (**self).fmt_utf16(builder);
    }
}

impl Utf16Display for str {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append_utf8(self);
    }
}

impl Utf16Display for String {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append_utf8(self);
    }
}

impl Utf16Display for Utf16View<'_> {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append(*self);
    }
}

impl Utf16Display for Utf16String {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append(Utf16View::of_string(self));
    }
}

impl Utf16Display for Utf16FlyString {
    fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
        builder.append(Utf16View::of_fly_string(self));
    }
}

macro_rules! impl_utf16_display_through_display {
    ($($type:ty),*) => {
        $(
            impl Utf16Display for $type {
                fn fmt_utf16(&self, builder: &mut Utf16StringBuilder) {
                    builder.append_utf8(&self.to_string());
                }
            }
        )*
    };
}

impl_utf16_display_through_display!(i8, i16, i32, i64, isize, u8, u16, u32, u64, usize, f32, f64, bool, char);

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
    assert!(units.is_ascii(), "ASCII storage holds ASCII");
    // SAFETY: ASCII is UTF-8, and checking for it is cheaper than validating UTF-8 in general.
    unsafe { core::str::from_utf8_unchecked(units) }
}

/// Mirrors utf16_string_external_memory_size() of LibJS/Runtime/ExternalMemory.h: the bytes of a string's storage
/// outside its one-word representation, which a short string does not have.
pub fn utf16_string_external_memory_size(string: &Utf16String) -> usize {
    if has_short_ascii_storage(string) {
        return 0;
    }
    match string.as_units() {
        Utf16StringUnits::Ascii(units) => units.len(),
        Utf16StringUnits::Utf16(units) => size_of_val(units),
    }
}

/// The concatenation of two strings with short ASCII storage if it is short as well, which AK keeps in one word: the
/// byte count in the tag byte and the bytes after it. The new word is made from the two words directly.
pub fn concatenate_short_ascii_strings(lhs: &Utf16String, rhs: &Utf16String) -> Option<Utf16String> {
    assert!(has_short_ascii_storage(lhs) && has_short_ascii_storage(rhs));
    let byte_count_of = |raw: usize| (raw & 0xff) >> ak::SHORT_STRING_BYTE_COUNT_SHIFT;
    let (lhs_raw, rhs_raw) = (lhs.raw_identity(), rhs.raw_identity());
    let (lhs_byte_count, rhs_byte_count) = (byte_count_of(lhs_raw), byte_count_of(rhs_raw));
    let byte_count = lhs_byte_count + rhs_byte_count;
    if byte_count > MAX_SHORT_STRING_BYTE_COUNT {
        return None;
    }

    // The tag is the lowest byte of the word on the little-endian targets the runtime builds for.
    const _: () = assert!(cfg!(target_endian = "little"));
    let bytes_of = |raw: usize, byte_count: usize| (raw >> 8) & ((1usize << (8 * byte_count)) - 1);
    let bytes = bytes_of(lhs_raw, lhs_byte_count) | (bytes_of(rhs_raw, rhs_byte_count) << (8 * lhs_byte_count));
    let raw = (bytes << 8) | (byte_count << ak::SHORT_STRING_BYTE_COUNT_SHIFT) | ak::SHORT_STRING_FLAG;
    // SAFETY: The word is AK's short string of the bytes of both strings, and a short string owns nothing.
    Some(unsafe { Utf16String::from_raw_owned(raw) })
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

/// Mirrors AK::Utf16String::to_well_formed(): the string with every unpaired surrogate replaced by U+FFFD.
pub fn to_well_formed(string: &Utf16String) -> Utf16String {
    match string.as_units() {
        Utf16StringUnits::Utf16(units) if !ak::validate_utf16(units) => Utf16String::well_formed_from_utf16(units),
        _ => string.clone(),
    }
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
        return Utf16String::from_ascii_concatenation(
            length,
            views.iter().map(|view| match view {
                Utf16View::Ascii(units) => *units,
                Utf16View::Utf16(_) => unreachable!("every view has ASCII storage"),
            }),
        );
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
