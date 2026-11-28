/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Realm, Trace, Vm};

use crate::bindings::callback::PromiseCallbackFunction;
use crate::bindings::interface::Ref;
use crate::bindings::promise::{WebIdlPromise, reject_promise};
use crate::dom::abort_signal::AbortSignal;
use crate::web_locks::abstract_operations::queue_web_locks_task;
use crate::web_locks::agent::{act_on_notices, agent_id, take_request};
use crate::web_locks::lock_manager::LockMode;
use crate::web_locks::lock_table::{self, AgentId, Entry, EntryId, LockManagerKey};

// https://w3c.github.io/web-locks/#lock-request
// NB: The lock table has the request's agent, clientId, manager, name and mode as well, under its id.
#[derive(Trace)]
pub(crate) struct LockRequest {
    #[gc(untraced)]
    pub(crate) id: EntryId,
    #[gc(untraced)]
    pub(crate) agent: AgentId,
    #[gc(untraced)]
    pub(crate) client_id: Utf16String,
    #[gc(untraced)]
    pub(crate) manager_key: LockManagerKey,
    /// The realm of the LockManager object that the request came from, which the objects for the request are made in.
    pub(crate) realm: Gc<Realm>,
    #[gc(untraced)]
    pub(crate) name: Utf16String,
    #[gc(untraced)]
    pub(crate) mode: LockMode,
    pub(crate) callback: PromiseCallbackFunction,
    pub(crate) promise: WebIdlPromise,
    pub(crate) signal: Option<Ref<AbortSignal>>,
    pub(crate) abort_algorithm_id: Cell<Option<u64>>,
}

pub(crate) static LOCK_REQUEST_CLASS: EmbedderClass<LockRequest> = EmbedderClass::new("LockRequest");

impl EmbedderValue for LockRequest {
    const CLASS: &'static EmbedderClass<Self> = &LOCK_REQUEST_CLASS;
}

pub(crate) type LockRequestRef = Gc<EmbedderCell<LockRequest>>;

impl LockRequest {
    /// The entry of the request in the lock table.
    pub(crate) fn entry(&self) -> Entry {
        Entry {
            id: self.id,
            agent: self.agent,
            client_id: self.client_id.clone(),
            name: self.name.clone(),
            mode: self.mode,
        }
    }

    // https://w3c.github.io/web-locks/#abort-the-request
    pub(crate) fn abort_request(this: LockRequestRef, vm: &Vm) {
        // 1. Assert: these steps are running on the lock task queue.

        // 2. Let manager be request’s manager.
        // 3. Let name be request’s name.
        // 4. Let queueMap be manager’s lock request queue map.
        // 5. Let queue be the result of getting the lock request queue from queueMap for name.
        // 6. Remove request from queue.
        // 7. Process the lock request queue queue.
        // NB: A request that the table already granted keeps its objects, for its grant to see that it was aborted.
        if lock_table::abort_request(&this.manager_key, &this.name, this.id, agent_id()) {
            take_request(vm, this.id);
        }
        act_on_notices(vm);
    }

    // https://w3c.github.io/web-locks/#signal-to-abort-the-request
    pub(crate) fn signal_to_abort_request(this: LockRequestRef, vm: &Vm, signal: &AbortSignal) {
        // 1. Enqueue the steps to abort the request request to the lock task queue.
        queue_web_locks_task(vm, this.realm, this, |vm, request| {
            Self::abort_request(request, vm);
        });

        // 2. Reject request’s promise with signal’s abort reason.
        reject_promise(vm, this.promise, signal.reason());
    }
}
