/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Standalone Rust tests use the test harness allocator without linking the C++ runtime.
#![cfg(not(test))]

use std::alloc::GlobalAlloc;
use std::alloc::Layout;

// Keep allocator selection in AK, including overrides used for memory profiling.
/// cbindgen:ignore
mod ffi {
    unsafe extern "C" {
        pub(super) fn ladybird_alloc(size: usize, alignment: usize) -> *mut u8;
        pub(super) fn ladybird_alloc_zeroed(size: usize, alignment: usize) -> *mut u8;
        pub(super) fn ladybird_realloc(pointer: *mut u8, old_size: usize, new_size: usize, alignment: usize)
        -> *mut u8;
        pub(super) fn ladybird_dealloc(pointer: *mut u8, alignment: usize);
    }
}

use ffi::{ladybird_alloc, ladybird_alloc_zeroed, ladybird_dealloc, ladybird_realloc};

struct LadybirdAllocator;

#[global_allocator]
static LADYBIRD_ALLOCATOR: LadybirdAllocator = LadybirdAllocator;

unsafe impl GlobalAlloc for LadybirdAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { ladybird_alloc(layout.size(), layout.align()) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        unsafe { ladybird_alloc_zeroed(layout.size(), layout.align()) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { ladybird_dealloc(pointer, layout.align()) };
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        unsafe { ladybird_realloc(pointer, layout.size(), new_size, layout.align()) }
    }
}
