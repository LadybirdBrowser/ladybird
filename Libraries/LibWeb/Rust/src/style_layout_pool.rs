/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The threads style and layout passes run on besides the calling thread. The owner of every document's render state
//! holds one pool for the process and hands it to each document's style engine, so the documents of a process share
//! these threads, and the heaps those threads warm.

use std::sync::{Arc, Mutex, PoisonError};

/// The most threads a pass runs on, however many are asked for.
pub(crate) const MAX_THREAD_COUNT: usize = 16;

/// The most threads a pass runs on where none are asked for. Beyond this many, passes over
/// a thousand elements take no less time, while each thread more faults in pages of its own and
/// keeps a stack and an allocator heap.
const DEFAULT_MAX_THREAD_COUNT: usize = 8;

/// How many threads a pass runs on where none are asked for: three of every four logical
/// processors, leaving the others to the main thread and the rest of the process, and at least one.
pub(crate) fn default_thread_count() -> usize {
    // NB: Each pass asks, and the answer takes system calls and reading files of the control groups.
    static THREAD_COUNT: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    *THREAD_COUNT.get_or_init(|| {
        std::thread::available_parallelism().map_or(1, |count| (count.get() * 3 / 4).clamp(1, DEFAULT_MAX_THREAD_COUNT))
    })
}

/// The pool of threads style and layout passes run on besides the calling thread.
pub(crate) enum StyleLayoutPool {
    /// The threads of the process, started with the owner of its render states, before any page can run script. How
    /// many there are never changes after that. There are none where a pass runs on the calling thread alone.
    OfProcess(Option<Arc<rayon::ThreadPool>>),
    /// The threads of an engine no render state holds, such as a test's, started once a pass first asks for them.
    OfEngine(Mutex<Option<Arc<rayon::ThreadPool>>>),
}

impl StyleLayoutPool {
    /// The threads of the process, which this starts: as many as `LIBWEB_STYLE_THREADS` asks for, or the default number
    /// for the machine where it names no number of them.
    pub(crate) fn of_process() -> Self {
        let workers = workers_for_threads(threads_from_environment());
        Self::OfProcess((workers > 0).then(|| Arc::new(start_threads(workers))))
    }

    /// The threads of an engine, which the first pass to ask for them starts.
    pub(crate) fn of_engine() -> Self {
        Self::OfEngine(Mutex::new(None))
    }

    /// The pool a pass on `threads` threads runs on besides the calling thread, or none for a pass on one thread. The
    /// threads of the process stay as they are, however many a pass asks for.
    pub(crate) fn for_threads(&self, threads: u32) -> Option<Arc<rayon::ThreadPool>> {
        let workers = workers_for_threads(threads);
        if workers == 0 {
            return None;
        }
        match self {
            Self::OfProcess(pool) => pool.clone(),
            Self::OfEngine(pool) => {
                let mut pool = pool.lock().unwrap_or_else(PoisonError::into_inner);
                if let Some(pool) = pool.as_ref().filter(|pool| pool.current_num_threads() == workers) {
                    return Some(pool.clone());
                }
                let started = Arc::new(start_threads(workers));
                *pool = Some(started.clone());
                Some(started)
            }
        }
    }

    /// The threads of the pool, whatever their number, where they have started.
    pub(crate) fn started(&self) -> Option<Arc<rayon::ThreadPool>> {
        match self {
            Self::OfProcess(pool) => pool.clone(),
            Self::OfEngine(pool) => pool.lock().unwrap_or_else(PoisonError::into_inner).clone(),
        }
    }
}

/// How many threads `LIBWEB_STYLE_THREADS` asks passes to run on, or 0 where it names no number of them.
pub(crate) fn threads_from_environment() -> u32 {
    std::env::var("LIBWEB_STYLE_THREADS")
        .ok()
        .and_then(|threads| threads.parse().ok())
        .unwrap_or(0)
}

/// How many threads a pass on `threads` threads runs on besides the calling thread, which is one of them: on the default
/// number for the machine for none.
fn workers_for_threads(threads: u32) -> usize {
    let threads = match threads {
        0 => default_thread_count(),
        threads => (threads as usize).min(MAX_THREAD_COUNT),
    };
    threads.max(1) - 1
}

fn start_threads(workers: usize) -> rayon::ThreadPool {
    rayon::ThreadPoolBuilder::new()
        .num_threads(workers)
        .stack_size(crate::stage_thread::STAGE_THREAD_STACK_SIZE)
        .thread_name(|index| format!("StyleLayout {index}"))
        .build()
        .expect("the style and layout thread pool starts")
}
