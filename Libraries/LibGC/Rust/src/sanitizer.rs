/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What AddressSanitizer and LeakSanitizer need to know about the heap: free cells are poisoned, so that any access to
//! a cell that was swept or never handed out is reported, and blocks are root regions, so that malloc memory that only
//! cells refer to is not reported as leaked. Without the address-sanitizer feature, all of this does nothing.

#[cfg(feature = "address-sanitizer")]
use core::ffi::c_void;

/// cbindgen:ignore
#[cfg(feature = "address-sanitizer")]
mod ffi {
    use core::ffi::c_void;

    unsafe extern "C" {
        pub(super) fn __asan_poison_memory_region(address: *const c_void, size: usize);
        pub(super) fn __asan_unpoison_memory_region(address: *const c_void, size: usize);
        pub(super) fn __lsan_register_root_region(address: *const c_void, size: usize);
        pub(super) fn __lsan_unregister_root_region(address: *const c_void, size: usize);
    }
}

#[cfg(feature = "address-sanitizer")]
use ffi::{
    __asan_poison_memory_region, __asan_unpoison_memory_region, __lsan_register_root_region,
    __lsan_unregister_root_region,
};

#[inline]
pub fn poison(address: usize, size: usize) {
    // SAFETY: Poisoning only changes what AddressSanitizer reports.
    #[cfg(feature = "address-sanitizer")]
    unsafe {
        __asan_poison_memory_region(address as *const c_void, size);
    }
    let _ = (address, size);
}

#[inline]
pub fn unpoison(address: usize, size: usize) {
    // SAFETY: As above.
    #[cfg(feature = "address-sanitizer")]
    unsafe {
        __asan_unpoison_memory_region(address as *const c_void, size);
    }
    let _ = (address, size);
}

pub fn register_root_region(address: usize, size: usize) {
    // SAFETY: The region is committed memory that LeakSanitizer only reads.
    #[cfg(feature = "address-sanitizer")]
    unsafe {
        __lsan_register_root_region(address as *const c_void, size);
    }
    let _ = (address, size);
}

pub fn unregister_root_region(address: usize, size: usize) {
    // SAFETY: The region was registered with register_root_region().
    #[cfg(feature = "address-sanitizer")]
    unsafe {
        __lsan_unregister_root_region(address as *const c_void, size);
    }
    let _ = (address, size);
}
