/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::scan;
use std::ops::Range;

/// The code units of a tokenizer's input stream.
///
/// Most documents only have code units below U+0100, and are kept at one byte per code unit. The stream is widened
/// the first time it is given a code unit that doesn't fit. Both forms are indexed by code unit, so offsets into the
/// stream stay the same when that happens.
#[derive(Clone, Debug)]
pub enum Input {
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

/// A borrowed run of input code units.
#[derive(Clone, Copy, Debug)]
pub enum InputSlice<'a> {
    Latin1(&'a [u8]),
    Utf16(&'a [u16]),
}

impl Default for Input {
    fn default() -> Self {
        Self::Latin1(Vec::new())
    }
}

impl From<Vec<u8>> for Input {
    fn from(code_units: Vec<u8>) -> Self {
        Self::Latin1(code_units)
    }
}

impl From<Vec<u16>> for Input {
    fn from(code_units: Vec<u16>) -> Self {
        Self::Utf16(code_units)
    }
}

impl Input {
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

    pub fn slice_from(&self, start: usize) -> InputSlice<'_> {
        match self {
            Self::Latin1(code_units) => InputSlice::Latin1(&code_units[start..]),
            Self::Utf16(code_units) => InputSlice::Utf16(&code_units[start..]),
        }
    }

    pub fn slice(&self, range: Range<usize>) -> InputSlice<'_> {
        match self {
            Self::Latin1(code_units) => InputSlice::Latin1(&code_units[range]),
            Self::Utf16(code_units) => InputSlice::Utf16(&code_units[range]),
        }
    }

    pub fn append(&mut self, code_units: InputSlice<'_>) {
        self.insert(self.len(), code_units);
    }

    pub fn insert(&mut self, offset: usize, code_units: InputSlice<'_>) {
        if self.is_empty() {
            *self = code_units.to_input();
            return;
        }
        match (&mut *self, code_units) {
            (Self::Latin1(input), InputSlice::Latin1(code_units)) => {
                input.splice(offset..offset, code_units.iter().copied());
            }
            (Self::Utf16(input), InputSlice::Latin1(code_units)) => {
                input.splice(offset..offset, code_units.iter().copied().map(u16::from));
            }
            (Self::Utf16(input), InputSlice::Utf16(code_units)) => {
                input.splice(offset..offset, code_units.iter().copied());
            }
            (Self::Latin1(input), InputSlice::Utf16(code_units)) => {
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
}

impl InputSlice<'_> {
    pub fn len(&self) -> usize {
        match self {
            Self::Latin1(code_units) => code_units.len(),
            Self::Utf16(code_units) => code_units.len(),
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn to_utf16(&self) -> Vec<u16> {
        let mut code_units = Vec::with_capacity(self.len());
        self.append_to(&mut code_units);
        code_units
    }

    pub fn contains_non_whitespace(&self) -> bool {
        match self {
            Self::Latin1(code_units) => scan::contains_non_whitespace(code_units),
            Self::Utf16(code_units) => scan::contains_non_whitespace(code_units),
        }
    }

    pub fn append_to(&self, output: &mut Vec<u16>) {
        match self {
            Self::Latin1(code_units) => output.extend(code_units.iter().copied().map(u16::from)),
            Self::Utf16(code_units) => output.extend_from_slice(code_units),
        }
    }

    fn to_input(self) -> Input {
        match self {
            Self::Latin1(code_units) => Input::Latin1(code_units.to_vec()),
            Self::Utf16(code_units) => Input::Utf16(code_units.to_vec()),
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
    fn one_byte_input_stays_one_byte() {
        let mut input = Input::default();
        input.append(InputSlice::Latin1(b"<p>"));
        input.append(InputSlice::Latin1(b"caf\xe9"));
        input.insert(3, InputSlice::Latin1(b"un "));
        assert!(matches!(input, Input::Latin1(_)));
        assert_eq!(input.to_utf16(), code_units("<p>un caf\u{e9}"));
        assert_eq!(input.code_unit_at(9), 0xe9);
        assert_eq!(input.get(10), None);
    }

    #[test]
    fn wide_code_units_widen_the_input_in_place() {
        let mut input = Input::default();
        input.append(InputSlice::Latin1(b"<p>caf\xe9</p>"));
        input.insert(7, InputSlice::Utf16(&code_units(" \u{2014} \u{1f600}")));
        assert!(matches!(input, Input::Utf16(_)));
        assert_eq!(input.to_utf16(), code_units("<p>caf\u{e9} \u{2014} \u{1f600}</p>"));
        input.append(InputSlice::Latin1(b"\xe9"));
        assert_eq!(input.code_unit_at(input.len() - 1), 0xe9);
    }

    #[test]
    fn empty_input_takes_the_form_of_what_it_is_given() {
        let mut input = Input::from(Vec::<u16>::new());
        input.append(InputSlice::Latin1(b"abc"));
        assert!(matches!(input, Input::Latin1(_)));
    }
}
