/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared plumbing for the C++/Rust FFI boundary: retained C++ object pointers, the call-scope
//! lifetime marker that pins borrowed DOM data, and borrowed DOM string views.

use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RetainedCxxPointer(Option<NonNull<c_void>>);

impl RetainedCxxPointer {
    pub(crate) fn new(pointer: *const c_void) -> Self {
        Self(NonNull::new(pointer.cast_mut()))
    }

    pub(crate) fn as_ptr(self) -> *const c_void {
        self.0.map_or(std::ptr::null(), |pointer| pointer.as_ptr().cast_const())
    }
}

/// Zero-sized marker whose borrow scopes every pointer C++ lends to Rust for the duration of one
/// synchronous FFI call. Types holding `PhantomData<&'a FfiCallScope>` cannot outlive the call.
pub(crate) struct FfiCallScope;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDomStringView {
    pub data: *const c_void,
    pub length: usize,
    pub is_ascii: bool,
}

#[derive(Clone, Copy)]
pub(crate) struct DomStringView<'a> {
    view: FfiDomStringView,
    marker: PhantomData<&'a FfiCallScope>,
}

impl DomStringView<'_> {
    /// The caller vouches that `view` borrows storage which stays valid for the current FFI call;
    /// the lifetime parameter ties the wrapper to that call scope.
    pub(crate) fn new(view: FfiDomStringView) -> Self {
        Self {
            view,
            marker: PhantomData,
        }
    }

    pub(crate) fn len(self) -> usize {
        self.view.length
    }

    pub(crate) fn code_unit_at(self, index: usize) -> u16 {
        assert!(index < self.len());
        assert!(!self.view.data.is_null());
        if self.view.is_ascii {
            // SAFETY: C++ guarantees that an ASCII view points at `length` bytes.
            return u16::from(unsafe { *(self.view.data.cast::<u8>().add(index)) });
        }
        // SAFETY: C++ guarantees that a UTF-16 view points at `length` aligned code units.
        unsafe { *(self.view.data.cast::<u16>().add(index)) }
    }
}

pub(crate) fn ascii_lowercase(code_unit: u16) -> u16 {
    if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
        code_unit + u16::from(b'a' - b'A')
    } else {
        code_unit
    }
}

pub(crate) fn utf16_equals_ignoring_ascii_case(first: DomStringView<'_>, second: &[u16]) -> bool {
    first.len() == second.len()
        && second
            .iter()
            .enumerate()
            .all(|(index, &second)| ascii_lowercase(first.code_unit_at(index)) == ascii_lowercase(second))
}

pub(crate) fn utf16_equals(first: DomStringView<'_>, second: &[u16]) -> bool {
    first.len() == second.len()
        && second
            .iter()
            .enumerate()
            .all(|(index, &second)| first.code_unit_at(index) == second)
}
