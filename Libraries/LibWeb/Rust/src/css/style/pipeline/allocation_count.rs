/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The global allocator of this crate's unit test binary: the system allocator, counting what a
//! thread allocates while it runs `count_allocations`, and which of that the engine allocates itself,
//! inside a `StyleSection`. Only the test binary has it; the crate as the browser links it never
//! does.
//!
//! With LIBWEB_TEST_ALLOCATION_BACKTRACES set, each allocation counted prints a backtrace to
//! stderr, for a test that fails to name where its allocations come from.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::OnceLock;

struct CountingAllocator;

#[global_allocator]
static COUNTING_ALLOCATOR: CountingAllocator = CountingAllocator;

thread_local! {
    static COUNTING: Cell<bool> = const { Cell::new(false) };
    static PRINTS_BACKTRACES: Cell<bool> = const { Cell::new(false) };
    static ALLOCATIONS: Cell<u64> = const { Cell::new(0) };
    static STYLE_ALLOCATIONS: Cell<u64> = const { Cell::new(0) };
    static STYLE_SECTION_DEPTH: Cell<u32> = const { Cell::new(0) };
    static LIVE_BYTES: Cell<i64> = const { Cell::new(0) };
}

/// How many times a thread allocated or reallocated.
#[derive(Clone, Copy, Debug)]
pub(super) struct Allocations {
    pub everywhere: u64,
    /// Those inside a `StyleSection`: invalidating, restyling and installing.
    pub in_style: u64,
}

/// Marks the allocations of the calling thread, until it drops, as the engine's own.
pub(super) struct StyleSection;

impl StyleSection {
    pub(super) fn enter() -> Self {
        STYLE_SECTION_DEPTH.with(|depth| depth.set(depth.get() + 1));
        Self
    }
}

impl Drop for StyleSection {
    fn drop(&mut self) {
        STYLE_SECTION_DEPTH.with(|depth| depth.set(depth.get() - 1));
    }
}

/// Notes that the calling thread's live heap grew by `bytes`, or shrank where negative, while it counts.
fn note_live_bytes(bytes: i64) {
    let Ok(true) = COUNTING.try_with(Cell::get) else {
        return;
    };
    LIVE_BYTES.with(|live| live.set(live.get() + bytes));
}

fn note_allocation() {
    // NB: The thread-local keys have no destructors, so they stay readable while a thread exits.
    let Ok(true) = COUNTING.try_with(Cell::get) else {
        return;
    };
    ALLOCATIONS.with(|allocations| allocations.set(allocations.get() + 1));
    if STYLE_SECTION_DEPTH.with(Cell::get) != 0 {
        STYLE_ALLOCATIONS.with(|allocations| allocations.set(allocations.get() + 1));
    }
    if PRINTS_BACKTRACES.with(Cell::get) {
        // NB: Printing a backtrace allocates, so stop counting while it prints.
        COUNTING.with(|counting| counting.set(false));
        eprintln!("allocation:\n{}", std::backtrace::Backtrace::force_capture());
        COUNTING.with(|counting| counting.set(true));
    }
}

// SAFETY: Every call goes to the system allocator as it is.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        note_live_bytes(layout.size() as i64);
        // SAFETY: Guaranteed by the caller.
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        note_allocation();
        note_live_bytes(layout.size() as i64);
        // SAFETY: Guaranteed by the caller.
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        note_live_bytes(-(layout.size() as i64));
        // SAFETY: Guaranteed by the caller.
        unsafe { System.dealloc(pointer, layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        note_allocation();
        note_live_bytes(new_size as i64 - layout.size() as i64);
        // SAFETY: Guaranteed by the caller.
        unsafe { System.realloc(pointer, layout, new_size) }
    }
}

/// Runs `body`, answering what it answers and how many times the calling thread allocated or
/// reallocated meanwhile. What other threads allocate is not counted.
pub(super) fn count_allocations<T>(body: impl FnOnce() -> T) -> (T, Allocations) {
    static PRINTS_BACKTRACES_CACHE: OnceLock<bool> = OnceLock::new();
    let prints_backtraces =
        *PRINTS_BACKTRACES_CACHE.get_or_init(|| std::env::var_os("LIBWEB_TEST_ALLOCATION_BACKTRACES").is_some());
    assert!(!COUNTING.with(Cell::get), "allocations are counted once at a time");
    PRINTS_BACKTRACES.with(|prints| prints.set(prints_backtraces));
    ALLOCATIONS.with(|allocations| allocations.set(0));
    STYLE_ALLOCATIONS.with(|allocations| allocations.set(0));
    COUNTING.with(|counting| counting.set(true));
    let answer = body();
    COUNTING.with(|counting| counting.set(false));
    let allocations = Allocations {
        everywhere: ALLOCATIONS.with(Cell::get),
        in_style: STYLE_ALLOCATIONS.with(Cell::get),
    };
    (answer, allocations)
}

/// Runs `body`, answering what it answers and the bytes the calling thread allocated meanwhile
/// that are still allocated when it returns: what the answer holds, and what `body` left
/// elsewhere, as in a cache. What other threads allocate is not counted.
pub(crate) fn count_live_bytes<T>(body: impl FnOnce() -> T) -> (T, i64) {
    assert!(!COUNTING.with(Cell::get), "allocations are counted once at a time");
    PRINTS_BACKTRACES.with(|prints| prints.set(false));
    LIVE_BYTES.with(|live| live.set(0));
    COUNTING.with(|counting| counting.set(true));
    let answer = body();
    COUNTING.with(|counting| counting.set(false));
    (answer, LIVE_BYTES.with(Cell::get))
}
