/*
 * Copyright (c) 2025, zaggy1024
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The log of what an upgrade transaction changed in the shape of an object store, which an abort reverts

use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Trace, Vm};

use crate::bindings::interface::Ref;
use crate::indexed_db::idb_database::IDBDatabase;
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::index::{Index, IndexRef};
use crate::indexed_db::internal::key::Key;
use crate::indexed_db::internal::object_store::ObjectStoreRef;

// https://w3c.github.io/IndexedDB/ has no notion of these changes: they are what DevTools reports of a transaction
// that commits, as Web::IndexedDB::TransactionChanges of LibWebCommon.
pub(crate) struct TransactionChange {
    pub(crate) database_name: Utf16String,
    pub(crate) object_store_name: Utf16String,
    pub(crate) key: Option<Rc<Key>>,
}

#[derive(Default)]
pub(crate) struct TransactionChanges {
    pub(crate) added: Vec<TransactionChange>,
    pub(crate) changed: Vec<TransactionChange>,
    pub(crate) deleted: Vec<TransactionChange>,
}

impl TransactionChanges {
    pub(crate) fn is_empty(&self) -> bool {
        self.added.is_empty() && self.changed.is_empty() && self.deleted.is_empty()
    }
}

#[derive(Trace)]
enum Entry {
    ObjectStoreCreated,
    ObjectStoreDeleted,
    ObjectStoreRenamed { old_name: Utf16String },
    IndexCreated { index: IndexRef },
    IndexDeleted { index: IndexRef },
    IndexRenamed { index: IndexRef, old_name: Utf16String },
}

// Tracks the changes an upgrade transaction makes to an object store and its indexes (creation, deletion, renaming), so
// that an abort can revert the connection's copy of them (abort a transaction step 2). The backend reverts its own.
// ObjectStoreCreated is always first (if present), and ObjectStoreDeleted is always last (if present).
#[derive(Trace)]
pub(crate) struct MutationLog {
    entries: GcRefCell<Vec<Entry>>,
}

static MUTATION_LOG_CLASS: EmbedderClass<MutationLog> = EmbedderClass::new("IndexedDBMutationLog");

impl EmbedderValue for MutationLog {
    const CLASS: &'static EmbedderClass<Self> = &MUTATION_LOG_CLASS;
}

pub(crate) type MutationLogRef = Gc<EmbedderCell<MutationLog>>;

impl MutationLog {
    pub(crate) fn create(vm: &Vm) -> MutationLogRef {
        MUTATION_LOG_CLASS.allocate(vm, |_| MutationLog {
            entries: GcRefCell::new(Vec::new()),
        })
    }

    fn append(&self, vm: &Vm, entry: Entry) {
        self.entries.borrow_mut(vm).push(entry);
    }

    // Schema-level entries (upgrade transactions only).
    pub(crate) fn note_object_store_created(&self, vm: &Vm) {
        self.append(vm, Entry::ObjectStoreCreated);
    }

    pub(crate) fn note_object_store_deleted(&self, vm: &Vm) {
        self.append(vm, Entry::ObjectStoreDeleted);
    }

    pub(crate) fn note_object_store_renamed(&self, vm: &Vm, old_name: Utf16String) {
        self.append(vm, Entry::ObjectStoreRenamed { old_name });
    }

    pub(crate) fn note_index_created(&self, vm: &Vm, index: IndexRef) {
        self.append(vm, Entry::IndexCreated { index });
    }

    pub(crate) fn note_index_deleted(&self, vm: &Vm, index: IndexRef) {
        self.append(vm, Entry::IndexDeleted { index });
    }

    pub(crate) fn note_index_renamed(&self, vm: &Vm, index: IndexRef, old_name: Utf16String) {
        self.append(vm, Entry::IndexRenamed { index, old_name });
    }

    pub(crate) fn append_changes(
        &self,
        database_name: &Utf16String,
        object_store_name: &Utf16String,
        changes: &mut TransactionChanges,
    ) {
        let change = |object_store_name: &Utf16String| TransactionChange {
            database_name: database_name.clone(),
            object_store_name: object_store_name.clone(),
            key: None,
        };

        for entry in self.entries.borrow().iter() {
            match entry {
                Entry::ObjectStoreCreated => changes.added.push(change(object_store_name)),
                Entry::ObjectStoreDeleted => changes.deleted.push(change(object_store_name)),
                Entry::ObjectStoreRenamed { old_name } => {
                    changes.deleted.push(change(old_name));
                    changes.added.push(change(object_store_name));
                }
                Entry::IndexCreated { .. } | Entry::IndexDeleted { .. } | Entry::IndexRenamed { .. } => {}
            }
        }
    }

    // Undo all logged changes in reverse order.
    pub(crate) fn revert(&self, vm: &Vm, store: ObjectStoreRef, database: DatabaseRef, connection: Ref<IDBDatabase>) {
        // Temporarily clear the store's mutation log pointer so that the operations below
        // don't re-log into this log while we're reverting it.
        let saved_log = store.mutation_log();
        store.set_mutation_log(None);

        loop {
            let Some(entry) = self.entries.borrow_mut(vm).pop() else {
                break;
            };
            match entry {
                Entry::ObjectStoreCreated => {
                    Database::remove_object_store(database, vm, store);
                    connection.remove_from_object_store_set(vm, store);

                    // https://w3c.github.io/IndexedDB/#abort-a-transaction
                    // Any object stores and indexes which were created during the transaction are now considered deleted
                    // for the purposes of other algorithms.
                    store.set_deleted(true);
                    for index in store.indexes(vm).values().iter() {
                        index.set_deleted(true);
                    }
                }
                Entry::ObjectStoreDeleted => {
                    store.set_deleted(false);
                    for index in store.indexes(vm).values().iter() {
                        index.set_deleted(false);
                    }
                    Database::add_object_store(database, vm, store);
                    connection.add_to_object_store_set(vm, store);
                }
                Entry::IndexCreated { index } => {
                    index.object_store().index_set_remove(vm, &index.name());

                    // https://w3c.github.io/IndexedDB/#abort-a-transaction
                    // Any object stores and indexes which were created during the transaction are now considered deleted
                    // for the purposes of other algorithms.
                    index.set_deleted(true);
                }
                Entry::IndexDeleted { index } => {
                    index.set_deleted(false);
                    index.object_store().index_set_set(vm, index.name(), index);
                }
                Entry::ObjectStoreRenamed { old_name } => {
                    store.set_name(old_name);
                }
                Entry::IndexRenamed { index, old_name } => {
                    Index::set_name(index, vm, old_name);
                }
            }
        }

        store.set_mutation_log(saved_log);
    }
}
