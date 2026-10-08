/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::CellHeader;
use crate::layout_forward::Utf16StringSlot;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DeferredKind {
    None,
    Rope,
    Substring,
    /// The ASCII characters are stored in the string cell itself, and only copied into `utf16_string` when something
    /// needs a string of its own.
    Inline,
}

/// A string value. Ropes, substrings and inline strings resolve into `utf16_string` on first use; the interpreter only
/// reads strings that are not deferred.
#[repr(C)]
pub struct PrimitiveString {
    pub header: CellHeader,
    pub deferred_kind: Cell<DeferredKind>,
    pub length_in_utf16_code_units: Cell<u32>,
    pub utf16_string: Utf16StringSlot,
}

/// The most ASCII characters an inline string holds.
pub const INLINE_STRING_CAPACITY: usize = 32;

/// OPTIMIZATION: A string of a few ASCII characters, which are stored in the cell rather than in storage of their own
///               that would have to be allocated and freed.
#[repr(C)]
pub struct InlineString {
    pub base: PrimitiveString,
    pub characters: [u8; INLINE_STRING_CAPACITY],
}
