/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Persistent workers for blocking batches of style work.
//!
//! A submitted closure may borrow data owned by the caller because [`run`]
//! does not return until every closure has finished. The lifetime erasure at
//! the queue boundary recreates the guarantee of `std::thread::scope` while
//! avoiding an operating-system thread spawn for every substituted value.

use std::any::Any;
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::sync::{Condvar, Mutex, OnceLock};

type Job = Box<dyn FnOnce() + Send + 'static>;

struct State {
    jobs: Vec<Job>,
    pending: usize,
    panic: Option<Box<dyn Any + Send>>,
}

struct Pool {
    state: Mutex<State>,
    work_available: Condvar,
    batch_done: Condvar,
    submission: Mutex<()>,
}

impl Pool {
    fn worker_loop(&self) {
        loop {
            let job = {
                let mut state = self.state.lock().unwrap();
                while state.jobs.is_empty() {
                    state = self.work_available.wait(state).unwrap();
                }
                state.jobs.pop().unwrap()
            };
            let panic = catch_unwind(AssertUnwindSafe(job)).err();
            let mut state = self.state.lock().unwrap();
            if state.panic.is_none() {
                state.panic = panic;
            }
            state.pending -= 1;
            if state.pending == 0 {
                self.batch_done.notify_one();
            }
        }
    }

    fn run<F: FnOnce() + Send>(&'static self, jobs: Vec<F>) {
        if jobs.is_empty() {
            return;
        }
        let _submission = self.submission.lock().unwrap();
        {
            let mut state = self.state.lock().unwrap();
            debug_assert_eq!(state.pending, 0);
            debug_assert!(state.panic.is_none());
            state.pending = jobs.len();
            state.jobs.extend(jobs.into_iter().map(|job| {
                let job: Box<dyn FnOnce() + Send + '_> = Box::new(job);
                // SAFETY: This function waits for `pending` to reach zero before returning, so
                // every captured borrow outlives its queued closure.
                unsafe { std::mem::transmute::<Box<dyn FnOnce() + Send + '_>, Job>(job) }
            }));
        }
        self.work_available.notify_all();

        let mut state = self.state.lock().unwrap();
        while state.pending != 0 {
            state = self.batch_done.wait(state).unwrap();
        }
        if let Some(panic) = state.panic.take() {
            drop(state);
            resume_unwind(panic);
        }
    }
}

fn pool() -> &'static Pool {
    static POOL: OnceLock<&'static Pool> = OnceLock::new();
    POOL.get_or_init(|| {
        let worker_count = std::thread::available_parallelism()
            .map_or(1, usize::from)
            .saturating_sub(1)
            .max(1);
        let pool: &'static Pool = Box::leak(Box::new(Pool {
            state: Mutex::new(State {
                jobs: Vec::new(),
                pending: 0,
                panic: None,
            }),
            work_available: Condvar::new(),
            batch_done: Condvar::new(),
            submission: Mutex::new(()),
        }));
        for _ in 0..worker_count {
            std::thread::Builder::new()
                .name("StyleWorker".into())
                .spawn(move || pool.worker_loop())
                .expect("spawn style worker");
        }
        pool
    })
}

/// Runs every job on persistent style workers and blocks until all jobs have
/// finished. Jobs never execute on the calling thread.
pub(crate) fn run<F: FnOnce() + Send>(jobs: Vec<F>) {
    pool().run(jobs);
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;

    #[test]
    fn runs_jobs_away_from_the_calling_thread() {
        let caller = std::thread::current().id();
        let workers = Mutex::new(Vec::new());
        run((0..4)
            .map(|_| || workers.lock().unwrap().push(std::thread::current().id()))
            .collect());
        let workers = workers.into_inner().unwrap();
        assert_eq!(workers.len(), 4);
        assert!(workers.into_iter().all(|worker| worker != caller));
    }
}
