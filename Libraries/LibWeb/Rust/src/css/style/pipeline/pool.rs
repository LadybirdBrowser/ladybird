/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The threads style passes run on besides the calling thread. The owner of every document's
//! render state holds one pool for the process and hands it to each document's style engine, so
//! the documents of a process share their style threads, and the heaps those threads warm.

use std::sync::{Arc, Mutex, PoisonError};

/// The pool of threads style passes run on besides the calling thread.
pub(crate) enum StylePool {
    /// The threads of the process, started with the owner of its render states, before any page can run script. How
    /// many there are never changes after that. There are none where a pass runs on the calling thread alone.
    OfProcess(Option<Arc<rayon::ThreadPool>>),
    /// The threads of an engine no render state holds, such as a test's, started once a pass first asks for them.
    OfEngine(Mutex<Option<Arc<rayon::ThreadPool>>>),
}

impl StylePool {
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
pub(super) fn threads_from_environment() -> u32 {
    std::env::var("LIBWEB_STYLE_THREADS")
        .ok()
        .and_then(|threads| threads.parse().ok())
        .unwrap_or(0)
}

/// How many threads a pass on `threads` threads runs on besides the calling thread, which is one of them: on the default
/// number for the machine for none.
fn workers_for_threads(threads: u32) -> usize {
    let threads = match threads {
        0 => super::traversal::thread_count(),
        threads => (threads as usize).min(super::traversal::MAX_THREAD_COUNT),
    };
    threads.max(1) - 1
}

fn start_threads(workers: usize) -> rayon::ThreadPool {
    rayon::ThreadPoolBuilder::new()
        .num_threads(workers)
        .thread_name(|index| format!("StyleWorker {index}"))
        .build()
        .expect("the engine thread pool starts")
}

/// What each thread a pass runs on keeps between passes, by thread, the calling thread's last: a slot is made the first
/// time its thread asks for it, so a document, or a fork of its render state, holds only what its threads used.
pub(super) struct PerThread<T>([std::sync::OnceLock<Box<Mutex<T>>>; super::traversal::MAX_THREAD_COUNT + 1]);

impl<T> Default for PerThread<T> {
    fn default() -> Self {
        Self(std::array::from_fn(|_| std::sync::OnceLock::new()))
    }
}

impl<T: Default> PerThread<T> {
    /// What thread `index` keeps, made where it is not yet, or none past the last thread.
    pub(super) fn get(&self, index: usize) -> Option<&Mutex<T>> {
        self.0.get(index).map(|slot| &**slot.get_or_init(Box::default))
    }

    /// What the threads that asked keep.
    pub(super) fn made(&self) -> impl Iterator<Item = &Mutex<T>> {
        self.0.iter().filter_map(|slot| slot.get().map(|made| &**made))
    }

    /// How many threads may keep anything.
    pub(super) fn len(&self) -> usize {
        self.0.len()
    }
}
