/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Counts the heap allocations parsing each document named on the command line takes: how many calls, the most
//! bytes allocated at once, and how many bytes the finished document holds on to. The row count is there to judge
//! how well the parser guesses the size of its result from the size of the source.

use std::alloc::GlobalAlloc;
use std::alloc::Layout;
use std::alloc::System;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering::Relaxed;

struct Counting;

static CALLS: AtomicUsize = AtomicUsize::new(0);
static REALLOCS: AtomicUsize = AtomicUsize::new(0);
static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);

fn grow(size: usize) {
    let live = LIVE.fetch_add(size, Relaxed) + size;
    PEAK.fetch_max(live, Relaxed);
}

// SAFETY: Every request is passed on to the system allocator unchanged.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        CALLS.fetch_add(1, Relaxed);
        grow(layout.size());
        // SAFETY: The caller upholds the contract of alloc.
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        LIVE.fetch_sub(layout.size(), Relaxed);
        // SAFETY: The caller upholds the contract of dealloc.
        unsafe { System.dealloc(pointer, layout) }
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        REALLOCS.fetch_add(1, Relaxed);
        LIVE.fetch_sub(layout.size(), Relaxed);
        grow(new_size);
        // SAFETY: The caller upholds the contract of realloc.
        unsafe { System.realloc(pointer, layout, new_size) }
    }
}

#[global_allocator]
static ALLOCATOR: Counting = Counting;

fn main() {
    let options = libxml_rust::Options {
        preserve_comments: true,
        preserve_cdata: true,
        resolve_html_entity: None,
    };
    println!(
        "{:>10} {:>9} {:>7} {:>8} {:>10} {:>10}  path",
        "bytes", "rows", "allocs", "reallocs", "peak", "retained"
    );
    for path in std::env::args().skip(1) {
        let source = std::fs::read(&path).unwrap();
        let (calls, reallocs, live) = (CALLS.load(Relaxed), REALLOCS.load(Relaxed), LIVE.load(Relaxed));
        PEAK.store(live, Relaxed);
        let document = libxml_rust::parse(&source, options);
        let peak = PEAK.load(Relaxed) - live;
        let retained = LIVE.load(Relaxed) - live;
        let allocs = CALLS.load(Relaxed) - calls;
        let reallocs = REALLOCS.load(Relaxed) - reallocs;
        let rows = document.tape.len();
        drop(document);
        println!(
            "{:>10} {rows:>9} {allocs:>7} {reallocs:>8} {peak:>10} {retained:>10}  {path}",
            source.len()
        );
    }
}
