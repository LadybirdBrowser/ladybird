/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Vm};

use crate::bindings::callback::PromiseCallbackFunction;
use crate::bindings::interface::Ref;
use crate::bindings::promise::{WebIdlPromise, reject_promise};
use crate::dom::abort_signal::AbortSignal;
use crate::web_locks::abstract_operations::queue_web_locks_task;
use crate::web_locks::lock_manager::{LockManager, LockMode};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct LockRequest {
    #[gc(untraced)]
    pub(crate) client_id: Utf16String,
    pub(crate) manager: Ref<LockManager>,
    #[gc(untraced)]
    pub(crate) name: Utf16String,
    #[gc(untraced)]
    pub(crate) mode: LockMode,
    pub(crate) callback: PromiseCallbackFunction,
    pub(crate) promise: WebIdlPromise,
    pub(crate) signal: Option<AbortSignal>,
    pub(crate) abort_algorithm_id: Cell<Option<u64>>,
}

pub(crate) static LOCK_REQUEST_CLASS: EmbedderClass<LockRequest> = EmbedderClass::new("LockRequest");

impl EmbedderValue for LockRequest {
    const CLASS: &'static EmbedderClass<Self> = &LOCK_REQUEST_CLASS;
}

pub(crate) type LockRequestRef = Gc<EmbedderCell<LockRequest>>;

impl LockRequest {
    // https://w3c.github.io/web-locks/#abort-the-request
    pub(crate) fn abort_request(this: LockRequestRef, vm: &Vm) {
        // 1. Assert: these steps are running on the lock task queue.

        // 2. Let manager be request’s manager.
        let manager = this.manager;

        // 3. Let name be request’s name.
        let name = &this.name;

        // 4. Let queueMap be manager’s lock request queue map.
        let queue_map = &manager.lock_request_queue_map;

        // 5. Let queue be the result of getting the lock request queue from queueMap for name.
        let queue = queue_map.borrow_mut(vm).get_lock_request_queue(vm, name);

        // 6. Remove request from queue.
        queue.0.borrow_mut(vm).retain(|request| *request != this);

        // 7. Process the lock request queue queue.
        LockManager::process_lock_request_queue(vm, queue);
    }

    // https://w3c.github.io/web-locks/#signal-to-abort-the-request
    pub(crate) fn signal_to_abort_request(this: LockRequestRef, vm: &Vm, signal: &AbortSignal) {
        // 1. Enqueue the steps to abort the request request to the lock task queue.
        queue_web_locks_task(vm, this.manager.realm, this, |vm, request| {
            Self::abort_request(request, vm);
        });

        // 2. Reject request’s promise with signal’s abort reason.
        reject_promise(vm, this.promise, signal.reason());
    }

    // https://w3c.github.io/web-locks/#grantable
    pub(crate) fn is_grantable(this: LockRequestRef, vm: &Vm) -> bool {
        // 1. Let manager be request’s manager.
        let manager = this.manager;

        // 2. Let queueMap be manager’s lock request queue map.
        let queue_map = &manager.lock_request_queue_map;

        // 3. Let name be request’s name.
        let name = &this.name;

        // 4. Let queue be the result of getting the lock request queue from queueMap for name.
        let queue = queue_map.borrow_mut(vm).get_lock_request_queue(vm, name);

        // 5. Let held be manager’s held lock set
        let held = manager.held_lock_set.borrow();

        // 6. Let mode be request’s mode
        let mode = this.mode;

        // 7. If queue is not empty and request is not the first item in queue, then return false.
        if queue.0.borrow().front().is_some_and(|request| *request != this) {
            return false;
        }

        // 8. If mode is "exclusive", then return true if no lock in held has name equal to name, and false otherwise.
        if mode == LockMode::Exclusive {
            return !held.iter().any(|lock| &lock.name == name);
        }

        // 9. Otherwise, mode is "shared"; return true if no lock in held has mode "exclusive" and has name equal to
        //    name, and false otherwise.
        !held
            .iter()
            .any(|lock| lock.mode == LockMode::Exclusive && &lock.name == name)
    }
}
