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

use crate::css::style_value::{retained_list_drop, retained_list_partial_eq};

unsafe extern "C" {
    fn ladybird_utf16_fly_string_unref(raw: usize);
    fn ladybird_utf16_fly_string_ref(raw: usize);
}

/// A retained AK::Utf16FlyString, stored as its one-word raw representation. Owns one reference
/// to the underlying string data unless it is a short string, which needs none; the C++ bridge
/// handles both cases.
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedUtf16FlyString {
    raw: usize,
}

impl RetainedUtf16FlyString {
    /// The raw one-word representation; fly strings are interned, so equal raw
    /// values mean equal strings.
    pub(crate) fn raw(&self) -> usize {
        self.raw
    }

    /// The no-string sentinel; holds no reference. No real fly string uses the
    /// zero raw representation.
    pub(crate) fn none() -> Self {
        Self { raw: 0 }
    }

    /// Assumes ownership of one leaked reference to the underlying string data.
    pub(crate) unsafe fn from_leaked_raw(raw: usize) -> Self {
        Self { raw }
    }

    /// Retains a new reference to the underlying string data.
    ///
    /// # Safety
    /// `raw` must be the raw representation of a live fly string.
    pub(crate) unsafe fn from_borrowed_raw(raw: usize) -> Self {
        crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StringRetainReleaseCallback);
        unsafe { ladybird_utf16_fly_string_ref(raw) };
        Self { raw }
    }
}

impl Clone for RetainedUtf16FlyString {
    fn clone(&self) -> Self {
        if self.raw == 0 {
            return Self::none();
        }
        // SAFETY: A non-zero raw is a live fly string this value retains.
        unsafe { Self::from_borrowed_raw(self.raw) }
    }
}

impl Drop for RetainedUtf16FlyString {
    fn drop(&mut self) {
        if self.raw == 0 {
            return;
        }
        crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StringRetainReleaseCallback);
        unsafe { ladybird_utf16_fly_string_unref(self.raw) };
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
            .map(|i| RetainedUtf16FlyString {
                raw: unsafe { *strings.add(i) },
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedUtf16FlyString;
        Self { pointer, length }
    }

    pub(crate) fn clone_retained(&self) -> Self {
        Self::from_retained_strings(
            self.as_slice()
                .iter()
                .map(|string| unsafe { RetainedUtf16FlyString::from_borrowed_raw(string.raw()) })
                .collect(),
        )
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
        // SAFETY: RetainedUtf16FlyString is a repr(C) struct of one usize, so
        // a slice of it has the layout of a usize slice.
        unsafe { std::slice::from_raw_parts(self.pointer.cast::<usize>(), self.length) }
    }
}

impl Clone for RetainedUtf16FlyStringList {
    fn clone(&self) -> Self {
        self.clone_retained()
    }
}

retained_list_drop!(RetainedUtf16FlyStringList);
retained_list_partial_eq!(RetainedUtf16FlyStringList, RetainedUtf16FlyString);
