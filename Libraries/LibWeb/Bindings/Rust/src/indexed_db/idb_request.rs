/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBRequest, https://w3c.github.io/IndexedDB/#request-api

use core::cell::Cell;
use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, Gc, GcRefCell, GcWeak, Realm, Trace, Value, Vm};

use crate::bindings::conversions::{CppInterface, ToJs};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Inherits, Ref, Wrappable, Wrapped, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::cpp::DomException;
use crate::indexed_db::idb_cursor::AnyIDBCursor;
use crate::indexed_db::idb_index::IDBIndex;
use crate::indexed_db::idb_object_store::IDBObjectStore;
use crate::indexed_db::idb_open_db_request::IDBOpenDBRequest;
use crate::indexed_db::idb_transaction::IDBTransaction;

/// The enumeration IDBRequestReadyState of IndexedDB/IDBRequest.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum IDBRequestReadyState {
    Pending,
    Done,
}

/// The union (IDBObjectStore or IDBIndex or IDBCursor) of IndexedDB/IDBRequest.idl.
#[allow(
    clippy::enum_variant_names,
    reason = "the variants have the names of the interfaces of the union"
)]
pub(crate) enum IDBObjectStoreOrIDBIndexOrIDBCursor {
    IDBObjectStore(Ref<IDBObjectStore>),
    IDBIndex(Ref<IDBIndex>),
    IDBCursor(AnyIDBCursor),
}

/// The source of a request: an object store handle, an index handle, or a cursor, or none for an open request.
#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum IDBRequestSource {
    Empty,
    ObjectStore(Ref<IDBObjectStore>),
    Index(Ref<IDBIndex>),
    Cursor(AnyIDBCursor),
}

// https://w3c.github.io/IndexedDB/#request-api
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBRequest {
    event_target: CppEventTarget,

    // A request has a processed flag which is initially false.
    processed: Cell<bool>,

    // A request has a done flag which is initially false.
    done: Cell<bool>,

    // A request has a result and an error
    result: Cell<Value>,
    error: GcRefCell<Option<DomException>>,

    // A request has a source object.
    source: Cell<IDBRequestSource>,
    realm: Gc<Realm>,

    // A request has a transaction which is initially null.
    transaction: Cell<Option<Ref<IDBTransaction>>>,

    // AD-HOC: Set to true to prevent queued operations and result events from executing.
    aborted: Cell<bool>,
}

impl Wrappable for IDBRequest {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for IDBRequest {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        // https://w3c.github.io/IndexedDB/#request-construct
        // A request’s get the parent algorithm returns the request’s transaction.
        self.transaction
            .get()
            .map(|transaction| transaction.event_target().clone())
    }
}

impl IDBRequest {
    pub(crate) fn new(realm: Gc<Realm>, source: IDBRequestSource) -> Self {
        Self {
            event_target: CppEventTarget::new(),
            processed: Cell::new(false),
            done: Cell::new(false),
            result: Cell::new(Value::UNDEFINED),
            error: GcRefCell::new(None),
            source: Cell::new(source),
            realm,
            transaction: Cell::new(None),
            aborted: Cell::new(false),
        }
    }

    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, source: IDBRequestSource) -> Ref<IDBRequest> {
        let request = allocate(vm, |_| IDBRequest::new(realm, source));
        request.event_target.set_owner(request);
        request
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn done(&self) -> bool {
        self.done.get()
    }

    pub(crate) fn processed(&self) -> bool {
        self.processed.get()
    }

    pub(crate) fn transaction_object(&self) -> Option<Ref<IDBTransaction>> {
        self.transaction.get()
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub(crate) fn set_done(&self, done: bool) {
        self.done.set(done);
    }

    pub(crate) fn set_result(&self, result: Value) {
        self.result.set(result);
    }

    pub(crate) fn set_error(&self, vm: &Vm, error: Option<DomException>) {
        *self.error.borrow_mut(vm) = error;
    }

    pub(crate) fn set_processed(&self, processed: bool) {
        self.processed.set(processed);
    }

    pub(crate) fn set_transaction(&self, transaction: Option<Ref<IDBTransaction>>) {
        self.transaction.set(transaction);
    }

    pub(crate) fn has_error(&self) -> bool {
        self.error.borrow().is_some()
    }

    pub(crate) fn aborted(&self) -> bool {
        self.aborted.get()
    }

    pub(crate) fn set_aborted(&self, aborted: bool) {
        self.aborted.set(aborted);
    }

    /// The error, without the done flag check of the getter.
    pub(crate) fn error_object(&self) -> Option<DomException> {
        self.error.borrow().clone()
    }

    // https://w3c.github.io/IndexedDB/#dom-idbrequest-result
    pub(crate) fn result(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
        if !self.done() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("The request is not done"),
            ));
        }

        // 2. Return this's result, or undefined if the request resulted in an error.
        if self.has_error() {
            return Ok(Value::UNDEFINED);
        }

        Ok(self.result.get())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbrequest-error
    pub(crate) fn error(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
        if !self.done.get() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("The request is not done"),
            ));
        }

        // 2. Return this's error, or null if no error occurred.
        Ok(self.error_object().map(|error| error.wrapper(cx)))
    }

    pub(crate) fn source(&self, _: &mut CallContext) -> WebIdlResult<Option<IDBObjectStoreOrIDBIndexOrIDBCursor>> {
        Ok(match self.source.get() {
            IDBRequestSource::Empty => None,
            IDBRequestSource::ObjectStore(object_store) => {
                Some(IDBObjectStoreOrIDBIndexOrIDBCursor::IDBObjectStore(object_store))
            }
            IDBRequestSource::Index(index) => Some(IDBObjectStoreOrIDBIndexOrIDBCursor::IDBIndex(index)),
            IDBRequestSource::Cursor(cursor) => Some(IDBObjectStoreOrIDBIndexOrIDBCursor::IDBCursor(cursor)),
        })
    }

    pub(crate) fn transaction(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<IDBTransaction>>> {
        Ok(self.transaction.get())
    }

    pub(crate) fn ready_state(&self, _: &mut CallContext) -> WebIdlResult<IDBRequestReadyState> {
        // The readyState getter steps are to return "pending" if this's done flag is false, and "done" otherwise.
        Ok(if self.done() {
            IDBRequestReadyState::Done
        } else {
            IDBRequestReadyState::Pending
        })
    }
}

#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AnyIDBRequest {
    IDBRequest(Ref<IDBRequest>),
    IDBOpenDBRequest(Ref<IDBOpenDBRequest>),
}

impl Deref for AnyIDBRequest {
    type Target = IDBRequest;

    fn deref(&self) -> &IDBRequest {
        match self {
            Self::IDBRequest(request) => request,
            Self::IDBOpenDBRequest(request) => {
                let request: &IDBOpenDBRequest = request;
                request.parent()
            }
        }
    }
}

impl ToJs for AnyIDBRequest {
    fn to_js(self, cx: &CallContext) -> libjs_rust::api::ThrowCompletionOr<Value> {
        match self {
            Self::IDBRequest(request) => request.to_js(cx),
            Self::IDBOpenDBRequest(request) => request.to_js(cx),
        }
    }
}

pub(crate) enum WeakIDBRequest {
    IDBRequest(GcWeak<EmbedderCell<Wrapped<IDBRequest>>>),
    IDBOpenDBRequest(GcWeak<EmbedderCell<Wrapped<IDBOpenDBRequest>>>),
}

impl WeakIDBRequest {
    pub(crate) fn new(vm: &Vm, request: AnyIDBRequest) -> Self {
        match request {
            AnyIDBRequest::IDBRequest(request) => Self::IDBRequest(GcWeak::new(vm.heap(), request)),
            AnyIDBRequest::IDBOpenDBRequest(request) => Self::IDBOpenDBRequest(GcWeak::new(vm.heap(), request)),
        }
    }

    pub(crate) fn get(&self) -> Option<AnyIDBRequest> {
        match self {
            Self::IDBRequest(request) => request.get().map(AnyIDBRequest::IDBRequest),
            Self::IDBOpenDBRequest(request) => request.get().map(AnyIDBRequest::IDBOpenDBRequest),
        }
    }
}
