/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The system backend's compatibility tests apply to Unix. Windows browser builds use mimalloc.
#![cfg(any(not(test), unix))]

use std::alloc::GlobalAlloc;
use std::alloc::Layout;

// Match AK's allocator so buffers transferred to C++ remain compatible with ak_kfree.
const USE_SYSTEM_ALLOCATOR: bool = cfg!(test) || option_env!("LADYBIRD_RUST_SYSTEM_ALLOCATOR").is_some();

// These are imports from the C allocators, not part of any crate's public C++ interface.
/// cbindgen:ignore
mod ffi {
    unsafe extern "C" {
        pub(super) fn mi_malloc_aligned(size: usize, alignment: usize) -> *mut u8;
        pub(super) fn mi_zalloc_aligned(size: usize, alignment: usize) -> *mut u8;
        pub(super) fn mi_free(ptr: *mut u8);
        pub(super) fn mi_realloc_aligned(ptr: *mut u8, new_size: usize, alignment: usize) -> *mut u8;
        pub(super) fn malloc(size: usize) -> *mut u8;
        pub(super) fn calloc(count: usize, size: usize) -> *mut u8;
        pub(super) fn free(ptr: *mut u8);
        pub(super) fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
        pub(super) fn posix_memalign(ptr: *mut *mut u8, alignment: usize, size: usize) -> std::ffi::c_int;
    }
}

use ffi::{
    calloc, free, malloc, mi_free, mi_malloc_aligned, mi_realloc_aligned, mi_zalloc_aligned, posix_memalign, realloc,
};

struct LadybirdAllocator;

// Unit tests use the harness allocator, and exercise the system backend explicitly below.
#[cfg(not(test))]
#[global_allocator]
static LADYBIRD_ALLOCATOR: LadybirdAllocator = LadybirdAllocator;

unsafe impl GlobalAlloc for LadybirdAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if !USE_SYSTEM_ALLOCATOR {
            return unsafe { mi_malloc_aligned(layout.size(), layout.align()) };
        }
        if layout.align() <= align_of::<usize>() {
            return unsafe { malloc(layout.size()) };
        }
        let mut pointer = std::ptr::null_mut();
        if unsafe { posix_memalign(&raw mut pointer, layout.align(), layout.size()) } != 0 {
            return std::ptr::null_mut();
        }
        pointer
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if !USE_SYSTEM_ALLOCATOR {
            return unsafe { mi_zalloc_aligned(layout.size(), layout.align()) };
        }
        if layout.align() <= align_of::<usize>() {
            return unsafe { calloc(1, layout.size()) };
        }
        let pointer = unsafe { self.alloc(layout) };
        if !pointer.is_null() {
            unsafe { pointer.write_bytes(0, layout.size()) };
        }
        pointer
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        if USE_SYSTEM_ALLOCATOR {
            unsafe { free(ptr) };
        } else {
            unsafe { mi_free(ptr) };
        }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if !USE_SYSTEM_ALLOCATOR {
            return unsafe { mi_realloc_aligned(ptr, new_size, layout.align()) };
        }
        if layout.align() <= align_of::<usize>() {
            return unsafe { realloc(ptr, new_size) };
        }
        // GlobalAlloc requires new_size to form a valid layout with the original alignment.
        let new_layout = unsafe { Layout::from_size_align_unchecked(new_size, layout.align()) };
        let new_pointer = unsafe { self.alloc(new_layout) };
        if !new_pointer.is_null() {
            unsafe {
                ptr.copy_to_nonoverlapping(new_pointer, layout.size().min(new_size));
                free(ptr);
            }
        }
        new_pointer
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocator_preserves_alignment_zeroing_and_contents_across_reallocation() {
        let allocator = LadybirdAllocator;
        for alignment in [1, 2, 8, 16, 64, 4096] {
            let initial = Layout::from_size_align(31, alignment).unwrap();
            let pointer = unsafe { allocator.alloc_zeroed(initial) };
            assert!(!pointer.is_null());
            assert_eq!(pointer as usize % alignment, 0);
            assert!(
                unsafe { std::slice::from_raw_parts(pointer, 31) }
                    .iter()
                    .all(|byte| *byte == 0)
            );
            unsafe { pointer.write_bytes(0x5a, 31) };
            let grown = unsafe { allocator.realloc(pointer, initial, 127) };
            assert!(!grown.is_null());
            assert_eq!(grown as usize % alignment, 0);
            assert!(
                unsafe { std::slice::from_raw_parts(grown, 31) }
                    .iter()
                    .all(|byte| *byte == 0x5a)
            );
            let grown_layout = Layout::from_size_align(127, alignment).unwrap();
            let shrunk = unsafe { allocator.realloc(grown, grown_layout, 7) };
            assert!(!shrunk.is_null());
            assert_eq!(shrunk as usize % alignment, 0);
            assert!(
                unsafe { std::slice::from_raw_parts(shrunk, 7) }
                    .iter()
                    .all(|byte| *byte == 0x5a)
            );
            // AK uses the C allocator's free without carrying Rust Layout metadata.
            unsafe { free(shrunk) };
        }
    }

    #[test]
    fn allocator_can_release_worker_allocations_on_the_receiving_thread() {
        let layout = Layout::from_size_align(257, 64).unwrap();
        let address = std::thread::spawn(move || {
            let pointer = unsafe { LadybirdAllocator.alloc(layout) };
            assert!(!pointer.is_null());
            unsafe { pointer.write_bytes(0x7b, layout.size()) };
            pointer as usize
        })
        .join()
        .unwrap();
        let pointer = address as *mut u8;
        assert!(
            unsafe { std::slice::from_raw_parts(pointer, layout.size()) }
                .iter()
                .all(|byte| *byte == 0x7b)
        );
        unsafe { LadybirdAllocator.dealloc(pointer, layout) };
    }
}
