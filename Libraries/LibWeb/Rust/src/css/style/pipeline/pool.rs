/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The threads style passes run on besides the calling thread. The owner of every document's
//! render state holds one pool for the process and hands it to each document's style engine, so
//! the documents of a process share their style threads, and the heaps those threads warm.

use std::sync::{Arc, Mutex, PoisonError};

/// When a pool's threads start.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum PoolStart {
    /// On a thread of its own, once a pass first asks for them: the passes until they have started
    /// run on the calling thread alone, which spares the first pass of a document starting them.
    InBackground,
    /// On the thread of the first pass asking for them, which then runs on them.
    OnFirstPass,
}

/// The pool of threads style passes run on besides the calling thread, started once a pass
/// first asks for it.
pub(crate) struct StylePool {
    start: PoolStart,
    threads: Mutex<PoolThreads>,
}

enum PoolThreads {
    NotStarted,
    /// Starting `workers` threads, on a thread of their own.
    Starting {
        workers: usize,
    },
    Started(Arc<rayon::ThreadPool>),
}

impl StylePool {
    pub(crate) fn new(start: PoolStart) -> Self {
        Self {
            start,
            threads: Mutex::new(PoolThreads::NotStarted),
        }
    }

    /// The pool of `workers` threads a pass runs on besides the calling thread, where it has
    /// started: none for no workers, and none while the threads start in the background, which
    /// the first such ask starts them for.
    pub(crate) fn workers(self: &Arc<Self>, workers: usize) -> Option<Arc<rayon::ThreadPool>> {
        if workers == 0 {
            return None;
        }
        let mut threads = self.threads.lock().unwrap_or_else(PoisonError::into_inner);
        match &*threads {
            PoolThreads::Started(pool) if pool.current_num_threads() == workers => return Some(pool.clone()),
            PoolThreads::Starting { workers: starting } if *starting == workers => return None,
            _ => {}
        }
        match self.start {
            PoolStart::OnFirstPass => {
                let pool = Arc::new(start_threads(workers));
                *threads = PoolThreads::Started(pool.clone());
                Some(pool)
            }
            PoolStart::InBackground => {
                *threads = PoolThreads::Starting { workers };
                let this = self.clone();
                std::thread::Builder::new()
                    .name("Style start".into())
                    .spawn(move || {
                        let pool = Arc::new(start_threads(workers));
                        let mut threads = this.threads.lock().unwrap_or_else(PoisonError::into_inner);
                        // NB: A pass may have asked for another number of threads meanwhile.
                        if matches!(*threads, PoolThreads::Starting { workers: starting } if starting == workers) {
                            *threads = PoolThreads::Started(pool);
                        }
                    })
                    .expect("the thread starting the engine pool starts");
                None
            }
        }
    }

    /// The threads of the pool, whatever their number, where they have started.
    pub(crate) fn started(&self) -> Option<Arc<rayon::ThreadPool>> {
        match &*self.threads.lock().unwrap_or_else(PoisonError::into_inner) {
            PoolThreads::Started(pool) => Some(pool.clone()),
            PoolThreads::NotStarted | PoolThreads::Starting { .. } => None,
        }
    }
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
