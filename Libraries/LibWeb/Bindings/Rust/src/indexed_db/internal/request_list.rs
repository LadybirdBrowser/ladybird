/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The request lists of transactions, https://w3c.github.io/IndexedDB/#transaction-request-list

use core::cell::{Cell, RefCell};

use libjs_rust::api::{DeferGc, MarkedVec, Trace, Vm};

use crate::indexed_db::idb_request::{AnyIDBRequest, WeakIDBRequest};
use crate::indexed_db::internal::function::{Function, call_function};

#[derive(Trace)]
struct Entry {
    #[gc(untraced = "the request is weak")]
    request: WeakIDBRequest,
}

#[derive(Trace)]
pub(crate) struct RequestList {
    #[gc(untraced = "the requests are weak")]
    entries: RefCell<Vec<Entry>>,
    on_all_processed: Cell<Option<Function>>,
}

impl RequestList {
    pub(crate) fn new(_: &DeferGc<'_>) -> Self {
        Self {
            entries: RefCell::new(Vec::new()),
            on_all_processed: Cell::new(None),
        }
    }

    /// Adds a request whose operation the backend performs, and whose result arrives as an event, in the order the
    /// agent sent the operations.
    pub(crate) fn enqueue_remote(&self, vm: &Vm, request: AnyIDBRequest) {
        self.entries.borrow_mut().push(Entry {
            request: WeakIDBRequest::new(vm, request),
        });
    }

    pub(crate) fn remove(&self, _vm: &Vm, request: AnyIDBRequest) {
        let mut entries = self.entries.borrow_mut();
        if let Some(position) = entries.iter().position(|entry| entry.request.get() == Some(request)) {
            entries.remove(position);
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.entries.borrow().is_empty()
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
            let mut entries = self.entries.borrow_mut();
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
