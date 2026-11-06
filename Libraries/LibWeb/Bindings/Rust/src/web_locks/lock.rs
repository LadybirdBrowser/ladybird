/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Value, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, resolve_promise};
use crate::bindings::{CallContext, WebIdlResult};
use crate::web_locks::abstract_operations::queue_web_locks_task;
use crate::web_locks::lock_manager::{LockManager, LockMode};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Lock {
    lock: LockDataRef,
}

impl Wrappable for Lock {}

impl Lock {
    pub(crate) fn create(vm: &Vm, lock: LockDataRef) -> Ref<Self> {
        allocate(vm, |_| Self { lock })
    }

    // https://w3c.github.io/web-locks/#dom-lock-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The name getter’s steps are to return the associated lock’s name.
        Ok(self.lock.name.clone())
    }

    // https://w3c.github.io/web-locks/#dom-lock-mode
    pub(crate) fn mode(&self, _: &mut CallContext) -> WebIdlResult<LockMode> {
        // The mode getter’s steps are to return the associated lock’s mode.
        Ok(self.lock.mode)
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct LockData {
    #[gc(untraced)]
    pub(crate) client_id: Utf16String,
    pub(crate) manager: Ref<LockManager>,
    #[gc(untraced)]
    pub(crate) name: Utf16String,
    #[gc(untraced)]
    pub(crate) mode: LockMode,
    pub(crate) released_promise: WebIdlPromise,
    pub(crate) waiting_promise: WebIdlPromise,
}

pub(crate) static LOCK_DATA_CLASS: EmbedderClass<LockData> = EmbedderClass::new("LockData");

impl EmbedderValue for LockData {
    const CLASS: &'static EmbedderClass<Self> = &LOCK_DATA_CLASS;
}

pub(crate) type LockDataRef = Gc<EmbedderCell<LockData>>;

impl LockData {
    // https://w3c.github.io/web-locks/#release-the-lock
    pub(crate) fn release_lock(this: LockDataRef, vm: &Vm) {
        // 1. Assert: these steps are running on the lock task queue.

        // 2. Let manager be lock’s manager.
        let manager = this.manager;

        // 3. Let queueMap be manager’s lock request queue map.
        let queue_map = &manager.lock_request_queue_map;

        // 4. Let name be lock’s resource name.
        let name = &this.name;

        // 5. Let queue be the result of getting the lock request queue from queueMap for name.
        let queue = queue_map.borrow_mut(vm).get_lock_request_queue(vm, name);

        // 6. Remove lock from the manager’s held lock set.
        manager.held_lock_set.borrow_mut(vm).retain(|lock| *lock != this);

        // 7. Process the lock request queue queue.
        LockManager::process_lock_request_queue(vm, queue);
    }

    // https://w3c.github.io/web-locks/#concept-lock
    pub(crate) fn waiting_promise_settled(cx: &CallContext, lock: LockDataRef, _: Value) -> WebIdlResult<Value> {
        // When lock lock’s waiting promise settles (fulfills or rejects), enqueue the following steps on the lock task
        // queue:
        queue_web_locks_task(cx.vm(), lock.manager.realm, lock, |vm, lock| {
            // 1. Release the lock lock.
            Self::release_lock(lock, vm);

            // 2. Resolve lock’s released promise with lock’s waiting promise.
            resolve_promise(
                vm,
                lock.released_promise,
                Value::from_object(lock.waiting_promise.promise()),
            );
        });
        Ok(Value::UNDEFINED)
    }
}
