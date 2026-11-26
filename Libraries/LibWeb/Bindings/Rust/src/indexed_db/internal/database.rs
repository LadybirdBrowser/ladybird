/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Databases, https://w3c.github.io/IndexedDB/#database-construct, and the connections associated with them

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Trace, Vm};

use crate::bindings::interface::Ref;
use crate::indexed_db::idb_transaction::IDBTransaction;
use crate::indexed_db::internal::object_store::ObjectStoreRef;

// https://w3c.github.io/IndexedDB/#database-construct
#[derive(Trace)]
pub(crate) struct Database {
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
}
