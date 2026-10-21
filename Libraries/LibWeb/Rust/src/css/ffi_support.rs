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

/// An object C++ holds a strong reference to, through a pointer `Rc::into_raw()` gave it, lent for `'a`.
///
/// Only C++ makes one, and a test from a borrowed `Rc`, so it is never null.
#[repr(transparent)]
pub struct RcRef<'a, T> {
    pointer: *const T,
    lent: PhantomData<&'a std::rc::Rc<T>>,
}

#[cfg(test)]
impl<'a, T> RcRef<'a, T> {
    pub(crate) fn new(rc: &'a std::rc::Rc<T>) -> Self {
        Self {
            pointer: std::rc::Rc::as_ptr(rc),
            lent: PhantomData,
        }
    }
}

impl<T> RcRef<'_, T> {
    /// A strong reference of Rust's own to the object.
    pub(crate) fn to_rc(&self) -> std::rc::Rc<T> {
        assert!(!self.pointer.is_null(), "C++ lends a live object");
        // SAFETY: The pointer comes from `Rc::into_raw()`, and the object is live for 'a.
        unsafe {
            std::rc::Rc::increment_strong_count(self.pointer);
            std::rc::Rc::from_raw(self.pointer)
        }
    }
}

/// [`RcRef`] for an object C++ holds through a pointer `Arc::into_raw()` gave it, which only C++ makes.
#[repr(transparent)]
pub struct ArcRef<'a, T> {
    pointer: *const T,
    lent: PhantomData<&'a std::sync::Arc<T>>,
}

impl<T> ArcRef<'_, T> {
    /// A strong reference of Rust's own to the object.
    pub(crate) fn to_arc(&self) -> std::sync::Arc<T> {
        assert!(!self.pointer.is_null(), "C++ lends a live object");
        // SAFETY: The pointer comes from `Arc::into_raw()`, and the object is live while it is lent.
        unsafe {
            std::sync::Arc::increment_strong_count(self.pointer);
            std::sync::Arc::from_raw(self.pointer)
        }
    }
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
