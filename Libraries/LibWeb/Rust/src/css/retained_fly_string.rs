/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Retained AK::Utf16FlyString types shared by generated FFI headers.
//!
//! Both the style-value and computed-values cbindgen invocations parse this
//! module so the same struct layout is emitted into each header's namespace,
//! the same arrangement `display.rs` uses for `FfiDisplay`.

use crate::css::style_value::retained_list_partial_eq;
/// A retained `AK::Utf16FlyString` with the same one-word representation.
#[repr(C)]
pub struct RetainedUtf16FlyString {
    raw: usize,
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

const _: () = assert!(size_of::<RetainedUtf16FlyString>() == size_of::<usize>());
const _: () = assert!(align_of::<RetainedUtf16FlyString>() == align_of::<usize>());

impl RetainedUtf16FlyString {
    /// The raw one-word representation; fly strings are interned, so equal raw
    /// values mean equal strings.
    pub(crate) fn raw(&self) -> usize {
        self.raw
    }

    pub(crate) fn raw_word(&self) -> &usize {
        &self.raw
    }

    /// The no-string sentinel; holds no reference. No real fly string uses the
    /// zero raw representation.
    pub(crate) fn none() -> Self {
        Self {
            raw: 0,
            _not_send_or_sync: std::marker::PhantomData,
        }
    }

    /// Assumes ownership of one leaked reference to the underlying string data.
    pub(crate) unsafe fn from_leaked_raw(raw: usize) -> Self {
        if raw == 0 {
            return Self::none();
        }
        Self {
            raw,
            _not_send_or_sync: std::marker::PhantomData,
        }
    }
}

impl Clone for RetainedUtf16FlyString {
    fn clone(&self) -> Self {
        if self.raw == 0 {
            return Self::none();
        }
        // SAFETY: Every non-zero value owns one reference to a valid fly string.
        unsafe { ak::reference_utf16_string(self.raw) };
        Self {
            raw: self.raw,
            _not_send_or_sync: std::marker::PhantomData,
        }
    }
}

impl Drop for RetainedUtf16FlyString {
    fn drop(&mut self) {
        if self.raw == 0 {
            return;
        }
        // SAFETY: Every non-zero value owns one reference to a valid fly string.
        unsafe { ak::release_utf16_string_with(self.raw, crate::css::ffi_stats::release_utf16_fly_string) };
    }
}

impl PartialEq for RetainedUtf16FlyString {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}

impl Eq for RetainedUtf16FlyString {}

impl std::fmt::Debug for RetainedUtf16FlyString {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_tuple("RetainedUtf16FlyString")
            .field(&self.raw)
            .finish()
    }
}

/// A Rust-owned array of retained AK::Utf16FlyString values.
#[repr(C)]
pub struct RetainedUtf16FlyStringList {
    pointer: *mut RetainedUtf16FlyString,
    length: usize,
}

impl RetainedUtf16FlyStringList {
    pub(crate) fn from_retained_strings(strings: Vec<RetainedUtf16FlyString>) -> Self {
        let slice = strings.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedUtf16FlyString;
        Self { pointer, length }
    }

    /// Takes ownership of one leaked reference to each string.
    ///
    /// # Safety
    /// `strings` must point to `length` valid leaked string raws.
    pub(crate) unsafe fn from_raw(strings: *const usize, length: usize) -> Self {
        let slice: Box<[RetainedUtf16FlyString]> = (0..length)
            .map(|i| unsafe { RetainedUtf16FlyString::from_leaked_raw(*strings.add(i)) })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedUtf16FlyString;
        Self { pointer, length }
    }

    pub(crate) fn clone_retained(&self) -> Self {
        Self::from_retained_strings(self.as_slice().to_vec())
    }

    pub(crate) fn as_slice(&self) -> &[RetainedUtf16FlyString] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }

    /// The list viewed as raw fly-string words; fly strings are interned, so
    /// equal raws mean equal strings.
    pub(crate) fn raws(&self) -> &[usize] {
        if self.pointer.is_null() {
            return &[];
        }
        // SAFETY: `RetainedUtf16FlyString` is represented by its single raw word.
        unsafe { std::slice::from_raw_parts(self.pointer.cast::<usize>(), self.length) }
    }
}

impl Clone for RetainedUtf16FlyStringList {
    fn clone(&self) -> Self {
        self.clone_retained()
    }
}

impl Drop for RetainedUtf16FlyStringList {
    fn drop(&mut self) {
        if !self.pointer.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.pointer, self.length)) });
        }
    }
}
retained_list_partial_eq!(RetainedUtf16FlyStringList, RetainedUtf16FlyString);
