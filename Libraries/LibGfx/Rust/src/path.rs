/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

unsafe extern "C" {
    fn ladybird_gfx_path_destroy(path: *mut c_void);
}

/// The sole owner of a heap-allocated `Gfx::Path`, destroying it on drop.
pub struct OwnedPath {
    raw: NonNull<c_void>,
}

impl OwnedPath {
    /// Assumes sole ownership of a heap-allocated `Gfx::Path`.
    ///
    /// # Safety
    ///
    /// `raw` must be the only pointer through which the heap-allocated
    /// `Gfx::Path` is owned or destroyed.
    #[inline]
    pub unsafe fn adopt(raw: *mut c_void) -> Self {
        Self {
            raw: NonNull::new(raw).expect("Gfx::Path pointer must not be null"),
        }
    }

    /// The raw `Gfx::Path` pointer, still owned by this value.
    #[inline]
    pub fn as_raw(&self) -> *mut c_void {
        self.raw.as_ptr()
    }
}

impl Drop for OwnedPath {
    fn drop(&mut self) {
        // SAFETY: adopt() took sole ownership of the heap-allocated path.
        unsafe { ladybird_gfx_path_destroy(self.raw.as_ptr()) };
    }
}
