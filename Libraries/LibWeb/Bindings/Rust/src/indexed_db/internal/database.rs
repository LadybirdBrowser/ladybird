/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Databases, https://w3c.github.io/IndexedDB/#database-construct, and the connections associated with them

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, GcWeak, MarkedVec, Trace, Vm};

use crate::bindings::interface::Ref;
use crate::indexed_db::idb_database::{ConnectionState, IDBDatabase};
use crate::indexed_db::idb_transaction::IDBTransaction;
use crate::indexed_db::internal::algorithms::queue_a_database_task;
use crate::indexed_db::internal::function::Function;
use crate::indexed_db::internal::object_store::ObjectStoreRef;
use crate::indexed_db::internal::storage::{StorageKey, storage};

type WeakConnection = GcWeak<EmbedderCell<crate::bindings::interface::Wrapped<IDBDatabase>>>;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct PendingConnectionWait {
    #[gc(untraced = "the connections are weak")]
    connections: Vec<WeakConnection>,
    callback: Function,
}

// https://w3c.github.io/IndexedDB/#database-construct
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Database {
    #[gc(untraced = "the connections are weak")]
    associated_connections: RefCell<Vec<WeakConnection>>,
    pending_connection_wait: GcRefCell<Option<PendingConnectionWait>>,

    // A database has a name which identifies it within a specific storage key.
    name: Utf16String,

    // A database has a version. When a database is first created, its version is 0 (zero).
    version: Cell<u64>,

    // A database has at most one associated upgrade transaction, which is either null or an upgrade transaction, and is initially null.
    upgrade_transaction: Cell<Option<Ref<IDBTransaction>>>,

    // A database has zero or more object stores which hold the data stored in the database.
    object_stores: GcRefCell<Vec<ObjectStoreRef>>,
}

static DATABASE_CLASS: EmbedderClass<Database> = EmbedderClass::new("IndexedDBDatabase");

impl EmbedderValue for Database {
    const CLASS: &'static EmbedderClass<Self> = &DATABASE_CLASS;
}

pub(crate) type DatabaseRef = Gc<EmbedderCell<Database>>;

impl Database {
    pub(crate) fn create(vm: &Vm, name: &Utf16String) -> DatabaseRef {
        DATABASE_CLASS.allocate(vm, |_| Database {
            associated_connections: RefCell::new(Vec::new()),
            pending_connection_wait: GcRefCell::new(None),
            name: name.clone(),
            version: Cell::new(0),
            upgrade_transaction: Cell::new(None),
            object_stores: GcRefCell::new(Vec::new()),
        })
    }

    pub(crate) fn set_version(&self, version: u64) {
        self.version.set(version);
    }

    pub(crate) fn version(&self) -> u64 {
        self.version.get()
    }

    pub(crate) fn name(&self) -> Utf16String {
        self.name.clone()
    }

    pub(crate) fn set_upgrade_transaction(&self, transaction: Option<Ref<IDBTransaction>>) {
        self.upgrade_transaction.set(transaction);
    }

    pub(crate) fn upgrade_transaction(&self) -> Option<Ref<IDBTransaction>> {
        self.upgrade_transaction.get()
    }

    pub(crate) fn object_stores<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, ObjectStoreRef> {
        let object_stores = MarkedVec::new(vm);
        object_stores.extend_from_slice(&self.object_stores.borrow());
        object_stores
    }

    pub(crate) fn object_store_with_name(&self, name: &Utf16String) -> Option<ObjectStoreRef> {
        self.object_stores
            .borrow()
            .iter()
            .find(|object_store| object_store.name() == *name)
            .copied()
    }

    pub(crate) fn add_object_store(this: DatabaseRef, vm: &Vm, object_store: ObjectStoreRef) {
        this.object_stores.borrow_mut(vm).push(object_store);
    }

    pub(crate) fn remove_object_store(this: DatabaseRef, vm: &Vm, object_store: ObjectStoreRef) {
        let mut object_stores = this.object_stores.borrow_mut(vm);
        if let Some(position) = object_stores.iter().position(|entry| *entry == object_store) {
            object_stores.remove(position);
        }
    }

    pub(crate) fn associate(&self, vm: &Vm, connection: Ref<IDBDatabase>) {
        self.associated_connections
            .borrow_mut()
            .push(GcWeak::new(vm.heap(), connection));
    }

    pub(crate) fn dissociate(&self, connection: Ref<IDBDatabase>) {
        self.associated_connections
            .borrow_mut()
            .retain(|entry| entry.get().is_some_and(|entry| entry != connection));
    }

    pub(crate) fn associated_connections<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, Ref<IDBDatabase>> {
        let connections = MarkedVec::new(vm);
        for connection in self.associated_connections.borrow().iter() {
            if let Some(connection) = connection.get() {
                connections.push(connection);
            }
        }
        connections
    }

    pub(crate) fn associated_connections_except<'vm>(
        &self,
        vm: &'vm Vm,
        connection: Ref<IDBDatabase>,
    ) -> MarkedVec<'vm, Ref<IDBDatabase>> {
        let connections = MarkedVec::new(vm);
        for associated_connection in self.associated_connections.borrow().iter() {
            if let Some(associated_connection) = associated_connection.get()
                && associated_connection != connection
            {
                connections.push(associated_connection);
            }
        }
        connections
    }

    pub(crate) fn for_key<'vm>(vm: &'vm Vm, key: &StorageKey) -> MarkedVec<'vm, DatabaseRef> {
        let databases = MarkedVec::new(vm);
        storage(vm).for_each_database_of_key(key, |database| databases.push(database));
        databases
    }

    pub(crate) fn for_key_and_name(vm: &Vm, key: &StorageKey, name: &Utf16String) -> Option<DatabaseRef> {
        storage(vm).database(key, name)
    }

    pub(crate) fn create_for_key_and_name(vm: &Vm, key: &StorageKey, name: &Utf16String) -> DatabaseRef {
        let value = Database::create(vm, name);
        storage(vm).set_database(vm, key, name, value);
        value
    }

    pub(crate) fn delete_for_key_and_name(vm: &Vm, key: &StorageKey, name: &Utf16String) {
        storage(vm).remove_database(vm, key, name);
    }

    pub(crate) fn for_each_database(vm: &Vm, mut visitor: impl FnMut(DatabaseRef)) {
        let databases = MarkedVec::new(vm);
        storage(vm).for_each_database(|database| databases.push(database));
        for database in databases.values().iter() {
            visitor(*database);
        }
    }

    pub(crate) fn wait_for_connections_to_close(&self, vm: &Vm, connections: &[Ref<IDBDatabase>], after_all: Function) {
        let all_closed = connections.iter().all(|entry| entry.state() == ConnectionState::Closed);

        if all_closed {
            queue_a_database_task(vm, after_all);
            return;
        }

        assert!(self.pending_connection_wait.borrow().is_none());
        let weak_connections = connections
            .iter()
            .map(|connection| GcWeak::new(vm.heap(), *connection))
            .collect();
        *self.pending_connection_wait.borrow_mut(vm) = Some(PendingConnectionWait {
            connections: weak_connections,
            callback: after_all,
        });
    }

    pub(crate) fn check_pending_connection_wait(&self, vm: &Vm) {
        let callback = {
            let wait = self.pending_connection_wait.borrow();
            let Some(wait) = &*wait else {
                return;
            };
            for connection in &wait.connections {
                if let Some(connection) = connection.get()
                    && connection.state() != ConnectionState::Closed
                {
                    return;
                }
            }
            wait.callback
        };

        // All connections are closed - invoke the callback and clear the wait
        *self.pending_connection_wait.borrow_mut(vm) = None;
        queue_a_database_task(vm, callback);
    }
}
