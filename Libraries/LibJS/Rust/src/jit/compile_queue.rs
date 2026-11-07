/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A VM's share of the compile threads of the process. Only snapshots go to them and only compile results come back: the
//! compiler never sees the heap.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use libjs_jit::CompileFailure;
use libjs_jit::code::{CompiledCode, Site};
use libjs_jit::compile_pool::{CompilePool, CompilePoolClient};
use libjs_jit::snapshot::Snapshot;

pub type CompileResult = Result<CompiledCode, CompileFailure>;

/// How long a compile thread waits for another job before it gives the memory its jobs freed back to the system.
const IDLE_MEMORY_RELEASE_DELAY: Duration = Duration::from_secs(1);

/// The compile threads of the process. The first VM to compile decides how many there can be.
fn compile_pool(thread_count: usize) -> &'static Arc<CompilePool> {
    static POOL: OnceLock<Arc<CompilePool>> = OnceLock::new();
    POOL.get_or_init(|| {
        // NB: A compile can allocate a lot for a while, and the allocator keeps the memory a thread freed until the
        //     thread allocates again, which an idle thread may never do.
        CompilePool::new(
            thread_count,
            "LibJS JIT",
            IDLE_MEMORY_RELEASE_DELAY,
            ak::release_unused_allocator_memory,
        )
    })
}

/// A VM's queue of compile jobs, which jobs of other VMs take turns with. Jobs finish in any order. Dropping the queue
/// drops the jobs that are still in it. The result of a job that is running by then is dropped once it finishes.
pub struct CompileQueue {
    client: CompilePoolClient,
    result_sender: Sender<(u64, CompileResult)>,
    results: Receiver<(u64, CompileResult)>,
    /// Set by a compile thread after it sends a result, and cleared before the results are taken. The thread that owns
    /// the queue checks for finished jobs very often, and the flag is cheaper to check than the channel.
    results_pending: Arc<AtomicBool>,
}

impl CompileQueue {
    pub fn start(thread_count: usize) -> Self {
        let (result_sender, results) = channel();
        Self {
            client: compile_pool(thread_count).add_client(),
            result_sender,
            results,
            results_pending: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn submit(&self, id: u64, snapshot: Box<Snapshot>) {
        let result_sender = self.result_sender.clone();
        let results_pending = Arc::clone(&self.results_pending);
        self.client.submit(Box::new(move || {
            let compiled = {
                let _scratch = ak::ScratchHeapScope::enter();
                libjs_jit::compile(&snapshot)
            };
            // NB: Nearly all a compile allocates is gone when it ends. The code it leaves outlives it, and would keep
            //     the pages of its temporary data if it stayed there, so it moves out of the scratch heap here.
            #[allow(
                clippy::map_clone,
                clippy::redundant_clone,
                reason = "the copy is what moves the code out of the scratch heap"
            )]
            let result = compiled.map(|code| code.clone());
            // NB: A VM that went away has no use for the result, and the thread goes on with other VMs' jobs.
            if result_sender.send((id, result)).is_ok() {
                results_pending.store(true, Ordering::Release);
            }
        }));
    }

    /// Has a compile thread drop the sites of code that died. The compile threads allocated them, so they free them
    /// faster than the thread that collects the code, and outside its collection. This outlives the VM.
    pub fn drop_sites(&self, sites: Vec<Site>) {
        self.client.pool().run_unowned(Box::new(move || drop(sites)));
    }

    /// The results of the jobs that finished since the last call, waiting for one to finish if none has. The queue must
    /// have a job.
    pub fn wait_for_finished(&self) -> Vec<(u64, CompileResult)> {
        let mut finished = vec![self.results.recv().expect("the queue keeps a sender")];
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
