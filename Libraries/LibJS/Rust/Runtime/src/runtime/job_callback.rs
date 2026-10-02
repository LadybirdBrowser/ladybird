/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;

// 9.5.1 JobCallback Records, https://tc39.es/ecma262/#sec-jobcallback-records
/// NB: C++ also keeps the host-defined [[HostDefined]] data LibWeb attaches to its callbacks. The hosts of the Rust
///     runtime attach none.
#[repr(C)]
#[derive(Trace)]
pub struct JobCallback {
    header: CellHeader,
    callback: Cell<Gc<FunctionObject>>,
}

define_cell!(JobCallback, Other);

impl JobCallback {
    pub fn create(vm: &Vm, callback: Gc<FunctionObject>) -> Gc<JobCallback> {
        vm.heap().allocate(JobCallback {
            header: CellHeader::for_class(Self::CLASS),
            callback: Cell::new(callback),
        })
    }

    pub fn callback(&self) -> Gc<FunctionObject> {
        self.callback.get()
    }
}

// 9.5.2 HostMakeJobCallback ( callback ), https://tc39.es/ecma262/#sec-hostmakejobcallback
pub fn make_job_callback(vm: &Vm, callback: Gc<FunctionObject>) -> Gc<JobCallback> {
    // 1. Return the JobCallback Record { [[Callback]]: callback, [[HostDefined]]: empty }.
    JobCallback::create(vm, callback)
}

// 9.5.3 HostCallJobCallback ( jobCallback, V, argumentsList ), https://tc39.es/ecma262/#sec-hostcalljobcallback
pub fn call_job_callback(
    vm: &Vm,
    job_callback: Gc<JobCallback>,
    this_value: Value,
    arguments_list: &[Value],
) -> ThrowCompletionOr<Value> {
    // 1. Assert: IsCallable(jobCallback.[[Callback]]) is true.

    // 2. Return ? Call(jobCallback.[[Callback]], V, argumentsList).
    call_function_object(vm, job_callback.callback(), this_value, arguments_list)
}
