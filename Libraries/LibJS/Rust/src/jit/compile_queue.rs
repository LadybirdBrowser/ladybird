/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compile threads. Only snapshots go to them and only compile results come back: the compiler never sees the heap.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, channel};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

use libjs_jit::CompileFailure;
use libjs_jit::code::{CompiledCode, Site};
use libjs_jit::snapshot::Snapshot;

pub type CompileResult = Result<CompiledCode, CompileFailure>;

/// How long a compile thread waits for another job before it gives the memory its jobs freed back to the system.
const IDLE_MEMORY_RELEASE_DELAY: Duration = Duration::from_secs(1);

/// What the compile threads do: compile a snapshot, or drop what dead code no longer needs.
enum Job {
    Compile(u64, Box<Snapshot>),
    /// The sites of code that died. The compile threads allocated them, so they free them faster than the thread that
    /// collects the code, and outside its collection.
    Drop(Vec<Site>),
}

struct Jobs {
    queue: VecDeque<Job>,
    /// Set once the queue is dropped, which ends the threads.
    closed: bool,
}

struct SharedJobs {
    jobs: Mutex<Jobs>,
    job_available: Condvar,
}

/// Threads that compile the snapshots they are sent, each taking the oldest job no other thread took yet. Jobs finish
/// in any order. Dropping the queue lets the threads end once they are done with the jobs they compile; the results of
/// abandoned jobs are dropped.
pub struct CompileQueue {
    jobs: Arc<SharedJobs>,
    results: Receiver<(u64, CompileResult)>,
    /// Set by a compile thread after it sends a result, and cleared before the results are taken. The thread that owns
    /// the queue checks for finished jobs very often, and the flag is cheaper to check than the channel.
    results_pending: Arc<AtomicBool>,
}

impl CompileQueue {
    pub fn start(thread_count: usize) -> Self {
        let jobs = Arc::new(SharedJobs {
            jobs: Mutex::new(Jobs {
                queue: VecDeque::new(),
                closed: false,
            }),
            job_available: Condvar::new(),
        });
        let (result_sender, results) = channel();
        let results_pending = Arc::new(AtomicBool::new(false));
        for index in 0..thread_count.max(1) {
            let jobs = Arc::clone(&jobs);
            let result_sender = result_sender.clone();
            let results_pending = Arc::clone(&results_pending);
            let spawned = std::thread::Builder::new()
                .name("LibJS JIT".to_string())
                .spawn(move || {
                    let mut released_memory_since_last_job = true;
                    loop {
                        let job = match next_job(&jobs, released_memory_since_last_job) {
                            NextJob::Job(job) => job,
                            NextJob::Idle => {
                                // NB: A compile can allocate a lot for a while, and the allocator keeps the memory a
                                //     thread freed until the thread allocates again, which an idle thread may never do.
                                ak::release_unused_allocator_memory();
                                released_memory_since_last_job = true;
                                continue;
                            }
                            NextJob::Closed => return,
                        };
                        released_memory_since_last_job = false;
                        let (id, snapshot) = match job {
                            Job::Compile(id, snapshot) => (id, snapshot),
                            Job::Drop(sites) => {
                                drop(sites);
                                continue;
                            }
                        };
                        let result = libjs_jit::compile(&snapshot);
                        if result_sender.send((id, result)).is_err() {
                            return;
                        }
                        results_pending.store(true, Ordering::Release);
                    }
                });
            // NB: The queue needs one thread. If the process cannot start more, it compiles on those it started.
            if index == 0 {
                spawned.expect("starting a JIT compile thread");
            } else if spawned.is_err() {
                break;
            }
        }
        Self {
            jobs,
            results,
            results_pending,
        }
    }

    pub fn submit(&self, id: u64, snapshot: Box<Snapshot>) {
        self.enqueue(Job::Compile(id, snapshot));
    }

    /// Has a compile thread drop the sites of code that died.
    pub fn drop_sites(&self, sites: Vec<Site>) {
        self.enqueue(Job::Drop(sites));
    }

    fn enqueue(&self, job: Job) {
        self.jobs
            .jobs
            .lock()
            .expect("no compile thread panics while holding the jobs")
            .queue
            .push_back(job);
        self.jobs.job_available.notify_one();
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
        // NB: A result sent after the flag is cleared sets it again, so it is taken here or by the next call. The load
        //     keeps the swap, which is a full barrier, out of the calls that find nothing.
        if !self.results_pending.load(Ordering::Relaxed) || !self.results_pending.swap(false, Ordering::Acquire) {
            return Vec::new();
        }
        self.results.try_iter().collect()
    }
}

impl Drop for CompileQueue {
    fn drop(&mut self) {
        self.jobs
            .jobs
            .lock()
            .expect("no compile thread panics while holding the jobs")
            .closed = true;
        self.jobs.job_available.notify_all();
    }
}

enum NextJob {
    Job(Job),
    /// No job came within IDLE_MEMORY_RELEASE_DELAY.
    Idle,
    /// The queue was dropped.
    Closed,
}

/// The next job, waiting for one. A thread that has not released its memory since its last job only waits
/// IDLE_MEMORY_RELEASE_DELAY.
fn next_job(jobs: &SharedJobs, released_memory_since_last_job: bool) -> NextJob {
    let mut guard = jobs
        .jobs
        .lock()
        .expect("no compile thread panics while holding the jobs");
    loop {
        if guard.closed {
            // NB: Sites still waiting to be dropped are dropped here too, by the threads that allocated them. The
            //     snapshots of abandoned compiles go with the queue.
            while let Some(job) = guard.queue.pop_front() {
                if let Job::Drop(_) = job {
                    return NextJob::Job(job);
                }
            }
            return NextJob::Closed;
        }
        if let Some(job) = guard.queue.pop_front() {
            return NextJob::Job(job);
        }
        if released_memory_since_last_job {
            guard = jobs
                .job_available
                .wait(guard)
                .expect("no compile thread panics while holding the jobs");
            continue;
        }
        let (next_guard, wait) = jobs
            .job_available
            .wait_timeout(guard, IDLE_MEMORY_RELEASE_DELAY)
            .expect("no compile thread panics while holding the jobs");
        guard = next_guard;
        if wait.timed_out() && guard.queue.is_empty() && !guard.closed {
            return NextJob::Idle;
        }
    }
}
