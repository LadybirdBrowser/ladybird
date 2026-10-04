/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Promises, promise capabilities and promise jobs.
//!
//! The functions here follow the contract object.rs states for the embedding module. A promise crosses as the JSObject
//! of the Promise, and the functions that take one abort for any other object, as the C++ as<JS::Promise>() does.
//! Settling a promise or adding a reaction to a settled one calls the embedder's enqueue_promise_job and
//! promise_rejection_tracker hooks, which may run JavaScript.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    cell_from_abi, cell_into_abi, completion_into_abi, object_into_abi, optional_cell_from_abi, vm_from_abi,
};
use crate::embedding::hooks::{JSPromiseJob, promise_job_from_abi};
use crate::gc::heap_function::HeapFunction;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSPromiseCapability, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::promise::{Promise, PromiseState};
use crate::runtime::promise_capability::new_promise_capability;

/// [[PromiseState]], in the order of the C++ JS::Promise::State.
pub type JSPromiseState = u8;

pub const JS_PROMISE_STATE_PENDING: JSPromiseState = 0;
pub const JS_PROMISE_STATE_FULFILLED: JSPromiseState = 1;
pub const JS_PROMISE_STATE_REJECTED: JSPromiseState = 2;

/// The functions CreateResolvingFunctions ( promise ) returns, which share one [[AlreadyResolved]] record.
#[repr(C)]
pub struct JSPromiseResolvingFunctions {
    pub resolve: *mut JSObject,
    pub reject: *mut JSObject,
}

/// # Safety
///
/// `promise` must be a live Promise.
unsafe fn promise_from_abi(promise: *mut JSObject) -> Gc<Promise> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(promise) }
        .downcast::<Promise>()
        .expect("the object is a Promise")
}

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

/// [[PromiseState]], one of JS_PROMISE_STATE_*. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_state(promise: *mut JSObject) -> JSPromiseState {
    // SAFETY: See the module documentation.
    match unsafe { promise_from_abi(promise) }.state() {
        PromiseState::Pending => JS_PROMISE_STATE_PENDING,
        PromiseState::Fulfilled => JS_PROMISE_STATE_FULFILLED,
        PromiseState::Rejected => JS_PROMISE_STATE_REJECTED,
    }
}

/// [[PromiseResult]]: the value or reason of a settled promise, and undefined while it is pending. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_result(promise: *mut JSObject) -> JSValue {
    // SAFETY: See the module documentation.
    unsafe { promise_from_abi(promise) }.result().0
}

/// [[PromiseIsHandled]]. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_is_handled(promise: *mut JSObject) -> bool {
    // SAFETY: See the module documentation.
    unsafe { promise_from_abi(promise) }.is_handled()
}

/// Sets [[PromiseIsHandled]] to true, without telling the promise_rejection_tracker hook, which the embedder does
/// itself where it needs to. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_set_is_handled(promise: *mut JSObject) {
    // SAFETY: See the module documentation.
    unsafe { promise_from_abi(promise) }.set_is_handled();
}

/// PerformPromiseThen ( promise, onFulfilled, onRejected [ , resultCapability ] ). A reaction that is not a function
/// passes the value or reason on, and a null capability stands for none. Returns the capability's promise, or
/// undefined without one. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_perform_then(
    vm: *mut JSVM,
    promise: *mut JSObject,
    on_fulfilled: JSValue,
    on_rejected: JSValue,
    result_capability: *mut JSPromiseCapability,
) -> JSValue {
    // SAFETY: See the module documentation.
    let (vm, promise, result_capability) = unsafe {
        (
            vm_from_abi(vm),
            promise_from_abi(promise),
            optional_cell_from_abi::<JSPromiseCapability>(result_capability),
        )
    };
    promise
        .perform_then(vm, Value(on_fulfilled), Value(on_rejected), result_capability)
        .0
}

/// CreateResolvingFunctions ( promise ), whose functions belong to the current realm. Returns unrooted functions. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_create_resolving_functions(
    vm: *mut JSVM,
    promise: *mut JSObject,
) -> JSPromiseResolvingFunctions {
    // SAFETY: See the module documentation.
    let (vm, promise) = unsafe { (vm_from_abi(vm), promise_from_abi(promise)) };
    let resolving_functions = promise.create_resolving_functions(vm);
    JSPromiseResolvingFunctions {
        resolve: object_into_abi(resolving_functions.resolve),
        reject: object_into_abi(resolving_functions.reject),
    }
}

/// NewPromiseCapability ( C ), whose payload is the JSPromiseCapability. It throws a TypeError if C is not a
/// constructor, and whatever constructing C throws; the realm's %Promise% does neither. Returns an unrooted capability.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_capability_new(vm: *mut JSVM, constructor: JSValue) -> JSCompletion {
    // SAFETY: See the module documentation.
    let vm = unsafe { vm_from_abi(vm) };
    completion_into_abi(new_promise_capability(vm, Value(constructor)).map(cell_into_abi::<JSPromiseCapability>))
}

/// [[Promise]] of the capability, which is an object but not necessarily a Promise. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_capability_promise(capability: *mut JSPromiseCapability) -> *mut JSObject {
    // SAFETY: See the module documentation.
    object_into_abi(unsafe { cell_from_abi::<JSPromiseCapability>(capability) }.promise())
}

/// [[Resolve]] of the capability, a function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_capability_resolve(capability: *mut JSPromiseCapability) -> *mut JSObject {
    // SAFETY: See the module documentation.
    object_into_abi(unsafe { cell_from_abi::<JSPromiseCapability>(capability) }.resolve())
}

/// [[Reject]] of the capability, a function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_promise_capability_reject(capability: *mut JSPromiseCapability) -> *mut JSObject {
    // SAFETY: See the module documentation.
    object_into_abi(unsafe { cell_from_abi::<JSPromiseCapability>(capability) }.reject())
}
