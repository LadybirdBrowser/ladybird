/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! FinalizationRegistries, whose cleanup the embedder runs as the jobs its enqueue_finalization_registry_cleanup_job
//! hook queues.
//!
//! WeakRefs need nothing of the embedder but the end of each synchronous run of code, ClearKeptObjects(), which
//! js_vm_finish_execution_generation() performs.
//!
//! The functions here follow the contract object.rs states for the embedding module. A FinalizationRegistry crosses as
//! its JSObject, and the functions that take one abort for any other object, as the C++ as<JS::FinalizationRegistry>()
//! does.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    JSRealm, cell_from_abi, cell_into_abi, completion_into_abi, optional_cell_from_abi, vm_from_abi,
};
use crate::embedding::realm::JSJobCallback;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM};
use crate::runtime::finalization_registry::FinalizationRegistry;

/// # Safety
///
/// `finalization_registry` must be a live FinalizationRegistry.
unsafe fn finalization_registry_from_abi(finalization_registry: *mut JSObject) -> Gc<FinalizationRegistry> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(finalization_registry) }
        .downcast::<FinalizationRegistry>()
        .expect("the object is a FinalizationRegistry")
}

/// [[Realm]] of the FinalizationRegistry, the realm its constructor belonged to. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_weak_finalization_registry_realm(finalization_registry: *mut JSObject) -> *mut JSRealm {
    // SAFETY: See the module documentation.
    cell_into_abi(unsafe { finalization_registry_from_abi(finalization_registry) }.realm())
}

/// [[CleanupCallback]] of the FinalizationRegistry, the JobCallback Record that HostMakeJobCallback made of the
/// function its constructor received. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_weak_finalization_registry_cleanup_callback(
    finalization_registry: *mut JSObject,
) -> *mut JSJobCallback {
    // SAFETY: See the module documentation.
    cell_into_abi(unsafe { finalization_registry_from_abi(finalization_registry) }.cleanup_callback())
}

/// CleanupFinalizationRegistry ( finalizationRegistry ): calls the callback, or [[CleanupCallback]] for null, through
/// the call_job_callback hook with the held value of each registered target that has been collected, and forgets
/// those registrations. A throw stops it and leaves the remaining ones for the next cleanup. It runs JavaScript, so
/// the embedder prepares to run script first, as HTML does. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_weak_finalization_registry_cleanup(
    vm: *mut JSVM,
    finalization_registry: *mut JSObject,
    callback: *mut JSJobCallback,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, finalization_registry, callback) = unsafe {
        (
            vm_from_abi(vm),
            finalization_registry_from_abi(finalization_registry),
            optional_cell_from_abi::<JSJobCallback>(callback),
        )
    };
    completion_into_abi(finalization_registry.cleanup(vm, callback))
}
