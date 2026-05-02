/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Under `cargo test`, fall back to the standard system allocator so that the
// crate's unit tests don't need to link against the C++ runtime.
#![cfg(not(test))]

use std::alloc::{GlobalAlloc, Layout};

unsafe extern "C" {
    fn ladybird_rust_alloc(size: usize, alignment: usize) -> *mut u8;
    fn ladybird_rust_alloc_zeroed(size: usize, alignment: usize) -> *mut u8;
    fn ladybird_rust_dealloc(ptr: *mut u8, alignment: usize);
    fn ladybird_rust_realloc(ptr: *mut u8, old_size: usize, new_size: usize, alignment: usize) -> *mut u8;
}

struct LadybirdAllocator;

#[global_allocator]
static LADYBIRD_ALLOCATOR: LadybirdAllocator = LadybirdAllocator;

unsafe impl GlobalAlloc for LadybirdAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { ladybird_rust_alloc(layout.size(), layout.align()) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        unsafe { ladybird_rust_alloc_zeroed(layout.size(), layout.align()) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { ladybird_rust_dealloc(ptr, layout.align()) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        unsafe { ladybird_rust_realloc(ptr, layout.size(), new_size, layout.align()) }
    }
}
