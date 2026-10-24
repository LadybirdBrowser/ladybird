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

/// [`RcRef`] for an object C++ holds through a pointer `Arc::into_raw()` gave it.
#[repr(transparent)]
pub struct ArcRef<'a, T> {
    pointer: *const T,
    lent: PhantomData<&'a std::sync::Arc<T>>,
}

#[cfg(test)]
impl<'a, T> ArcRef<'a, T> {
    pub(crate) fn new(arc: &'a std::sync::Arc<T>) -> Self {
        Self {
            pointer: std::sync::Arc::as_ptr(arc),
            lent: PhantomData,
        }
    }
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

/// A strong reference C++ hands back, through a pointer `Rc::into_raw()` gave it, or null for none.
///
/// Only C++ makes one, so the reference is Rust's own once it is handed over, and dropping one releases it.
#[repr(transparent)]
pub struct OwnedRc<T> {
    pointer: *const T,
    owned: PhantomData<std::rc::Rc<T>>,
}

impl<T> OwnedRc<T> {
    fn take(&mut self) -> Option<std::rc::Rc<T>> {
        let pointer = std::mem::replace(&mut self.pointer, std::ptr::null());
        // SAFETY: A non-null pointer comes from `Rc::into_raw()`, and its reference is this handle's own.
        (!pointer.is_null()).then(|| unsafe { std::rc::Rc::from_raw(pointer) })
    }
}

impl<T> Drop for OwnedRc<T> {
    fn drop(&mut self) {
        drop(self.take());
    }
}

/// [`OwnedRc`] for a reference C++ hands back through a pointer `Arc::into_raw()` gave it.
#[repr(transparent)]
pub struct OwnedArc<T> {
    pointer: *const T,
    owned: PhantomData<std::sync::Arc<T>>,
}

impl<T> OwnedArc<T> {
    fn take(&mut self) -> Option<std::sync::Arc<T>> {
        let pointer = std::mem::replace(&mut self.pointer, std::ptr::null());
        // SAFETY: A non-null pointer comes from `Arc::into_raw()`, and its reference is this handle's own.
        (!pointer.is_null()).then(|| unsafe { std::sync::Arc::from_raw(pointer) })
    }

    /// The reference, which must not be null.
    pub(crate) fn into_arc(mut self) -> std::sync::Arc<T> {
        self.take().expect("C++ hands over a live object")
    }

    /// The reference, or none where C++ handed over null.
    pub(crate) fn into_option(mut self) -> Option<std::sync::Arc<T>> {
        self.take()
    }
}

impl<T> Drop for OwnedArc<T> {
    fn drop(&mut self) {
        drop(self.take());
    }
}

/// An object C++ hands back, through a pointer `Box::into_raw()` gave it, or null for none.
///
/// Only C++ makes one, so the object is Rust's own once it is handed over, and dropping one drops it.
#[repr(transparent)]
pub struct OwnedBox<T> {
    pointer: *mut T,
    owned: PhantomData<Box<T>>,
}

impl<T> OwnedBox<T> {
    fn take(&mut self) -> Option<Box<T>> {
        let pointer = std::mem::replace(&mut self.pointer, std::ptr::null_mut());
        // SAFETY: A non-null pointer comes from `Box::into_raw()`, and its object is this handle's own.
        (!pointer.is_null()).then(|| unsafe { Box::from_raw(pointer) })
    }
}

impl<T> Drop for OwnedBox<T> {
    fn drop(&mut self) {
        drop(self.take());
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
