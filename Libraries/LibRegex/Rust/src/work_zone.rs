/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory for the work data of a regex compile: the parse of the pattern and its compile to a program. A thread
//! that compiles regexes has one zone, and each compile on the thread puts all of its work data in it. What a compile
//! keeps is copied out of the zone before the compile ends, and the zone is empty again.
//!
//! The zone keeps its memory when a compile ends, for the next compile on the thread, so compiles take no memory from
//! the system and give none back. An outsized compile is the one exception: it gives back what is past the size that
//! the zone keeps. All of the memory goes back when the thread goes idle (see `release_idle_memory()`).

use std::cell::Cell;

use ak::Zone;

/// The most bytes that the work data of one compile can take. It is address space, which costs nothing before it is
/// used, and it is far above what the largest pattern takes.
const LIMIT: usize = 1 << 34;

/// The most bytes of memory that the zone keeps for the next compile. Nearly all patterns need far less.
const MAXIMUM_SPARE_SIZE: usize = 1024 * 1024;

thread_local! {
    /// The zone of this thread, while no compile has it.
    static SPARE_ZONE: Cell<Option<Zone>> = const { Cell::new(None) };
}

/// Runs `job` with the zone of the thread for its work data. Nothing that `job` returns can refer to the zone.
pub(crate) fn with_work_zone<R>(job: impl for<'zone> FnOnce(&'zone Zone) -> R) -> R {
    let mut zone = SPARE_ZONE.take().unwrap_or_else(|| {
        ak::add_idle_thread_memory_release(release_idle_memory);
        Zone::new(LIMIT, c"Regex work")
    });
    let result = zone.scope(job);
    // NB: Give back the memory of an outsized compile rather than keep it until the thread goes idle.
    if zone.memory() > MAXIMUM_SPARE_SIZE {
        zone.trim(MAXIMUM_SPARE_SIZE);
    }
    SPARE_ZONE.set(Some(zone));
    result
}

/// Gives the memory of the zone of this thread back to the system. A thread pool calls this on a thread that goes idle
/// (see `ak::add_idle_thread_memory_release()`).
extern "C" fn release_idle_memory() {
    drop(SPARE_ZONE.take());
}
