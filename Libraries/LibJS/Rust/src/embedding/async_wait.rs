/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use std::sync::{Arc, PoisonError};

use crate::embedding::abi_types::{JSRealm, cell_into_abi, vm_from_abi};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::JSVM;
use crate::runtime::async_wait::AsyncWaitAgent;
use crate::runtime::realm::Realm;

pub struct JSAsyncWaitQueue {
    _private: [u8; 0],
}

/// Scheduling for asynchronous atomic waits. wake() can run on any thread and receives an owned queue reference.
/// It must arrange dispatch on the owning thread, then release that reference, including if dispatch is canceled.
/// The other scheduling callbacks run on the VM's thread. destroy() can run on any thread after the last wake returns.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSAsyncWaitHooks {
    pub data: *mut c_void,
    pub wake: unsafe extern "C" fn(*mut c_void, *const JSAsyncWaitQueue),
    pub destroy: unsafe extern "C" fn(*mut c_void),
    pub enqueue_resolution: unsafe extern "C" fn(*mut c_void, *mut JSVM, u64, *mut JSRealm),
    pub schedule_timeout: unsafe extern "C" fn(*mut c_void, *mut JSVM, u64, *mut JSRealm, f64) -> i32,
    pub cancel_timeout: unsafe extern "C" fn(*mut c_void, *mut JSVM, *mut JSRealm, i32),
}

pub(crate) struct AsyncWaitHooks(JSAsyncWaitHooks);

// SAFETY: The installer guarantees that wake/destroy and their context can be used from any thread. The runtime
// calls the other hooks only on the VM's thread.
unsafe impl Send for AsyncWaitHooks {}
// SAFETY: As above; concurrent wake calls are permitted by the installation contract.
unsafe impl Sync for AsyncWaitHooks {}

impl Drop for AsyncWaitHooks {
    fn drop(&mut self) {
        // SAFETY: The last owner releases the context after every callback has returned.
        unsafe { (self.0.destroy)(self.0.data) };
    }
}

impl AsyncWaitHooks {
    pub(crate) fn wake(&self, agent: Arc<AsyncWaitAgent>) {
        // SAFETY: The host owns this retained reference and must release it after dispatch or cancellation.
        unsafe { (self.0.wake)(self.0.data, Arc::into_raw(agent).cast()) };
    }

    pub(crate) fn enqueue_resolution(&self, vm: &Vm, id: u64, realm: Gc<Realm>) {
        // SAFETY: The callback runs on the VM's thread and borrows a live realm.
        unsafe {
            (self.0.enqueue_resolution)(
                self.0.data,
                core::ptr::from_ref(vm).cast_mut().cast(),
                id,
                cell_into_abi(realm),
            );
        }
    }

    pub(crate) fn schedule_timeout(&self, vm: &Vm, id: u64, realm: Gc<Realm>, milliseconds: f64) -> i32 {
        // SAFETY: The callback runs on the VM's thread and borrows a live realm.
        unsafe {
            (self.0.schedule_timeout)(
                self.0.data,
                core::ptr::from_ref(vm).cast_mut().cast(),
                id,
                cell_into_abi(realm),
                milliseconds,
            )
        }
    }
    pub(crate) fn cancel_timeout(&self, vm: &Vm, realm: Gc<Realm>, timer: i32) {
        // SAFETY: Only the owning VM cancels its timer, while the realm is still live.
        unsafe {
            (self.0.cancel_timeout)(
                self.0.data,
                core::ptr::from_ref(vm).cast_mut().cast(),
                cell_into_abi(realm),
                timer,
            );
        }
    }
}

/// # Safety
/// vm must be live on this thread. hooks must be readable and satisfy JSAsyncWaitHooks' threading contract.
/// Ownership of hooks.data transfers to the VM; the host must keep any realm it queues alive until its callback runs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_set_async_wait_hooks(vm: *mut JSVM, hooks: &JSAsyncWaitHooks) {
    // SAFETY: Guaranteed by the caller.
    let vm = unsafe { vm_from_abi(vm) };
    *vm.async_wait_agent()
        .hooks
        .lock()
        .unwrap_or_else(PoisonError::into_inner) = Some(Arc::new(AsyncWaitHooks(*hooks)));
}

/// # Safety
/// queue must be a retained reference passed to wake(), and this must be its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_async_wait_queue_dispatch(queue: *const JSAsyncWaitQueue) {
    // SAFETY: The caller keeps the queue reference alive throughout this call.
    unsafe { &*queue.cast::<AsyncWaitAgent>() }.dispatch();
}

/// # Safety
/// queue must be an owned reference passed to wake(), and must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_async_wait_queue_release(queue: *const JSAsyncWaitQueue) {
    // SAFETY: This consumes exactly the reference transferred to wake().
    drop(unsafe { Arc::from_raw(queue.cast::<AsyncWaitAgent>()) });
}

/// # Safety
/// vm must be live on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_resolve_async_wait(vm: *mut JSVM, id: u64) {
    // SAFETY: Guaranteed by the caller.
    unsafe { vm_from_abi(vm) }.resolve_async_wait(id);
}

/// # Safety
/// vm must be live on its owning thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_timeout_async_wait(vm: *mut JSVM, id: u64) {
    // SAFETY: Guaranteed by the caller.
    unsafe { vm_from_abi(vm) }.timeout_async_wait(id);
}

/// # Safety
/// vm must be live on its owning thread. Call this when the timer fires, before queuing its timeout job.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_async_wait_timer_fired(vm: *mut JSVM, id: u64) {
    // SAFETY: Guaranteed by the caller.
    let vm = unsafe { vm_from_abi(vm) };
    if let Some(wait) = vm.async_waits().borrow_mut(vm).iter_mut().find(|wait| wait.id == id) {
        wait.host_timer = None;
    }
}
