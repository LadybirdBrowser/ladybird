/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The request lists of transactions, https://w3c.github.io/IndexedDB/#transaction-request-list, and of connection queues

use core::cell::Cell;

use libjs_rust::api::{DeferGc, GcRefCell, MarkedVec, Trace, Vm};

use crate::indexed_db::idb_request::{AnyIDBRequest, WeakIDBRequest};
use crate::indexed_db::internal::algorithms::queue_a_database_task;
use crate::indexed_db::internal::function::{Function, call_function};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct Entry {
    #[gc(untraced = "the request is weak")]
    request: WeakIDBRequest,
    steps: Option<Function>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct RequestList {
    entries: GcRefCell<Vec<Entry>>,
    on_all_processed: Cell<Option<Function>>,
    blocked: Cell<bool>,
}

impl RequestList {
    pub(crate) fn new(_: &DeferGc<'_>) -> Self {
        Self {
            entries: GcRefCell::new(Vec::new()),
            on_all_processed: Cell::new(None),
            blocked: Cell::new(false),
        }
    }

    pub(crate) fn enqueue(&self, vm: &Vm, request: AnyIDBRequest, steps: Function) {
        self.entries.borrow_mut(vm).push(Entry {
            request: WeakIDBRequest::new(vm, request),
            steps: Some(steps),
        });
        self.maybe_process_next_request(vm);
    }

    pub(crate) fn maybe_process_next_request(&self, vm: &Vm) {
        if self.blocked.get() {
            return;
        }

        let mut entries = self.entries.borrow_mut(vm);
        while entries.first().is_some_and(|entry| entry.request.get().is_none()) {
            entries.remove(0);
        }

        let mut steps_to_queue = None;
        for entry in entries.iter_mut() {
            let Some(request) = entry.request.get() else {
                continue;
            };
            if request.processed() {
                continue;
            }
            if request.aborted() {
                continue;
            }

            // If the steps are null here, the request is still executing, so we should wait until it is finished.
            let Some(steps) = entry.steps else {
                return;
            };

            steps_to_queue = Some(steps);
            entry.steps = None;
            break;
        }
        drop(entries);

        if let Some(steps) = steps_to_queue {
            queue_a_database_task(vm, steps);
        }
    }

    pub(crate) fn on_request_processed(&self, vm: &Vm) {
        self.maybe_process_next_request(vm);
    }

    pub(crate) fn remove(&self, vm: &Vm, request: AnyIDBRequest) {
        let mut entries = self.entries.borrow_mut(vm);
        if let Some(position) = entries.iter().position(|entry| entry.request.get() == Some(request)) {
            entries.remove(position);
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.entries.borrow().is_empty()
    }

    pub(crate) fn block_execution(&self) {
        self.blocked.set(true);
    }

    pub(crate) fn execution_is_blocked(&self) -> bool {
        self.blocked.get()
    }

    pub(crate) fn unblock_execution(&self, vm: &Vm) {
        self.blocked.set(false);
        self.maybe_process_next_request(vm);
    }

    pub(crate) fn set_on_all_processed(&self, vm: &Vm, callback: Function) {
        self.on_all_processed.set(Some(callback));
        self.check_all_processed(vm);
    }

    pub(crate) fn clear_on_all_processed(&self) {
        self.on_all_processed.set(None);
    }

    pub(crate) fn check_all_processed(&self, vm: &Vm) {
        if self.on_all_processed.get().is_none() {
            return;
        }

        {
            let mut entries = self.entries.borrow_mut(vm);
            entries.retain(|entry| entry.request.get().is_some());

            for entry in entries.iter() {
                if !entry.request.get().expect("the request is live").processed() {
                    return;
                }
            }
        }

        let callback = self.on_all_processed.take().expect("a callback is set");
        call_function(vm, callback);
    }

    pub(crate) fn requests<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, AnyIDBRequest> {
        let requests = MarkedVec::new(vm);
        for entry in self.entries.borrow().iter() {
            if let Some(request) = entry.request.get() {
                requests.push(request);
            }
        }
        requests
    }
}
