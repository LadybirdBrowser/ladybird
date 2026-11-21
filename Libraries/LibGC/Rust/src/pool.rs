/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The threads that help the thread that owns a heap mark it. They live as long as the process, and wait for the next
//! collection in between, so a collection does not pay for creating them.

use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard, OnceLock, PoisonError};

/// The most threads that mark a heap, the thread that owns it included, unless LIBGC_MARKER_THREADS asks for another
/// number. Marking is bound by memory accesses, so more threads than this gain little.
const DEFAULT_MAX_THREADS: usize = 8;

/// The threads that mark a heap, the thread that owns it included.
pub(crate) fn thread_count() -> usize {
    static COUNT: OnceLock<usize> = OnceLock::new();
    *COUNT.get_or_init(|| {
        if let Some(count) = std::env::var("LIBGC_MARKER_THREADS")
            .ok()
            .and_then(|value| value.trim().parse::<usize>().ok())
        {
            return count.clamp(1, 64);
        }
        std::thread::available_parallelism()
            .map_or(1, usize::from)
            .min(DEFAULT_MAX_THREADS)
    })
}

/// What a collection hands the helpers: a function they each call with the data and their index, from 1 up.
#[derive(Clone, Copy)]
struct Task {
    run: unsafe fn(data: *const (), index: usize),
    data: *const (),
}

// SAFETY: The thread that hands out a task keeps its data alive and waits for every helper to be done with it.
unsafe impl Send for Task {}

struct State {
    /// Goes up by one for every task, which tells the helpers that a new one is there.
    generation: u64,
    task: Option<Task>,
    /// The helpers that have yet to finish the current task.
    helpers_left: usize,
}

pub(crate) struct Pool {
    /// The helpers that started, which is set once before the pool is used.
    helper_count: AtomicUsize,
    state: Mutex<State>,
    task_available: Condvar,
    helpers_done: Condvar,
    /// Held while a collection uses the helpers. Heaps of other threads collect on their own meanwhile.
    in_use: Mutex<()>,
    /// Whether the helpers ran a task since they last gave their unused memory back, see release_unused_memory().
    ran_since_release: AtomicBool,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

impl Pool {
    /// The pool of the process, which is created on first use. There is none if only one thread marks.
    pub(crate) fn the() -> Option<&'static Pool> {
        static POOL: OnceLock<Option<&'static Pool>> = OnceLock::new();
        *POOL.get_or_init(|| {
            let helper_count = thread_count() - 1;
            if helper_count == 0 {
                return None;
            }
            let pool: &'static Pool = Box::leak(Box::new(Pool {
                helper_count: AtomicUsize::new(0),
                state: Mutex::new(State {
                    generation: 0,
                    task: None,
                    helpers_left: 0,
                }),
                task_available: Condvar::new(),
                helpers_done: Condvar::new(),
                in_use: Mutex::new(()),
                ran_since_release: AtomicBool::new(false),
            }));
            // NB: A helper that cannot start, as when the process has as many threads as it may, leaves marking to the
            //     ones that did, or to the thread that owns the heap alone.
            let started = (1..=helper_count)
                .take_while(|&index| {
                    std::thread::Builder::new()
                        .name("GC marker".into())
                        .spawn(move || pool.run_helper(index))
                        .is_ok()
                })
                .count();
            if started == 0 {
                return None;
            }
            pool.helper_count.store(started, Ordering::Relaxed);
            Some(pool)
        })
    }

    pub(crate) fn helper_count(&self) -> usize {
        self.helper_count.load(Ordering::Relaxed)
    }

    /// Takes the helpers for one collection, unless another thread uses them.
    pub(crate) fn try_acquire(&'static self) -> Option<PoolGuard> {
        let guard = self.in_use.try_lock().ok()?;
        Some(PoolGuard {
            pool: self,
            _in_use: guard,
        })
    }

    /// Has the helpers give back the memory they freed, unless another heap uses them now, which does the same once it is
    /// done. What a helper allocates while it marks, such as room in its queue, is freed by the thread that collects
    /// once the collection is over, and the allocator keeps it for the helper until the helper allocates again, which an
    /// idle helper never does.
    pub(crate) fn release_unused_memory(&'static self) {
        if !self.ran_since_release.load(Ordering::Relaxed) {
            return;
        }
        let Some(helpers) = self.try_acquire() else {
            return;
        };
        unsafe fn release(_: *const (), _: usize) {
            ak::release_unused_allocator_memory();
        }
        // SAFETY: release() takes nothing, and any thread may call it.
        unsafe { helpers.run(release, core::ptr::null(), || {}) };
        // NB: Running this task set the flag as well. No other task runs while the helpers are taken.
        self.ran_since_release.store(false, Ordering::Relaxed);
    }

    fn run_helper(&self, index: usize) {
        // NB: The pool starts at generation 0, so a helper that starts after the first task was handed out still sees it.
        let mut seen_generation = 0;
        loop {
            let task = {
                let mut state = lock(&self.state);
                while state.generation == seen_generation {
                    state = self.task_available.wait(state).unwrap_or_else(PoisonError::into_inner);
                }
                seen_generation = state.generation;
                state.task.expect("a new generation has a task")
            };
            // SAFETY: The thread that handed out the task keeps its data alive until every helper is done with it.
            unsafe { (task.run)(task.data, index) };
            let mut state = lock(&self.state);
            state.helpers_left -= 1;
            if state.helpers_left == 0 {
                self.helpers_done.notify_one();
            }
        }
    }
}

/// The helpers, taken for one collection.
pub(crate) struct PoolGuard {
    pool: &'static Pool,
    _in_use: MutexGuard<'static, ()>,
}

impl PoolGuard {
    /// Has every helper call `run` with `data` and its index while the calling thread calls `owner`, and returns once
    /// all are done.
    ///
    /// # Safety
    ///
    /// `run` must be safe to call with `data` and each index from 1 to helper_count(), on any thread, at the same time
    /// as `owner` runs.
    pub(crate) unsafe fn run(
        &self,
        run: unsafe fn(data: *const (), index: usize),
        data: *const (),
        owner: impl FnOnce(),
    ) {
        let pool = self.pool;
        pool.ran_since_release.store(true, Ordering::Relaxed);
        {
            let mut state = lock(&pool.state);
            state.generation += 1;
            state.task = Some(Task { run, data });
            state.helpers_left = pool.helper_count();
            pool.task_available.notify_all();
        }
        owner();
        let mut state = lock(&pool.state);
        while state.helpers_left > 0 {
            state = pool.helpers_done.wait(state).unwrap_or_else(PoisonError::into_inner);
        }
        state.task = None;
    }
}
