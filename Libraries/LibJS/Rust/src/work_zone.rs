/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The memory for the work data of a parse job: the first parse of a source, or the compile of a function, which parses
//! it again. A thread that parses has one zone, and each job on the thread puts all of its work data in it. What a job
//! keeps is copied out of the zone before the job ends, and the zone is empty again.
//!
//! The zone keeps its memory when a job ends, for the next job on the thread, so jobs take no memory from the system
//! and give none back. An outsized job is the one exception: it gives back what is past the size that the zone keeps.
//! All of the memory goes back when the thread goes idle (see `release_idle_memory()`).

use std::cell::Cell;

use ak::Zone;

/// The most bytes that the work data of one job can take. It is address space, which costs nothing before it is
/// used, and it is far above what the largest source takes.
const LIMIT: usize = 1 << 34;

/// The most bytes of memory that the zone keeps for the next job, about what a tree of 2^16 nodes takes with the
/// data of scope analysis. Nearly all jobs need less.
const MAXIMUM_SPARE_SIZE: usize = 2 * 1024 * 1024;

thread_local! {
    /// The zone of this thread, while no job has it.
    static SPARE_ZONE: Cell<Option<Zone>> = const { Cell::new(None) };
}

/// Runs `job` with the zone of the thread for its work data. Nothing that `job` returns can refer to the zone.
pub(crate) fn with_work_zone<R>(job: impl for<'zone> FnOnce(&'zone Zone) -> R) -> R {
    let mut zone = SPARE_ZONE.take().unwrap_or_else(|| {
        ak::add_idle_thread_memory_release(release_idle_memory);
        Zone::new(LIMIT, c"JS parse work")
    });
    let mut size = 0;
    let result = zone.scope(|zone| {
        let result = job(zone);
        size = zone.used();
        result
    });
    // NB: Give back the memory of an outsized job rather than keep it until the thread goes idle.
    if size > MAXIMUM_SPARE_SIZE {
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
