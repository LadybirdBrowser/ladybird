/*
 * Copyright (c) 2025, zaggy1024
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The log of what a transaction changed in an object store, which an abort reverts

use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Trace, Vm};

use crate::bindings::interface::Ref;
use crate::indexed_db::idb_database::IDBDatabase;
use crate::indexed_db::idb_record::{IndexRecord, ObjectStoreRecord};
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
#[gc(crate = libjs_rust)]
enum Entry {
    ObjectStoreCreated,
    ObjectStoreDeleted,
    ObjectStoreRenamed {
        old_name: Utf16String,
    },
    IndexCreated {
        index: IndexRef,
    },
    IndexDeleted {
        index: IndexRef,
    },
    IndexRenamed {
        index: IndexRef,
        old_name: Utf16String,
    },
    KeyGeneratorChanged {
        old_value: u64,
    },
    RecordsDeleted {
        #[gc(untraced)]
        records: Vec<ObjectStoreRecord>,
    },
    RecordStored {
        #[gc(untraced)]
        key: Rc<Key>,
    },
    IndexRecordsDeleted {
        index: IndexRef,
        #[gc(untraced)]
        records: Vec<IndexRecord>,
    },
    IndexRecordStored {
        index: IndexRef,
        #[gc(untraced)]
        record: IndexRecord,
    },
}

// Tracks mutations to an object store and its indexes so they can be reverted on request failure
// (asynchronously execute a request step 5.4) or transaction abort (abort a transaction step 2).
//
// For upgrade transactions, this also tracks schema-level changes (store/index creation and deletion).
// Schema entries are always at the boundaries: ObjectStoreCreated is always first (if present),
// and ObjectStoreDeleted is always last (if present). Index schema entries appear in order between
// data mutations.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
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

    // Record that the key generator value was changed, saving the old value for revert.
    pub(crate) fn note_key_generator_changed(&self, vm: &Vm, old_value: u64) {
        self.append(vm, Entry::KeyGeneratorChanged { old_value });
    }

    // Record that records were deleted from the object store, saving them for re-insertion on revert.
    pub(crate) fn note_records_deleted(&self, vm: &Vm, records: Vec<ObjectStoreRecord>) {
        self.append(vm, Entry::RecordsDeleted { records });
    }

    // Record that a new record was stored in the object store, saving the key for deletion on revert.
    pub(crate) fn note_record_stored(&self, vm: &Vm, key: Rc<Key>) {
        self.append(vm, Entry::RecordStored { key });
    }

    // Record that records were deleted from an index, saving them for re-insertion on revert.
    pub(crate) fn note_index_records_deleted(&self, vm: &Vm, index: IndexRef, records: Vec<IndexRecord>) {
        self.append(vm, Entry::IndexRecordsDeleted { index, records });
    }

    // Record that a new record was stored in an index, saving it for deletion on revert.
    pub(crate) fn note_index_record_stored(&self, vm: &Vm, index: IndexRef, record: IndexRecord) {
        self.append(vm, Entry::IndexRecordStored { index, record });
    }

    pub(crate) fn position(&self) -> usize {
        self.entries.borrow().len()
    }

    pub(crate) fn append_changes(
        &self,
        database_name: &Utf16String,
        object_store_name: &Utf16String,
        changes: &mut TransactionChanges,
    ) {
        let mut stored_keys: Vec<Rc<Key>> = Vec::new();
        let mut changed_keys: Vec<Rc<Key>> = Vec::new();
        let mut deleted_keys: Vec<Rc<Key>> = Vec::new();

        fn remove_key(keys: &mut Vec<Rc<Key>>, key: &Key) -> bool {
            match keys.iter().position(|existing| Key::equals(existing, key)) {
                Some(position) => {
                    keys.remove(position);
                    true
                }
                None => false,
            }
        }

        let change = |key: Option<Rc<Key>>| TransactionChange {
            database_name: database_name.clone(),
            object_store_name: object_store_name.clone(),
            key,
        };

        for entry in self.entries.borrow().iter() {
            match entry {
                Entry::ObjectStoreCreated => {
                    changes.added.push(change(None));
                }
                Entry::ObjectStoreDeleted => {
                    changes.deleted.push(change(None));
                }
                Entry::ObjectStoreRenamed { old_name } => {
                    changes.deleted.push(TransactionChange {
                        database_name: database_name.clone(),
                        object_store_name: old_name.clone(),
                        key: None,
                    });
                    changes.added.push(change(None));
                }
                Entry::IndexCreated { .. } | Entry::IndexDeleted { .. } | Entry::IndexRenamed { .. } => {}
                Entry::KeyGeneratorChanged { .. } => {}
                Entry::RecordsDeleted { records } => {
                    for record in records {
                        if remove_key(&mut stored_keys, &record.key) {
                            continue;
                        }
                        remove_key(&mut changed_keys, &record.key);
                        deleted_keys.push(record.key.clone());
                    }
                }
                Entry::RecordStored { key } => {
                    if remove_key(&mut deleted_keys, key) {
                        changed_keys.push(key.clone());
                        continue;
                    }
                    stored_keys.push(key.clone());
                }
                Entry::IndexRecordsDeleted { .. } | Entry::IndexRecordStored { .. } => {}
            }
        }

        for key in stored_keys {
            changes.added.push(change(Some(key)));
        }

        for key in changed_keys {
            changes.changed.push(change(Some(key)));
        }

        for key in deleted_keys {
            changes.deleted.push(change(Some(key)));
        }
    }

    // Undo all logged mutations in reverse order. Database and connection are required for schema entries.
    pub(crate) fn revert(&self, vm: &Vm, store: ObjectStoreRef, database: DatabaseRef, connection: Ref<IDBDatabase>) {
        self.revert_entries(vm, store, 0, Some((database, connection)));
    }

    // Undo logged mutations from the given position forward (used for per-request revert on failure).
    // Asserts that no schema entries are encountered.
    pub(crate) fn revert_from(&self, vm: &Vm, store: ObjectStoreRef, position: usize) {
        self.revert_entries(vm, store, position, None);
    }

    fn revert_entries(
        &self,
        vm: &Vm,
        store: ObjectStoreRef,
        from_position: usize,
        schema: Option<(DatabaseRef, Ref<IDBDatabase>)>,
    ) {
        // Temporarily clear the store's mutation log pointer so that the operations below
        // don't re-log into this log while we're reverting it.
        let saved_log = store.mutation_log();
        store.set_mutation_log(None);

        loop {
            let entry = {
                let mut entries = self.entries.borrow_mut(vm);
                if entries.len() <= from_position {
                    break;
                }
                entries.pop().expect("the log has an entry past the position")
            };
            match entry {
                Entry::ObjectStoreCreated => {
                    let (database, connection) = schema.expect("schema entries are reverted with the database");
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
                    let (database, connection) = schema.expect("schema entries are reverted with the database");
                    store.set_deleted(false);
                    for index in store.indexes(vm).values().iter() {
                        index.set_deleted(false);
                    }
                    Database::add_object_store(database, vm, store);
                    connection.add_to_object_store_set(vm, store);
                }
                Entry::IndexCreated { index } => {
                    assert!(schema.is_some(), "schema entries are reverted with the database");
                    index.object_store().index_set_remove(vm, &index.name());

                    // https://w3c.github.io/IndexedDB/#abort-a-transaction
                    // Any object stores and indexes which were created during the transaction are now considered deleted
                    // for the purposes of other algorithms.
                    index.set_deleted(true);
                }
                Entry::IndexDeleted { index } => {
                    assert!(schema.is_some(), "schema entries are reverted with the database");
                    index.set_deleted(false);
                    index.object_store().index_set_set(vm, index.name(), index);
                }
                Entry::ObjectStoreRenamed { old_name } => {
                    store.set_name(old_name);
                }
                Entry::IndexRenamed { index, old_name } => {
                    Index::set_name(index, vm, old_name);
                }
                Entry::KeyGeneratorChanged { old_value } => {
                    store.key_generator().set(old_value);
                }
                Entry::RecordsDeleted { records } => {
                    for record in records {
                        store.store_a_record(vm, record);
                    }
                }
                Entry::RecordStored { key } => {
                    store.remove_record_with_key(&key);
                }
                Entry::IndexRecordsDeleted { index, records } => {
                    for record in &records {
                        Index::store_a_record(index, vm, record);
                    }
                }
                Entry::IndexRecordStored { index, record } => {
                    index.remove_record(&record);
                }
            }
        }

        store.set_mutation_log(saved_log);
    }
}
