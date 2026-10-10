/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compile thread. Only snapshots go to it and only compile results come back: the compiler never sees the heap.

use std::sync::mpsc::{Receiver, Sender, channel};

use libjs_jit::CompileFailure;
use libjs_jit::code::CompiledCode;
use libjs_jit::snapshot::Snapshot;

pub type CompileResult = Result<CompiledCode, CompileFailure>;

/// A thread that compiles the snapshots it is sent, one at a time, in order. Dropping the queue lets the thread end
/// once it is done with the job it compiles; the results of abandoned jobs are dropped.
pub struct CompileQueue {
    jobs: Sender<(u64, Box<Snapshot>)>,
    results: Receiver<(u64, CompileResult)>,
}

impl CompileQueue {
    pub fn start() -> Self {
        let (jobs, job_receiver) = channel::<(u64, Box<Snapshot>)>();
        let (result_sender, results) = channel();
        std::thread::Builder::new()
            .name("LibJS JIT".to_string())
            .spawn(move || {
                while let Ok((id, snapshot)) = job_receiver.recv() {
                    let result = libjs_jit::compile(&snapshot);
                    if result_sender.send((id, result)).is_err() {
                        return;
                    }
                }
            })
            .expect("starting the JIT compile thread");
        Self { jobs, results }
    }

    pub fn submit(&self, id: u64, snapshot: Box<Snapshot>) {
        self.jobs
            .send((id, snapshot))
            .expect("the compile thread runs while the queue lives");
    }

    /// The results of the jobs that finished since the last call, waiting for one to finish if none has. The queue must
    /// have a job.
    pub fn wait_for_finished(&self) -> Vec<(u64, CompileResult)> {
        let mut finished = vec![
            self.results
                .recv()
                .expect("the compile thread runs while the queue lives"),
        ];
        finished.extend(self.results.try_iter());
        finished
    }

    /// The results of the jobs that finished since the last call.
    pub fn take_finished(&self) -> Vec<(u64, CompileResult)> {
        self.results.try_iter().collect()
    }
}
