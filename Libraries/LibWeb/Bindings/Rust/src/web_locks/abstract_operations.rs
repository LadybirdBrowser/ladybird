/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use std::collections::{HashMap, VecDeque};
use std::hash::{BuildHasherDefault, DefaultHasher};

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Realm, Trace, Visitor, Vm};

use crate::bindings::callback::PromiseCallbackFunction;
use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;
use crate::bindings::interface::{Ref, cell_from_abi};
use crate::bindings::promise::{pending_steps, run_pending_steps};
use crate::web_locks::lock_manager::LockManager;
use crate::web_locks::lock_request::LockRequestRef;

unsafe extern "C" {
    fn web_locks_environment_lock_manager(environment: *mut c_void) -> *mut c_void;
    fn web_locks_queue_task(
        realm: *mut c_void,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
    fn web_locks_queue_callback_task(
        callback: *mut c_void,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct LockRequestQueue(pub(crate) GcRefCell<VecDeque<LockRequestRef>>);

static LOCK_REQUEST_QUEUE_CLASS: EmbedderClass<LockRequestQueue> = EmbedderClass::new("LockRequestQueue");

impl EmbedderValue for LockRequestQueue {
    const CLASS: &'static EmbedderClass<Self> = &LOCK_REQUEST_QUEUE_CLASS;
}

pub(crate) type LockRequestQueueRef = Gc<EmbedderCell<LockRequestQueue>>;

// NB: A fixed hasher keeps the order of query()'s pending list the same from run to run.
#[derive(Default)]
pub(crate) struct LockRequestQueueMap(
    pub(crate) HashMap<Utf16String, LockRequestQueueRef, BuildHasherDefault<DefaultHasher>>,
);

// SAFETY: Only the map's values contain cells.
unsafe impl Trace for LockRequestQueueMap {
    fn trace(&self, visitor: &mut Visitor) {
        for queue in self.0.values() {
            queue.trace(visitor);
        }
    }
}

impl LockRequestQueueMap {
    // https://w3c.github.io/web-locks/#get-the-lock-request-queue
    pub(crate) fn get_lock_request_queue(&mut self, vm: &Vm, name: &Utf16String) -> LockRequestQueueRef {
        // 1. If queueMap[name] does not exist, set queueMap[name] to a new empty lock request queue.
        let queue = *self.0.entry(name.clone()).or_insert_with(|| {
            LOCK_REQUEST_QUEUE_CLASS.allocate(vm, |_| LockRequestQueue(GcRefCell::new(VecDeque::new())))
        });

        // 2. Return queueMap[name].
        queue
    }
}

// https://w3c.github.io/web-locks/#obtain-a-lock-manager
pub(crate) fn obtain_lock_manager(environment: &CppEnvironmentSettingsObject) -> Ref<LockManager> {
    // 1. Let map be the result of obtaining a local storage bottle map given environment and "web-locks".
    // 2. If map is failure, then return failure.
    // 3. Let bottle be map’s associated storage bottle.
    // 4. Return bottle’s associated lock manager.

    // FIXME: Spec issue: Storing and obtaining a lock manager in the environment's storage is currently not well
    //        defined. So we store a single instance per environment for now. See:
    //        https://w3c.github.io/web-locks/#issue-73644ca1
    // SAFETY: The environment is live, and its settings object keeps its manager alive.
    unsafe { cell_from_abi(web_locks_environment_lock_manager(environment.as_ptr())) }
}

pub(crate) fn queue_web_locks_task<C: Trace + Clone + 'static>(
    vm: &Vm,
    realm: Gc<Realm>,
    captures: C,
    steps: fn(&Vm, C),
) {
    let cell = pending_steps(vm, captures, steps);
    // SAFETY: LibWeb retains the realm and the traced captures until the task runs.
    unsafe { web_locks_queue_task(realm.as_ptr().cast(), cell, run_pending_steps) };
}

pub(crate) fn queue_web_locks_callback_task<C: Trace + Clone + 'static>(
    vm: &Vm,
    callback: &PromiseCallbackFunction,
    captures: C,
    steps: fn(&Vm, C),
) {
    let cell = pending_steps(vm, captures, steps);
    // SAFETY: LibWeb retains the callback and the traced captures until the task runs.
    unsafe { web_locks_queue_callback_task(callback.as_ptr(), cell, run_pending_steps) };
}
