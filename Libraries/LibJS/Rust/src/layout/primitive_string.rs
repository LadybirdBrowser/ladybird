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

/// The bits of PrimitiveString::deferred_kind_and_flags that hold its DeferredKind.
pub const DEFERRED_KIND_MASK: u8 = 0b11;

/// The flag of PrimitiveString::deferred_kind_and_flags that says the string is interned: it is the only interned
/// string with its contents, so two interned strings are equal if and only if they are the same string.
pub const INTERNED_FLAG: u8 = 1 << 7;

/// A string value. Ropes, substrings and inline strings resolve into `utf16_string` on first use; the interpreter only
/// reads strings that are not deferred.
#[repr(C)]
pub struct PrimitiveString {
    pub header: CellHeader,
    /// The DeferredKind in the bits of DEFERRED_KIND_MASK, and INTERNED_FLAG. They share a byte since the cell has
    /// no other room for the flag.
    pub deferred_kind_and_flags: Cell<u8>,
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
