/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory for the work data of a parse. A thread that parses has one zone, and each parse on the thread puts all of
//! its work data in it. What a parse keeps is copied out of the zone before the parse ends, and the zone is empty again.
//!
//! The zone keeps its memory when a parse ends, for the next parse on the thread, so parses take no memory from the
//! system and give none back. An outsized parse is the one exception: it gives back what is past the size that the
//! zone keeps. All of the memory goes back when the thread goes idle (see `release_idle_memory()`).

use std::cell::Cell;

use ak::Zone;

/// The most bytes that the work data of one parse can take. It is address space, which costs nothing before it is
/// used, and it is far above what the largest source takes.
const LIMIT: usize = 1 << 34;

/// The most bytes of memory that the zone keeps for the next parse, about what a tree of 2^16 nodes takes. Nearly all
/// parses need less.
const MAXIMUM_SPARE_SIZE: usize = 1024 * 1024;

thread_local! {
    /// The zone of this thread, while no parse has it.
    static SPARE_ZONE: Cell<Option<Zone>> = const { Cell::new(None) };
}

/// Runs `job` with the zone of the thread for its work data. Nothing that `job` returns can refer to the zone.
pub(crate) fn with_work_zone<R>(job: impl for<'zone> FnOnce(&'zone Zone) -> R) -> R {
    let mut zone = SPARE_ZONE.take().unwrap_or_else(|| {
        ak::set_idle_thread_memory_release(release_idle_memory);
        Zone::new(LIMIT, c"JS parse work")
    });
    let mut size = 0;
    let result = zone.scope(|zone| {
        let result = job(zone);
        size = zone.used();
        result
    });
    // NB: Give back the memory of an outsized parse rather than keep it until the thread goes idle.
    if size > MAXIMUM_SPARE_SIZE {
        zone.trim(MAXIMUM_SPARE_SIZE);
    }
    SPARE_ZONE.set(Some(zone));
    result
}

/// Gives the memory of the zone of this thread back to the system. A thread pool calls this on a thread that goes idle
/// (see `ak::set_idle_thread_memory_release()`).
pub(crate) extern "C" fn release_idle_memory() {
    drop(SPARE_ZONE.take());
}
