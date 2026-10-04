/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Promises, promise capabilities and promise jobs.

use crate::embedding::abi_types::{completion_into_abi, vm_from_abi};
use crate::embedding::hooks::{JSPromiseJob, promise_job_from_abi};
use crate::gc::heap_function::HeapFunction;
use crate::layout::host_class::{JSCompletion, JSVM};

/// Runs a promise job that the embedder's enqueue_promise_job hook received, and returns its completion. Like the jobs
/// of the VM's own queue, it runs on top of the running execution context, so the embedder prepares that first, as
/// HTML prepares to run script with the job's realm. Each job runs once. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `job` a promise job of it that the embedder kept alive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_job_run(vm: *mut JSVM, job: *mut JSPromiseJob) -> JSCompletion {
    // SAFETY: The caller passes a live VM and promise job.
    let (vm, job) = unsafe { (vm_from_abi(vm), promise_job_from_abi(job)) };
    completion_into_abi(HeapFunction::call(job, vm))
}
