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
}

/// A string value. Ropes and substrings resolve into `utf16_string` on first use; the interpreter only reads strings
/// that are not deferred.
#[repr(C)]
pub struct PrimitiveString {
    pub header: CellHeader,
    pub deferred_kind: Cell<DeferredKind>,
    pub length_in_utf16_code_units: Cell<u32>,
    pub utf16_string: Utf16StringSlot,
}
