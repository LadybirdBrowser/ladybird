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
use std::ffi::c_void;
use std::sync::Arc;

struct FlyStringStorage {
    raw: usize,
}

impl Drop for FlyStringStorage {
    fn drop(&mut self) {
        crate::css::ffi_stats::release_utf16_fly_string(self.raw);
    }
}

/// A retained AK::Utf16FlyString. Rust clones share one storage reference, so
/// copying style values and computed payloads never crosses into C++.
#[repr(C)]
pub struct RetainedUtf16FlyString {
    raw: usize,
    storage: *const c_void,
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
        Self {
            raw: 0,
            storage: std::ptr::null(),
        }
    }

    /// Assumes ownership of one leaked reference to the underlying string data.
    pub(crate) unsafe fn from_leaked_raw(raw: usize) -> Self {
        if raw == 0 {
            return Self::none();
        }
        Self {
            raw,
            storage: Arc::into_raw(Arc::new(FlyStringStorage { raw })).cast(),
        }
    }
}

impl Clone for RetainedUtf16FlyString {
    fn clone(&self) -> Self {
        if self.raw == 0 {
            return Self::none();
        }
        // SAFETY: Every non-zero value owns one Arc strong reference.
        unsafe { Arc::increment_strong_count(self.storage.cast::<FlyStringStorage>()) };
        Self {
            raw: self.raw,
            storage: self.storage,
        }
    }
}

impl Drop for RetainedUtf16FlyString {
    fn drop(&mut self) {
        if self.raw == 0 {
            return;
        }
        // SAFETY: Every non-zero value owns one Arc strong reference.
        unsafe { Arc::decrement_strong_count(self.storage.cast::<FlyStringStorage>()) };
    }
}

impl PartialEq for RetainedUtf16FlyString {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}

/// A Rust-owned array of retained AK::Utf16FlyString values.
#[repr(C)]
pub struct RetainedUtf16FlyStringList {
    pointer: *mut RetainedUtf16FlyString,
    length: usize,
    raw_pointer: *mut usize,
}

impl RetainedUtf16FlyStringList {
    pub(crate) fn from_retained_strings(strings: Vec<RetainedUtf16FlyString>) -> Self {
        let raw_slice = strings
            .iter()
            .map(RetainedUtf16FlyString::raw)
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let raw_pointer = Box::into_raw(raw_slice) as *mut usize;
        let slice = strings.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedUtf16FlyString;
        Self {
            pointer,
            length,
            raw_pointer,
        }
    }

    /// Takes ownership of one leaked reference to each string.
    ///
    /// # Safety
    /// `strings` must point to `length` valid leaked string raws.
    pub(crate) unsafe fn from_raw(strings: *const usize, length: usize) -> Self {
        let slice: Box<[RetainedUtf16FlyString]> = (0..length)
            .map(|i| unsafe { RetainedUtf16FlyString::from_leaked_raw(*strings.add(i)) })
            .collect();
        let raw_slice = slice
            .iter()
            .map(RetainedUtf16FlyString::raw)
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let raw_pointer = Box::into_raw(raw_slice) as *mut usize;
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedUtf16FlyString;
        Self {
            pointer,
            length,
            raw_pointer,
        }
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
        if self.raw_pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.raw_pointer, self.length) }
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
        if !self.raw_pointer.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.raw_pointer, self.length)) });
        }
    }
}
retained_list_partial_eq!(RetainedUtf16FlyStringList, RetainedUtf16FlyString);
