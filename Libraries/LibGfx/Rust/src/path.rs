/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::ptr::NonNull;

unsafe extern "C" {
    fn ladybird_gfx_path_destroy(path: *mut c_void);
    fn ladybird_gfx_path_equals(a: *const c_void, b: *const c_void) -> bool;
}

/// The sole owner of a heap-allocated `Gfx::Path`, destroying it on drop.
pub struct OwnedPath {
    raw: NonNull<c_void>,
    identity: u64,
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
        static NEXT_IDENTITY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        Self {
            raw: NonNull::new(raw).expect("Gfx::Path pointer must not be null"),
            identity: NEXT_IDENTITY.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
        }
    }

    /// The raw `Gfx::Path` pointer, still owned by this value.
    #[inline]
    pub fn as_raw(&self) -> *mut c_void {
        self.raw.as_ptr()
    }

    /// A process-unique, never-reused identity for this path allocation, so
    /// consumers holding a copied snapshot can recognize an unchanged path
    /// without comparing contents.
    #[inline]
    pub fn identity(&self) -> u64 {
        self.identity
    }
}

impl PartialEq for OwnedPath {
    fn eq(&self, other: &Self) -> bool {
        self.identity == other.identity
            // SAFETY: Both sides own live heap-allocated paths for the duration of the call.
            || unsafe { ladybird_gfx_path_equals(self.raw.as_ptr(), other.raw.as_ptr()) }
    }
}

impl Drop for OwnedPath {
    fn drop(&mut self) {
        // SAFETY: adopt() took sole ownership of the heap-allocated path.
        unsafe { ladybird_gfx_path_destroy(self.raw.as_ptr()) };
    }
}
