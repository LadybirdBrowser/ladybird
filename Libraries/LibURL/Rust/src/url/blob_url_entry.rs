/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::fmt;
use std::ptr::NonNull;

unsafe extern "C" {
    fn url_rust_blob_url_entry_ref(entry: *const c_void);
    fn url_rust_blob_url_entry_unref(entry: *const c_void);
}

// https://w3c.github.io/FileAPI/#blob-url-entry
pub struct BlobUrlEntry(NonNull<c_void>);

impl BlobUrlEntry {
    /// # Safety
    /// `entry` must be null or the blob URL entry of a URL of LibURL's C++ code, borrowed for the duration of the call.
    pub unsafe fn from_borrowed(entry: *const c_void) -> Option<Self> {
        let entry = NonNull::new(entry.cast_mut())?;
        // SAFETY: The caller passes a live entry, which this keeps a reference to.
        unsafe { url_rust_blob_url_entry_ref(entry.as_ptr()) };
        Some(Self(entry))
    }

    pub fn as_ptr(&self) -> *const c_void {
        self.0.as_ptr()
    }
}

impl Clone for BlobUrlEntry {
    fn clone(&self) -> Self {
        // SAFETY: This keeps a reference to the entry.
        unsafe { url_rust_blob_url_entry_ref(self.as_ptr()) };
        Self(self.0)
    }
}

impl Drop for BlobUrlEntry {
    fn drop(&mut self) {
        // SAFETY: This keeps a reference to the entry, which it lets go of.
        unsafe { url_rust_blob_url_entry_unref(self.as_ptr()) };
    }
}

impl fmt::Debug for BlobUrlEntry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_tuple("BlobUrlEntry").field(&self.0).finish()
    }
}
