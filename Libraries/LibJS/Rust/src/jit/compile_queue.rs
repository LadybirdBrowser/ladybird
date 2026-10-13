/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compile threads. Only snapshots go to them and only compile results come back: the compiler never sees the heap.

use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, Mutex};

use libjs_jit::CompileFailure;
use libjs_jit::code::{CompiledCode, Site};
use libjs_jit::snapshot::Snapshot;

pub type CompileResult = Result<CompiledCode, CompileFailure>;

/// What the compile threads do: compile a snapshot, or drop what dead code no longer needs.
enum Job {
    Compile(u64, Box<Snapshot>),
    /// The sites of code that died. The compile threads allocated them, so they free them faster than the thread that
    /// collects the code, and outside its collection.
    Drop(Vec<Site>),
}

/// Threads that compile the snapshots they are sent, each taking the oldest job no other thread took yet. Jobs finish
/// in any order. Dropping the queue lets the threads end once they are done with the jobs they compile; the results of
/// abandoned jobs are dropped.
pub struct CompileQueue {
    jobs: Sender<Job>,
    results: Receiver<(u64, CompileResult)>,
}

impl CompileQueue {
    pub fn start(thread_count: usize) -> Self {
        let (jobs, job_receiver) = channel::<Job>();
        let job_receiver = Arc::new(Mutex::new(job_receiver));
        let (result_sender, results) = channel();
        for index in 0..thread_count.max(1) {
            let job_receiver = Arc::clone(&job_receiver);
            let result_sender = result_sender.clone();
            let spawned = std::thread::Builder::new()
                .name("LibJS JIT".to_string())
                .spawn(move || {
                    loop {
                        // NB: The lock is held only while waiting for a job, so the other threads compile meanwhile.
                        let job = job_receiver
                            .lock()
                            .expect("no compile thread panics while waiting")
                            .recv();
                        let (id, snapshot) = match job {
                            Ok(Job::Compile(id, snapshot)) => (id, snapshot),
                            Ok(Job::Drop(sites)) => {
                                drop(sites);
                                continue;
                            }
                            Err(_) => return,
                        };
                        let result = libjs_jit::compile(&snapshot);
                        if result_sender.send((id, result)).is_err() {
                            return;
                        }
                    }
                });
            // NB: The queue needs one thread. If the process cannot start more, it compiles on those it started.
            if index == 0 {
                spawned.expect("starting a JIT compile thread");
            } else if spawned.is_err() {
                break;
            }
        }
        Self { jobs, results }
    }

    pub fn submit(&self, id: u64, snapshot: Box<Snapshot>) {
        self.jobs
            .send(Job::Compile(id, snapshot))
            .expect("the compile threads run while the queue lives");
    }

    /// Has a compile thread drop the sites of code that died.
    pub fn drop_sites(&self, sites: Vec<Site>) {
        self.jobs
            .send(Job::Drop(sites))
            .expect("the compile threads run while the queue lives");
    }

    /// The results of the jobs that finished since the last call, waiting for one to finish if none has. The queue must
    /// have a job.
    pub fn wait_for_finished(&self) -> Vec<(u64, CompileResult)> {
        let mut finished = vec![
            self.results
                .recv()
                .expect("the compile threads run while the queue lives"),
        ];
        finished.extend(self.results.try_iter());
        finished
    }

    /// The results of the jobs that finished since the last call.
    pub fn take_finished(&self) -> Vec<(u64, CompileResult)> {
        self.results.try_iter().collect()
    }
}
