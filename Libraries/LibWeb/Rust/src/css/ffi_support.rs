/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared utilities for C++/Rust FFI data.

use std::marker::PhantomData;

use crate::css::css_tokenizer::TokenizerInput;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum StyleNestingParent {
    None,
    Style,
    Scope,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStringView {
    pub data: *const u16,
    pub length: usize,
}

/// Text borrowed for `'a`, as ASCII bytes or UTF-16 code units.
///
/// Rust builds a view only from a borrowed slice, and C++ hands one over under the same contract: exactly one non-empty
/// pointer identifies `length` units that stay readable for `'a`.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiUtf16View<'a> {
    ascii: *const u8,
    utf16: *const u16,
    length: usize,
    units: PhantomData<TokenizerInput<'a>>,
}

impl Default for FfiUtf16View<'_> {
    fn default() -> Self {
        Self {
            ascii: std::ptr::null(),
            utf16: std::ptr::null(),
            length: 0,
            units: PhantomData,
        }
    }
}

impl<'a> FfiUtf16View<'a> {
    pub(crate) fn from_ascii(units: &'a [u8]) -> Self {
        Self {
            ascii: units.as_ptr(),
            length: units.len(),
            ..Self::default()
        }
    }

    pub(crate) fn from_utf16(units: &'a [u16]) -> Self {
        Self {
            utf16: units.as_ptr(),
            length: units.len(),
            ..Self::default()
        }
    }

    pub(crate) fn from_input(input: TokenizerInput<'a>) -> Self {
        match input {
            TokenizerInput::Ascii(units) => Self::from_ascii(units),
            TokenizerInput::Utf16(units) => Self::from_utf16(units),
        }
    }

    pub(crate) fn is_empty(self) -> bool {
        self.length == 0
    }

    /// The viewed units, or none for a view that names both or neither kind of storage.
    pub(crate) fn units(self) -> Option<TokenizerInput<'a>> {
        // SAFETY: The view's contract keeps exactly one non-empty pointer readable for `length` units during `'a`.
        unsafe { TokenizerInput::from_raw_parts(self.ascii, self.utf16, self.length) }
    }

    pub(crate) fn to_utf16(self) -> Option<Vec<u16>> {
        let units = self.units()?;
        let mut output = Vec::with_capacity(units.len());
        units.append_to(&mut output);
        Some(output)
    }
}

pub(crate) fn ascii_lowercase(code_unit: u16) -> u16 {
    if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
        code_unit + u16::from(b'a' - b'A')
    } else {
        code_unit
    }
}
