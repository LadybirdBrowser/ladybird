/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// Bytes of the other side of the C ABI, which live for as long as that side says.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiBytes {
    pub data: *const u8,
    pub length: usize,
}

impl FfiBytes {
    pub(crate) fn from_slice(bytes: &[u8]) -> Self {
        Self {
            data: bytes.as_ptr(),
            length: bytes.len(),
        }
    }

    /// # Safety
    /// `data` points to `length` bytes that live for `'a`, or `length` is zero.
    pub(crate) unsafe fn as_slice<'a>(self) -> &'a [u8] {
        // SAFETY: The caller says so.
        unsafe { ak::slice_from_raw_parts(self.data, self.length) }
    }

    /// # Safety
    /// As for `as_slice()`.
    pub(crate) unsafe fn to_string_lossy(self) -> String {
        // SAFETY: The caller says so.
        String::from_utf8_lossy(unsafe { self.as_slice() }).into_owned()
    }
}
