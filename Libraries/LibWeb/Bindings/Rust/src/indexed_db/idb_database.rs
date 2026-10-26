/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBDatabase, https://w3c.github.io/IndexedDB/#database-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, MarkedVec, Realm, Trace, Vm};

use crate::bindings::conversions::{CppInterface, DOMStringOrDOMStringSequence};
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::cpp::{enqueue_post_gc_task, main_thread_event_loop};
use crate::indexed_db::idb_object_store::{
    IDBObjectStore, constraint_error, invalid_state_error, not_found_error, transaction_inactive_error,
};
use crate::indexed_db::idb_transaction::{IDBTransaction, IDBTransactionMode, TransactionState};
use crate::indexed_db::internal::algorithms::{
    KeyPath, close_a_database_connection, commit_a_transaction, create_a_sorted_name_list, is_valid_key_path,
    queue_a_database_task,
};
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::function::{Function, call_function, create_function};
use crate::indexed_db::internal::object_store::{ObjectStore, ObjectStoreRef};

/// The enumeration IDBTransactionDurability of IndexedDB/IDBDatabase.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum IDBTransactionDurability {
    Default,
    Strict,
    Relaxed,
}

/// The dictionary IDBTransactionOptions of IndexedDB/IDBDatabase.idl.
pub(crate) struct IDBTransactionOptions {
    pub(crate) durability: IDBTransactionDurability,
}

/// The dictionary IDBObjectStoreParameters of IndexedDB/IDBDatabase.idl.
pub(crate) struct IDBObjectStoreParameters {
    pub(crate) auto_increment: bool,
    pub(crate) key_path: Option<DOMStringOrDOMStringSequence>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ConnectionState {
    Open,
    Closed,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct PendingTransactionWait {
    transactions: Vec<Ref<IDBTransaction>>,
    callback: Function,
}

// https://w3c.github.io/IndexedDB/#IDBDatabase-interface
// https://w3c.github.io/IndexedDB/#database-connection
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBDatabase {
    event_target: CppEventTarget,

    pending_transaction_waits: GcRefCell<Vec<PendingTransactionWait>>,

    version: Cell<u64>,
    name: Utf16String,

    // Each connection has a close pending flag which is initially false.
    close_pending: Cell<bool>,

    // When a connection is initially created it is in an opened state.
    #[gc(untraced)]
    state: Cell<ConnectionState>,

    // A connection has an object store set, which is initialized to the set of object stores in the associated database when the connection is created.
    // The contents of the set will remain constant except when an upgrade transaction is live.
    object_store_set: GcRefCell<Vec<ObjectStoreRef>>,

    // NOTE: There is an associated database in the spec, but there is no mention where it is assigned, nor where its from
    //       So we stash the one we have when opening a connection.
    associated_database: DatabaseRef,
    realm: Gc<Realm>,

    // NOTE: We need to keep track of what transactions were created by this connection
    transactions: GcRefCell<Vec<Ref<IDBTransaction>>>,
}

impl Wrappable for IDBDatabase {
    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        object.associated_database.dissociate(object);
        enqueue_post_gc_task(object.associated_database);
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for IDBDatabase {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl IDBDatabase {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>, db: DatabaseRef) -> Ref<IDBDatabase> {
        let object_stores = db.object_stores(vm);
        let connection = allocate(vm, |_| IDBDatabase {
            event_target: CppEventTarget::new(),
            pending_transaction_waits: GcRefCell::new(Vec::new()),
            version: Cell::new(0),
            name: db.name(),
            close_pending: Cell::new(false),
            state: Cell::new(ConnectionState::Open),
            object_store_set: GcRefCell::new(object_stores.to_vec()),
            associated_database: db,
            realm,
            transactions: GcRefCell::new(Vec::new()),
        });
        connection.event_target.set_owner(connection);
        db.associate(vm, connection);
        connection
    }

    pub(crate) fn event_target(&self) -> &CppEventTarget {
        &self.event_target
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub(crate) fn set_version(&self, version: u64) {
        self.version.set(version);
    }

    pub(crate) fn set_close_pending(&self, close_pending: bool) {
        self.close_pending.set(close_pending);
    }

    pub(crate) fn set_state(&self, state: ConnectionState) {
        self.state.set(state);
    }

    pub(crate) fn close_pending(&self) -> bool {
        self.close_pending.get()
    }

    pub(crate) fn state(&self) -> ConnectionState {
        self.state.get()
    }

    pub(crate) fn associated_database(&self) -> DatabaseRef {
        self.associated_database
    }

    pub(crate) fn object_store_set<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, ObjectStoreRef> {
        let object_store_set = MarkedVec::new(vm);
        object_store_set.extend_from_slice(&self.object_store_set.borrow());
        object_store_set
    }

    pub(crate) fn add_to_object_store_set(&self, vm: &Vm, object_store: ObjectStoreRef) {
        self.object_store_set.borrow_mut(vm).push(object_store);
    }

    pub(crate) fn remove_from_object_store_set(&self, vm: &Vm, object_store: ObjectStoreRef) {
        let mut object_store_set = self.object_store_set.borrow_mut(vm);
        if let Some(position) = object_store_set.iter().position(|entry| *entry == object_store) {
            object_store_set.remove(position);
        }
    }

    pub(crate) fn transactions<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Ref<IDBTransaction>> {
        let transactions = MarkedVec::new(vm);
        transactions.extend_from_slice(&self.transactions.borrow());
        transactions
    }

    pub(crate) fn add_transaction(&self, vm: &Vm, transaction: Ref<IDBTransaction>) {
        self.transactions.borrow_mut(vm).push(transaction);
    }

    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.name.clone())
    }

    pub(crate) fn version(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        Ok(self.version.get())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbdatabase-close
    pub(crate) fn close(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<()> {
        // 1. Run close a database connection with this connection.
        close_a_database_connection(cx.vm(), *this, None, false);
        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
    pub(crate) fn create_object_store(
        this: &Ref<Self>,
        cx: &mut CallContext,
        name: Utf16String,
        options: IDBObjectStoreParameters,
    ) -> WebIdlResult<Ref<IDBObjectStore>> {
        let vm = cx.vm();

        // 1. Let database be this's associated database.
        let database = this.associated_database;

        // 2. Let transaction be database’s upgrade transaction if it is not null, or throw an "InvalidStateError" DOMException otherwise.
        let Some(transaction) = database.upgrade_transaction() else {
            return Err(invalid_state_error("Upgrade transaction is null"));
        };

        // 3. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while creating object store",
            ));
        }

        // 4. Let keyPath be options’s keyPath member if it is not undefined or null, or null otherwise.
        let key_path = options.key_path.map(|key_path| match key_path {
            DOMStringOrDOMStringSequence::DOMString(value) => KeyPath::String(value),
            DOMStringOrDOMStringSequence::Sequence(values) => KeyPath::Sequence(values),
        });

        // 5. If keyPath is not null and is not a valid key path, throw a "SyntaxError" DOMException.
        if let Some(key_path) = &key_path
            && !is_valid_key_path(key_path)
        {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_utf8("Invalid key path"),
            ));
        }

        // 6. If an object store named name already exists in database throw a "ConstraintError" DOMException.
        if database.object_store_with_name(&name).is_some() {
            return Err(constraint_error("Object store already exists"));
        }

        // 7. Let autoIncrement be options’s autoIncrement member.
        let auto_increment = options.auto_increment;

        let is_empty_key_path_or_sequence = match &key_path {
            Some(KeyPath::String(value)) => value.is_empty(),
            Some(KeyPath::Sequence(_)) => true,
            None => false,
        };

        // 8. If autoIncrement is true and keyPath is an empty string or any sequence (empty or otherwise), throw an "InvalidAccessError" DOMException.
        if auto_increment && is_empty_key_path_or_sequence {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8("Auto increment is true and key path is empty or sequence"),
            ));
        }

        // 9. Let store be a new object store in database.
        //    Set the created object store's name to name.
        //    If autoIncrement is true, then the created object store uses a key generator.
        //    If keyPath is not null, set the created object store's key path to keyPath.
        let object_store = ObjectStore::create(vm, database, name, auto_increment, key_path);

        // AD-HOC: Add newly created object store to this's object store set.
        this.add_to_object_store_set(vm, object_store);

        // AD-HOC: Set up a mutation log for this store and log its creation for potential revert on abort.
        transaction.set_up_mutation_log_for_new_store(vm, object_store);

        // 10. Return a new object store handle associated with store and transaction.
        transaction.add_to_scope(vm, object_store);
        Ok(IDBTransaction::get_or_create_object_store_handle(
            transaction,
            vm,
            object_store,
        ))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbdatabase-objectstorenames
    pub(crate) fn object_store_names(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // 1. Let names be a list of the names of the object stores in this's object store set.
        let names = self
            .object_store_set
            .borrow()
            .iter()
            .map(|object_store| object_store.name())
            .collect();

        // 2. Return the result (a DOMStringList) of creating a sorted name list with names.
        Ok(create_a_sorted_name_list(cx, names))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbdatabase-deleteobjectstore
    pub(crate) fn delete_object_store(&self, cx: &mut CallContext, name: Utf16String) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let database be this's associated database.
        let database = self.associated_database;

        // 2. Let transaction be database’s upgrade transaction if it is not null, or throw an "InvalidStateError" DOMException otherwise.
        let Some(transaction) = database.upgrade_transaction() else {
            return Err(invalid_state_error("Upgrade transaction is null"));
        };

        // 3. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while deleting object store",
            ));
        }

        // 4. Let store be the object store named name in database, or throw a "NotFoundError" DOMException if none.
        let Some(store) = database.object_store_with_name(&name) else {
            return Err(not_found_error("Object store not found while trying to delete"));
        };

        // 5. Remove store from this's object store set.
        self.remove_from_object_store_set(vm, store);

        // NB: Upgrade transactions' scope is always the entire database. Since we removed this store from the database,
        //     it no longer belongs in the scope.
        transaction.remove_from_scope(vm, store);

        // 6. If there is an object store handle associated with store and transaction, remove all entries from its index set.
        if let Some(handle) = transaction.object_store_handle_for(store) {
            handle.index_set_clear(vm);
        }

        // AD-HOC: Mark the store and its indexes as deleted so that stale handles throw InvalidStateError.
        store.set_deleted(true);
        for index in store.indexes(vm).values().iter() {
            index.set_deleted(true);
        }

        // AD-HOC: Log the deletion for potential revert on abort.
        store
            .mutation_log()
            .expect("an upgrade transaction logs the mutations of its stores")
            .note_object_store_deleted(vm);

        // 7. Destroy store.
        Database::remove_object_store(database, vm, store);

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbdatabase-transaction
    pub(crate) fn transaction(
        this: &Ref<Self>,
        cx: &mut CallContext,
        store_names: DOMStringOrDOMStringSequence,
        mode: IDBTransactionMode,
        options: IDBTransactionOptions,
    ) -> WebIdlResult<Ref<IDBTransaction>> {
        let vm = cx.vm();

        // 1. If a live upgrade transaction is associated with the connection, throw an "InvalidStateError" DOMException.
        let database = this.associated_database;
        if database.upgrade_transaction().is_some() {
            return Err(invalid_state_error("Upgrade transaction is live"));
        }

        // 2. If this's close pending flag is true, then throw an "InvalidStateError" DOMException.
        if this.close_pending() {
            return Err(invalid_state_error("Close pending"));
        }

        // 3. Let scope be the set of unique strings in storeNames if it is a sequence, or a set containing one string equal to storeNames otherwise.
        let mut scope: Vec<Utf16String> = Vec::new();
        match store_names {
            DOMStringOrDOMStringSequence::Sequence(store_names) => {
                for store_name in store_names {
                    if !scope.contains(&store_name) {
                        scope.push(store_name);
                    }
                }
            }
            DOMStringOrDOMStringSequence::DOMString(store_name) => {
                scope.push(store_name);
            }
        }

        // 4. If any string in scope is not the name of an object store in the connected database, throw a "NotFoundError" DOMException.
        for store_name in &scope {
            if database.object_store_with_name(store_name).is_none() {
                return Err(not_found_error(
                    "Provided object store names does not exist in database",
                ));
            }
        }

        // 5. If scope is empty, throw an "InvalidAccessError" DOMException.
        if scope.is_empty() {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8("Scope is empty"),
            ));
        }

        // 6. If mode is not "readonly" or "readwrite", throw a TypeError.
        if mode != IDBTransactionMode::Readonly && mode != IDBTransactionMode::Readwrite {
            return Err(Exception::type_error("Invalid transaction mode"));
        }

        // 7. Let transaction be a newly created transaction with this connection, mode, options’ durability member, and the set of object stores named in scope.
        let scope_stores = MarkedVec::new(vm);
        for store_name in &scope {
            let store = database
                .object_store_with_name(store_name)
                .expect("every name in scope is an object store");
            scope_stores.push(store);
        }

        let transaction = IDBTransaction::create(vm, *this, mode, options.durability, &scope_stores.values());

        // 8. Set transaction’s cleanup event loop to the current event loop.
        transaction.set_cleanup_event_loop(vm, Some(main_thread_event_loop()));

        Self::block_on_conflicting_transactions(*this, vm, transaction);

        // 9. Return an IDBTransaction object representing transaction.
        Ok(transaction)
    }

    pub(crate) fn wait_for_transactions_to_finish(
        &self,
        vm: &Vm,
        transactions: &[Ref<IDBTransaction>],
        on_complete: Function,
    ) {
        let all_finished = transactions.iter().all(|entry| entry.is_finished());

        if all_finished {
            queue_a_database_task(vm, on_complete);
            return;
        }

        self.pending_transaction_waits
            .borrow_mut(vm)
            .push(PendingTransactionWait {
                transactions: transactions.to_vec(),
                callback: on_complete,
            });
    }

    pub(crate) fn check_pending_transaction_waits(&self, vm: &Vm) {
        let mut i = 0;
        loop {
            let callback = {
                let mut waits = self.pending_transaction_waits.borrow_mut(vm);
                if i >= waits.len() {
                    break;
                }
                let all_finished = waits[i]
                    .transactions
                    .iter()
                    .all(|transaction| transaction.is_finished());
                if !all_finished {
                    i += 1;
                    continue;
                }
                waits.remove(i).callback
            };
            call_function(vm, callback);
        }
    }

    // https://w3c.github.io/IndexedDB/#transaction-scheduling
    pub(crate) fn block_on_conflicting_transactions(this: Ref<Self>, vm: &Vm, transaction: Ref<IDBTransaction>) {
        // The following constraints define when a transaction can be started:

        // - A read-only transactions tx can start when there are no read/write transactions which:
        // - A read/write transaction tx can start when there are no transactions which:

        let blocking = MarkedVec::new(vm);
        let transaction_scope = transaction.scope(vm);
        for other in this.transactions(vm).values().iter() {
            let other = *other;

            // - Were created before tx; and
            if other == transaction {
                break;
            }

            // NB: According to the above conditions, we only block on transactions if one is read/write.
            if transaction.is_readonly() && other.is_readonly() {
                continue;
            }

            // - have overlapping scopes with tx; and
            let other_scope = other.scope(vm);
            let have_overlapping_scopes = transaction_scope
                .values()
                .iter()
                .any(|store| other_scope.values().contains(store));
            if !have_overlapping_scopes {
                continue;
            }

            // - are not finished.
            if other.is_finished() {
                continue;
            }

            blocking.push(other);
        }

        if blocking.is_empty() {
            if !transaction.is_readonly() {
                transaction.set_up_mutation_logs(vm);
            }
            return;
        }

        transaction.request_list().block_execution();
        this.wait_for_transactions_to_finish(
            vm,
            &blocking.values(),
            create_function(vm, transaction, |vm, &transaction, ()| {
                assert!(transaction.state() != TransactionState::Active);
                if transaction.request_list().is_empty() {
                    // https://w3c.github.io/IndexedDB/#transaction-commit
                    // The implementation must attempt to commit an inactive transaction when all requests placed
                    // against the transaction have completed and their returned results handled, no new requests have
                    // been placed against the transaction, and the transaction has not been aborted

                    // If we were blocked, that means that the JS task has had its chance to make requests. If the request
                    // list is empty, then the cleanup in the microtask checkpoint already ran, but skipped auto-committing.
                    // We have to do it here instead.

                    // FIXME: Update if this becomes explicit:
                    //        https://github.com/w3c/IndexedDB/issues/489#issuecomment-3994928473
                    commit_a_transaction(vm, transaction);
                    return;
                }
                if !transaction.is_readonly() {
                    transaction.set_up_mutation_logs(vm);
                }
                transaction.request_list().unblock_execution(vm);
            }),
        );
    }
}
