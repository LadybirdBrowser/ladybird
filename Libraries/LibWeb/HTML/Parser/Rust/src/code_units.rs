/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::scan;
use ak::Utf16View;
use std::ops::Range;

/// A string of code units, in either of the two forms the engine's own strings have: one byte per code unit while
/// every code unit is below U+0100, and UTF-16 otherwise.
///
/// The tokenizer's input stream is one of these, and so is everything the tokenizer reads out of it. Most documents
/// only have code units below U+0100. A string is widened the first time it is given a code unit that doesn't fit.
/// Both forms are indexed by code unit, so offsets into a string stay the same when that happens.
#[derive(Clone, Debug)]
pub enum CodeUnits {
    Latin1(Vec<u8>),
    Utf16(Vec<u16>),
}

/// Where a run of code units is in the input.
#[derive(Clone, Debug)]
pub struct InputRun {
    pub range: Range<usize>,
    /// Whether every code unit of the run is known to be ASCII.
    pub is_ascii: bool,
}

/// A `Utf16View` as the host is handed one. Exactly one pointer is non-null unless the view is empty, and what it
/// points at is only readable until the call it is passed to returns.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RustFfiHtmlUtf16View {
    pub latin1: *const u8,
    pub utf16: *const u16,
    pub length: usize,
}

impl From<Utf16View<'_>> for RustFfiHtmlUtf16View {
    fn from(view: Utf16View<'_>) -> Self {
        match view {
            Utf16View::Latin1(code_units) => Self {
                latin1: code_units.as_ptr(),
                utf16: std::ptr::null(),
                length: code_units.len(),
            },
            Utf16View::Utf16(code_units) => Self {
                latin1: std::ptr::null(),
                utf16: code_units.as_ptr(),
                length: code_units.len(),
            },
        }
    }
}

#[cfg(test)]
impl RustFfiHtmlUtf16View {
    /// What a test's stand-in for the host was handed, as text.
    pub(crate) fn to_test_string(self) -> String {
        let code_units = if self.length == 0 {
            Vec::new()
        } else if !self.latin1.is_null() {
            Utf16View::Latin1(unsafe { ak::slice_from_raw_parts(self.latin1, self.length) }).to_utf16()
        } else {
            unsafe { ak::slice_from_raw_parts(self.utf16, self.length) }.to_vec()
        };
        String::from_utf16(&code_units).unwrap()
    }
}

/// Whether any code unit is something other than the ASCII whitespace the tree builder ignores for "frameset-ok".
pub fn contains_non_whitespace(view: Utf16View<'_>) -> bool {
    match view {
        Utf16View::Latin1(code_units) => scan::contains_non_whitespace(code_units),
        Utf16View::Utf16(code_units) => scan::contains_non_whitespace(code_units),
    }
}

impl Default for CodeUnits {
    fn default() -> Self {
        Self::Latin1(Vec::new())
    }
}

impl From<Vec<u8>> for CodeUnits {
    fn from(code_units: Vec<u8>) -> Self {
        Self::Latin1(code_units)
    }
}

impl From<Vec<u16>> for CodeUnits {
    fn from(code_units: Vec<u16>) -> Self {
        Self::Utf16(code_units)
    }
}

impl CodeUnits {
    #[inline(always)]
    pub fn len(&self) -> usize {
        match self {
            Self::Latin1(code_units) => code_units.len(),
            Self::Utf16(code_units) => code_units.len(),
        }
    }

    #[inline(always)]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The code unit at `offset`, which must be inside the input.
    #[inline(always)]
    pub fn code_unit_at(&self, offset: usize) -> u16 {
        match self {
            Self::Latin1(code_units) => u16::from(code_units[offset]),
            Self::Utf16(code_units) => code_units[offset],
        }
    }

    #[inline(always)]
    pub fn get(&self, offset: usize) -> Option<u16> {
        match self {
            Self::Latin1(code_units) => code_units.get(offset).copied().map(u16::from),
            Self::Utf16(code_units) => code_units.get(offset).copied(),
        }
    }

    pub fn slice_from(&self, start: usize) -> Utf16View<'_> {
        match self {
            Self::Latin1(code_units) => Utf16View::Latin1(&code_units[start..]),
            Self::Utf16(code_units) => Utf16View::Utf16(&code_units[start..]),
        }
    }

    pub fn slice(&self, range: Range<usize>) -> Utf16View<'_> {
        match self {
            Self::Latin1(code_units) => Utf16View::Latin1(&code_units[range]),
            Self::Utf16(code_units) => Utf16View::Utf16(&code_units[range]),
        }
    }

    pub fn append(&mut self, code_units: Utf16View<'_>) {
        self.insert(self.len(), code_units);
    }

    pub fn insert(&mut self, offset: usize, code_units: Utf16View<'_>) {
        if self.is_empty() {
            *self = match code_units {
                Utf16View::Latin1(code_units) => Self::Latin1(code_units.to_vec()),
                Utf16View::Utf16(code_units) => Self::Utf16(code_units.to_vec()),
            };
            return;
        }
        match (&mut *self, code_units) {
            (Self::Latin1(input), Utf16View::Latin1(code_units)) => {
                input.splice(offset..offset, code_units.iter().copied());
            }
            (Self::Utf16(input), Utf16View::Latin1(code_units)) => {
                input.splice(offset..offset, code_units.iter().copied().map(u16::from));
            }
            (Self::Utf16(input), Utf16View::Utf16(code_units)) => {
                input.splice(offset..offset, code_units.iter().copied());
            }
            (Self::Latin1(input), Utf16View::Utf16(code_units)) => {
                let mut widened = Vec::with_capacity(input.len() + code_units.len());
                widened.extend(input[..offset].iter().copied().map(u16::from));
                widened.extend_from_slice(code_units);
                widened.extend(input[offset..].iter().copied().map(u16::from));
                *self = Self::Utf16(widened);
            }
        }
    }

    pub fn to_utf16(&self) -> Vec<u16> {
        self.slice_from(0).to_utf16()
    }

    pub const fn new() -> Self {
        Self::Latin1(Vec::new())
    }

    pub fn as_slice(&self) -> Utf16View<'_> {
        self.slice_from(0)
    }

    pub fn capacity(&self) -> usize {
        match self {
            Self::Latin1(code_units) => code_units.capacity(),
            Self::Utf16(code_units) => code_units.capacity(),
        }
    }

    /// Empties the string. A one-byte string keeps its buffer.
    pub fn clear(&mut self) {
        match self {
            Self::Latin1(code_units) => code_units.clear(),
            // NB: An empty string is always in the one-byte form, so that what is written to it next stays at one
            //     byte per code unit if it can.
            Self::Utf16(_) => *self = Self::new(),
        }
    }

    /// Takes the string out, and leaves an empty one.
    pub fn take(&mut self) -> Self {
        std::mem::take(self)
    }

    #[inline(always)]
    pub fn push_ascii(&mut self, byte: u8) {
        debug_assert!(byte.is_ascii());
        match self {
            Self::Latin1(code_units) => code_units.push(byte),
            Self::Utf16(code_units) => code_units.push(u16::from(byte)),
        }
    }

    pub fn push_ascii_str(&mut self, string: &str) {
        debug_assert!(string.is_ascii());
        self.append(Utf16View::Latin1(string.as_bytes()));
    }

    /// Appends code units as they are. Unlike `append()`, a string that is empty keeps its buffer.
    pub fn push_slice(&mut self, code_units: Utf16View<'_>) {
        match (&mut *self, code_units) {
            (Self::Latin1(string), Utf16View::Latin1(code_units)) => string.extend_from_slice(code_units),
            (Self::Utf16(string), Utf16View::Utf16(code_units)) => string.extend_from_slice(code_units),
            (Self::Utf16(string), Utf16View::Latin1(code_units)) => {
                string.extend(code_units.iter().copied().map(u16::from));
            }
            // NB: Input stored as UTF-16 is mostly code units that fit in a byte, which keeps this at a byte each
            //     and in the buffer it already has.
            (Self::Latin1(string), Utf16View::Utf16(code_units))
                if code_units.iter().all(|&code_unit| code_unit <= 0xFF) =>
            {
                string.extend(code_units.iter().map(|&code_unit| code_unit as u8));
            }
            (Self::Latin1(string), Utf16View::Utf16(code_units)) => {
                let mut widened = Vec::with_capacity(string.len() + code_units.len());
                widened.extend(string.iter().copied().map(u16::from));
                widened.extend_from_slice(code_units);
                *self = Self::Utf16(widened);
            }
        }
    }

    /// Appends a code point. A surrogate is replaced with U+FFFD REPLACEMENT CHARACTER.
    #[inline(always)]
    pub fn push_code_point(&mut self, code_point: u32) {
        if let Self::Latin1(code_units) = self
            && code_point < 0x100
        {
            code_units.push(code_point as u8);
            return;
        }
        self.push_wide_code_point(code_point);
    }

    fn push_wide_code_point(&mut self, code_point: u32) {
        if let Self::Latin1(code_units) = self {
            *self = Self::Utf16(code_units.iter().copied().map(u16::from).collect());
        }
        let Self::Utf16(code_units) = self else {
            unreachable!();
        };
        match code_point {
            0xD800..=0xDFFF => code_units.push(0xFFFD),
            0..=0xFFFF => code_units.push(code_point as u16),
            _ => {
                let offset = code_point - 0x10000;
                code_units.push(0xD800 | (offset >> 10) as u16);
                code_units.push(0xDC00 | (offset & 0x3FF) as u16);
            }
        }
    }
}

impl PartialEq for CodeUnits {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

impl Eq for CodeUnits {}

impl PartialEq<str> for CodeUnits {
    fn eq(&self, other: &str) -> bool {
        self.as_slice() == *other
    }
}

impl PartialEq<&str> for CodeUnits {
    fn eq(&self, other: &&str) -> bool {
        self.as_slice() == **other
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn code_units(input: &str) -> Vec<u16> {
        input.encode_utf16().collect()
    }

    #[test]
    fn one_byte_input_stays_one_byte() {
        let mut input = CodeUnits::default();
        input.append(Utf16View::Latin1(b"<p>"));
        input.append(Utf16View::Latin1(b"caf\xe9"));
        input.insert(3, Utf16View::Latin1(b"un "));
        assert!(matches!(input, CodeUnits::Latin1(_)));
        assert_eq!(input.to_utf16(), code_units("<p>un caf\u{e9}"));
        assert_eq!(input.code_unit_at(9), 0xe9);
        assert_eq!(input.get(10), None);
    }

    #[test]
    fn wide_code_units_widen_the_input_in_place() {
        let mut input = CodeUnits::default();
        input.append(Utf16View::Latin1(b"<p>caf\xe9</p>"));
        input.insert(7, Utf16View::Utf16(&code_units(" \u{2014} \u{1f600}")));
        assert!(matches!(input, CodeUnits::Utf16(_)));
        assert_eq!(input.to_utf16(), code_units("<p>caf\u{e9} \u{2014} \u{1f600}</p>"));
        input.append(Utf16View::Latin1(b"\xe9"));
        assert_eq!(input.code_unit_at(input.len() - 1), 0xe9);
    }

    #[test]
    fn code_points_are_pushed_at_one_byte_each_until_one_doesnt_fit() {
        let mut string = CodeUnits::new();
        string.push_ascii(b'a');
        string.push_code_point(0xe9);
        assert!(matches!(string, CodeUnits::Latin1(_)));
        string.push_code_point(0x2014);
        string.push_code_point(0x1f600);
        string.push_code_point(0xd83d);
        string.push_ascii_str("ok");
        string.push_slice(Utf16View::Latin1(b"\xe9"));
        string.push_slice(Utf16View::Utf16(&code_units("\u{2014}")));
        assert!(matches!(string, CodeUnits::Utf16(_)));
        assert_eq!(string, "a\u{e9}\u{2014}\u{1f600}\u{fffd}ok\u{e9}\u{2014}");
        string.clear();
        string.push_slice(Utf16View::Latin1(b"a"));
        assert!(matches!(string, CodeUnits::Latin1(_)));
        string.push_slice(Utf16View::Utf16(&code_units("\u{2014}")));
        assert_eq!(string, "a\u{2014}");
    }

    #[test]
    fn strings_compare_by_code_unit_whatever_form_they_are_in() {
        let narrow = CodeUnits::from(b"caf\xe9".to_vec());
        let wide = CodeUnits::from(code_units("caf\u{e9}"));
        assert_eq!(narrow, wide);
        assert_eq!(narrow, "caf\u{e9}");
        assert_ne!(narrow, "cafe");
        assert_ne!(wide, "caf");
    }

    #[test]
    fn empty_input_takes_the_form_of_what_it_is_given() {
        let mut input = CodeUnits::from(Vec::<u16>::new());
        input.append(Utf16View::Latin1(b"abc"));
        assert!(matches!(input, CodeUnits::Latin1(_)));
    }
}
