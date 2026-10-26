/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBTransaction, https://w3c.github.io/IndexedDB/#transaction

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Realm, Trace, Vm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::indexed_db::cpp::{DomException, devtools_client_is_active, notify_devtools_of_committed_changes};
use crate::indexed_db::idb_database::{IDBDatabase, IDBTransactionDurability};
use crate::indexed_db::idb_index::IDBIndex;
use crate::indexed_db::idb_object_store::{IDBObjectStore, invalid_state_error, not_found_error};
use crate::indexed_db::idb_request::AnyIDBRequest;
use crate::indexed_db::internal::algorithms::{abort_a_transaction, commit_a_transaction, create_a_sorted_name_list};
use crate::indexed_db::internal::mutation_log::{MutationLog, MutationLogRef, TransactionChanges};
use crate::indexed_db::internal::object_store::ObjectStoreRef;
use crate::indexed_db::internal::request_list::RequestList;

/// The enumeration IDBTransactionMode of IndexedDB/IDBTransaction.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum IDBTransactionMode {
    Readonly,
    Readwrite,
    Versionchange,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum TransactionState {
    Active,
    Inactive,
    Committing,
    Finished,
}

// AD-HOC: Per-store mutation logs for data and schema change reversion.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct StoreMutationLog {
    store: ObjectStoreRef,
    log: MutationLogRef,
}

// https://w3c.github.io/IndexedDB/#transaction
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBTransaction {
    event_target: CppEventTarget,

    // AD-HOC: The transaction has a connection
    connection: Ref<IDBDatabase>,

    // A transaction has a mode that determines which types of interactions can be performed upon that transaction.
    #[gc(untraced)]
    mode: IDBTransactionMode,

    // A transaction has a durability hint. This is a hint to the user agent of whether to prioritize performance or durability when committing the transaction.
    #[gc(untraced)]
    durability: IDBTransactionDurability,

    // A transaction has a state
    #[gc(untraced)]
    state: Cell<TransactionState>,

    // A transaction has a error which is set if the transaction is aborted.
    error: GcRefCell<Option<DomException>>,

    // A transaction has an associated upgrade request
    associated_request: Cell<Option<AnyIDBRequest>>,

    // AD-HOC: We need to track abort state separately, since we cannot rely on only the error.
    aborted: Cell<bool>,

    // A transaction has a scope which is a set of object stores that the transaction may interact with.
    scope: GcRefCell<Vec<ObjectStoreRef>>,

    // A transaction has a request list of pending requests which have been made against the transaction.
    request_list: RequestList,

    // A transaction optionally has a cleanup event loop which is an event loop.
    cleanup_event_loop: GcRefCell<Option<CppCell>>,

    object_store_handles: GcRefCell<Vec<(ObjectStoreRef, Ref<IDBObjectStore>)>>,
    index_handles: GcRefCell<Vec<Ref<IDBIndex>>>,

    store_mutation_logs: GcRefCell<Vec<StoreMutationLog>>,
    original_version: Cell<u64>,
}

impl Wrappable for IDBTransaction {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for IDBTransaction {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        // https://w3c.github.io/IndexedDB/#transaction-construct
        // A transaction’s get the parent algorithm returns the transaction’s connection.
        Some(self.connection.event_target().clone())
    }
}

impl IDBTransaction {
    pub(crate) fn create(
        vm: &Vm,
        connection: Ref<IDBDatabase>,
        mode: IDBTransactionMode,
        durability: IDBTransactionDurability,
        scope: &[ObjectStoreRef],
    ) -> Ref<IDBTransaction> {
        let transaction = allocate(vm, |defer| IDBTransaction {
            event_target: CppEventTarget::new(),
            connection,
            mode,
            durability,
            state: Cell::new(TransactionState::Active),
            error: GcRefCell::new(None),
            associated_request: Cell::new(None),
            aborted: Cell::new(false),
            scope: GcRefCell::new(scope.to_vec()),
            request_list: RequestList::new(defer),
            cleanup_event_loop: GcRefCell::new(None),
            object_store_handles: GcRefCell::new(Vec::new()),
            index_handles: GcRefCell::new(Vec::new()),
            store_mutation_logs: GcRefCell::new(Vec::new()),
            original_version: Cell::new(0),
        });
        transaction.event_target.set_owner(transaction);
        connection.add_transaction(vm, transaction);
        transaction
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn state(&self) -> TransactionState {
        self.state.get()
    }

    pub(crate) fn error_object(&self) -> Option<DomException> {
        self.error.borrow().clone()
    }

    pub(crate) fn connection_object(&self) -> Ref<IDBDatabase> {
        self.connection
    }

    pub(crate) fn associated_request(&self) -> Option<AnyIDBRequest> {
        self.associated_request.get()
    }

    pub(crate) fn aborted(&self) -> bool {
        self.aborted.get()
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.connection.realm()
    }

    pub(crate) fn request_list(&self) -> &RequestList {
        &self.request_list
    }

    pub(crate) fn scope<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, ObjectStoreRef> {
        let scope = MarkedVec::new(vm);
        scope.extend_from_slice(&self.scope.borrow());
        scope
    }

    pub(crate) fn cleanup_event_loop(&self) -> Option<*mut c_void> {
        self.cleanup_event_loop.borrow().as_ref().map(CppCell::as_ptr)
    }

    pub(crate) fn set_error(&self, vm: &Vm, error: Option<DomException>) {
        *self.error.borrow_mut(vm) = error;
    }

    pub(crate) fn set_associated_request(&self, request: Option<AnyIDBRequest>) {
        self.associated_request.set(request);
    }

    pub(crate) fn set_aborted(&self, aborted: bool) {
        self.aborted.set(aborted);
    }

    pub(crate) fn set_cleanup_event_loop(&self, vm: &Vm, event_loop: Option<CppCell>) {
        *self.cleanup_event_loop.borrow_mut(vm) = event_loop;
    }

    pub(crate) fn set_state(&self, state: TransactionState) {
        self.state.set(state);
    }

    pub(crate) fn is_upgrade_transaction(&self) -> bool {
        self.mode == IDBTransactionMode::Versionchange
    }

    pub(crate) fn is_readonly(&self) -> bool {
        self.mode == IDBTransactionMode::Readonly
    }

    pub(crate) fn is_finished(&self) -> bool {
        self.state.get() == TransactionState::Finished
    }

    pub(crate) fn is_active(&self) -> bool {
        self.state.get() == TransactionState::Active
    }

    pub(crate) fn object_store_named(&self, name: &Utf16String) -> Option<ObjectStoreRef> {
        self.scope.borrow().iter().find(|store| store.name() == *name).copied()
    }

    pub(crate) fn add_to_scope(&self, vm: &Vm, object_store: ObjectStoreRef) {
        self.scope.borrow_mut(vm).push(object_store);
    }

    pub(crate) fn remove_from_scope(&self, vm: &Vm, object_store: ObjectStoreRef) {
        let mut scope = self.scope.borrow_mut(vm);
        if let Some(position) = scope.iter().position(|other| *other == object_store) {
            scope.remove(position);
        }
    }

    pub(crate) fn set_scope(&self, vm: &Vm, scope: &[ObjectStoreRef]) {
        *self.scope.borrow_mut(vm) = scope.to_vec();
    }

    pub(crate) fn get_or_create_object_store_handle(
        this: Ref<Self>,
        vm: &Vm,
        store: ObjectStoreRef,
    ) -> Ref<IDBObjectStore> {
        if let Some(handle) = this.object_store_handle_for(store) {
            return handle;
        }

        let handle = IDBObjectStore::create(vm, store, this);
        this.object_store_handles.borrow_mut(vm).push((store, handle));
        handle
    }

    pub(crate) fn object_store_handle_for(&self, store: ObjectStoreRef) -> Option<Ref<IDBObjectStore>> {
        self.object_store_handles
            .borrow()
            .iter()
            .find(|(entry, _)| *entry == store)
            .map(|(_, handle)| *handle)
    }

    pub(crate) fn object_store_handles<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Ref<IDBObjectStore>> {
        let handles = MarkedVec::new(vm);
        for (_, handle) in self.object_store_handles.borrow().iter() {
            handles.push(*handle);
        }
        handles
    }

    pub(crate) fn register_index_handle(&self, vm: &Vm, handle: Ref<IDBIndex>) {
        self.index_handles.borrow_mut(vm).push(handle);
    }

    pub(crate) fn index_handles<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Ref<IDBIndex>> {
        let handles = MarkedVec::new(vm);
        handles.extend_from_slice(&self.index_handles.borrow());
        handles
    }

    // Mutation log management. Logs are per-store and track both data and schema mutations.
    pub(crate) fn set_up_mutation_logs(&self, vm: &Vm) {
        self.original_version
            .set(self.connection.associated_database().version());

        for store in self.scope(vm).values().iter() {
            let log = MutationLog::create(vm);
            store.set_mutation_log(Some(log));
            self.store_mutation_logs
                .borrow_mut(vm)
                .push(StoreMutationLog { store: *store, log });
        }
    }

    pub(crate) fn original_version(&self) -> u64 {
        self.original_version.get()
    }

    pub(crate) fn set_up_mutation_log_for_new_store(&self, vm: &Vm, store: ObjectStoreRef) {
        let log = MutationLog::create(vm);
        store.set_mutation_log(Some(log));
        self.store_mutation_logs
            .borrow_mut(vm)
            .push(StoreMutationLog { store, log });
        log.note_object_store_created(vm);
    }

    pub(crate) fn revert_all_mutations(&self, vm: &Vm) {
        let database = self.connection.associated_database();
        let entries = MarkedVec::new(vm);
        for entry in self.store_mutation_logs.borrow().iter() {
            entries.push((entry.store, entry.log));
        }
        for (store, log) in entries.values().iter().rev() {
            log.revert(vm, *store, database, self.connection);
        }

        database.set_version(self.original_version.get());
        self.connection.set_version(self.original_version.get());
    }

    pub(crate) fn discard_mutation_logs(&self, vm: &Vm) {
        for entry in self.store_mutation_logs.borrow().iter() {
            entry.store.set_mutation_log(None);
        }
        self.store_mutation_logs.borrow_mut(vm).clear();
    }

    pub(crate) fn notify_devtools_of_committed_changes(&self) {
        if !devtools_client_is_active(self.realm()) {
            return;
        }

        let mut changes = TransactionChanges::default();
        let database_name = self.connection.associated_database().name();
        for entry in self.store_mutation_logs.borrow().iter() {
            entry
                .log
                .append_changes(&database_name, &entry.store.name(), &mut changes);
        }
        if changes.is_empty() {
            return;
        }

        notify_devtools_of_committed_changes(self.realm(), &changes);
    }

    // https://w3c.github.io/IndexedDB/#dom-idbtransaction-objectstorenames
    pub(crate) fn object_store_names(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // 1. Let names be a list of the names of the object stores in this's scope.
        let names = self
            .scope
            .borrow()
            .iter()
            .map(|object_store| object_store.name())
            .collect();

        // 2. Return the result (a DOMStringList) of creating a sorted name list with names.
        Ok(create_a_sorted_name_list(cx, names))
    }

    pub(crate) fn mode(&self, _: &mut CallContext) -> WebIdlResult<IDBTransactionMode> {
        Ok(self.mode)
    }

    pub(crate) fn durability(&self, _: &mut CallContext) -> WebIdlResult<IDBTransactionDurability> {
        Ok(self.durability)
    }

    pub(crate) fn connection(&self, _: &mut CallContext) -> WebIdlResult<Ref<IDBDatabase>> {
        Ok(self.connection)
    }

    pub(crate) fn error(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        Ok(self.error_object().map(|error| error.wrapper(cx)))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbtransaction-abort
    pub(crate) fn abort(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this's state is committing or finished, then throw an "InvalidStateError" DOMException.
        if this.state.get() == TransactionState::Committing || this.state.get() == TransactionState::Finished {
            return Err(invalid_state_error("Transaction is ending"));
        }

        // 2. Run abort a transaction with this and null.
        abort_a_transaction(cx.vm(), *this, None);
        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbtransaction-commit
    pub(crate) fn commit(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. If this's state is not active, then throw an "InvalidStateError" DOMException.
        if this.state.get() != TransactionState::Active {
            return Err(invalid_state_error("Transaction is not active while committing"));
        }

        // 2. Run commit a transaction with this.
        commit_a_transaction(cx.vm(), *this);

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbtransaction-objectstore
    pub(crate) fn object_store(
        this: &Ref<Self>,
        cx: &mut CallContext,
        name: Utf16String,
    ) -> WebIdlResult<Ref<IDBObjectStore>> {
        // 1. If this's state is finished, then throw an "InvalidStateError" DOMException.
        if this.state.get() == TransactionState::Finished {
            return Err(invalid_state_error("Transaction is finished"));
        }

        // 2. Let store be the object store named name in this's scope, or throw a "NotFoundError" DOMException if none.
        let Some(store) = this.object_store_named(&name) else {
            return Err(not_found_error("Object store not found in transactions scope"));
        };

        // 3. Return an object store handle associated with store and this.

        // https://w3c.github.io/IndexedDB/#object-store-handle-construct
        // Multiple handles may be associated with the same object store in different transactions,
        // but there must be only one object store handle associated with a particular object store
        // within a transaction.
        Ok(Self::get_or_create_object_store_handle(*this, cx.vm(), store))
    }
}
