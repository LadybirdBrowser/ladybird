/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared utilities for C++/Rust FFI data.

use crate::css::css_tokenizer::TokenizerInput;

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiUtf16View {
    pub ascii: *const u8,
    pub utf16: *const u16,
    pub length: usize,
}

impl FfiUtf16View {
    /// # Safety
    /// Exactly one non-empty pointer must identify `length` readable units.
    pub(crate) unsafe fn units<'a>(self) -> Option<TokenizerInput<'a>> {
        unsafe { TokenizerInput::from_raw_parts(self.ascii, self.utf16, self.length) }
    }

    /// # Safety
    /// The view must satisfy [`Self::units`]'s requirements.
    pub(crate) unsafe fn to_utf16(self) -> Option<Vec<u16>> {
        let units = unsafe { self.units()? };
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
