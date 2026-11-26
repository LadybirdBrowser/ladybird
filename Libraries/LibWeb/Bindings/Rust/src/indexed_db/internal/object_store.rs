/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Object stores, https://w3c.github.io/IndexedDB/#object-store-construct, as a connection knows them: The backend
//! holds their records.

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::{DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Trace, Vm};

use crate::indexed_db::internal::algorithms::KeyPath;
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::index::IndexRef;
use crate::indexed_db::internal::mutation_log::MutationLogRef;

// https://w3c.github.io/IndexedDB/#object-store-construct
#[derive(Trace)]
pub(crate) struct ObjectStore {
    // AD-HOC: An ObjectStore needs to know what Database it belongs to...
    database: DatabaseRef,

    // AD-HOC: An Index has referenced ObjectStores, we also need the reverse mapping
    indexes: GcRefCell<Vec<(Utf16String, IndexRef)>>,

    // An object store has a name, which is a name. At any one time, the name is unique within the database to which it belongs.
    #[gc(untraced)]
    name: RefCell<Utf16String>,

    // An object store optionally has a key path. If the object store has a key path it is said to use in-line keys. Otherwise it is said to use out-of-line keys.
    #[gc(untraced)]
    key_path: Option<KeyPath>,

    // An object store optionally has a key generator.
    // NB: The backend holds the key generator, and the records; a connection only knows if there is one.
    auto_increment: bool,

    deleted: Cell<bool>,

    // AD-HOC: Tracks mutations for revert on request failure or transaction abort.
    mutation_log: Cell<Option<MutationLogRef>>,
}

static OBJECT_STORE_CLASS: EmbedderClass<ObjectStore> = EmbedderClass::new("IndexedDBObjectStore");

impl EmbedderValue for ObjectStore {
    const CLASS: &'static EmbedderClass<Self> = &OBJECT_STORE_CLASS;
}

pub(crate) type ObjectStoreRef = Gc<EmbedderCell<ObjectStore>>;

impl ObjectStore {
    pub(crate) fn create(
        vm: &Vm,
        database: DatabaseRef,
        name: Utf16String,
        auto_increment: bool,
        key_path: Option<KeyPath>,
    ) -> ObjectStoreRef {
        let object_store = OBJECT_STORE_CLASS.allocate(vm, |_| ObjectStore {
            database,
            indexes: GcRefCell::new(Vec::new()),
            name: RefCell::new(name),
            key_path,
            auto_increment,
            deleted: Cell::new(false),
            mutation_log: Cell::new(None),
        });
        Database::add_object_store(database, vm, object_store);
        object_store
    }

    pub(crate) fn name(&self) -> Utf16String {
        self.name.borrow().clone()
    }

    pub(crate) fn set_name(&self, name: Utf16String) {
        *self.name.borrow_mut() = name;
    }

    pub(crate) fn key_path(&self) -> Option<&KeyPath> {
        self.key_path.as_ref()
    }

    pub(crate) fn uses_inline_keys(&self) -> bool {
        self.key_path.is_some()
    }

    pub(crate) fn uses_out_of_line_keys(&self) -> bool {
        self.key_path.is_none()
    }

    pub(crate) fn uses_a_key_generator(&self) -> bool {
        self.auto_increment
    }

    /// The index set, as a list of the indexes with their names, for the storage of a cell being built.
    pub(crate) fn index_set(&self, _: &DeferGc<'_>) -> Vec<(Utf16String, IndexRef)> {
        self.indexes.borrow().clone()
    }

    pub(crate) fn copy_index_set_into(&self, indexes: &mut Vec<(Utf16String, IndexRef)>) {
        indexes.clone_from(&self.indexes.borrow());
    }

    pub(crate) fn indexes<'vm>(&self, vm: &'vm Vm) -> MarkedVec<'vm, IndexRef> {
        let indexes = MarkedVec::new(vm);
        for (_, index) in self.indexes.borrow().iter() {
            indexes.push(*index);
        }
        indexes
    }

    pub(crate) fn index_set_get(&self, name: &Utf16String) -> Option<IndexRef> {
        self.indexes
            .borrow()
            .iter()
            .find(|(index_name, _)| index_name == name)
            .map(|(_, index)| *index)
    }

    pub(crate) fn index_set_set(&self, vm: &Vm, name: Utf16String, index: IndexRef) {
        let mut indexes = self.indexes.borrow_mut(vm);
        if let Some(entry) = indexes.iter_mut().find(|(index_name, _)| *index_name == name) {
            entry.1 = index;
            return;
        }
        indexes.push((name, index));
    }

    pub(crate) fn index_set_take(&self, vm: &Vm, name: &Utf16String) -> Option<IndexRef> {
        let mut indexes = self.indexes.borrow_mut(vm);
        let position = indexes.iter().position(|(index_name, _)| index_name == name)?;
        Some(indexes.remove(position).1)
    }

    pub(crate) fn index_set_remove(&self, vm: &Vm, name: &Utf16String) {
        self.index_set_take(vm, name);
    }

    pub(crate) fn is_deleted(&self) -> bool {
        self.deleted.get()
    }

    pub(crate) fn set_deleted(&self, deleted: bool) {
        self.deleted.set(deleted);
    }

    pub(crate) fn database(&self) -> DatabaseRef {
        self.database
    }

    // Mutation log access. The log is set externally by the readwrite transaction that owns it.
    pub(crate) fn mutation_log(&self) -> Option<MutationLogRef> {
        self.mutation_log.get()
    }

    pub(crate) fn set_mutation_log(&self, log: Option<MutationLogRef>) {
        self.mutation_log.set(log);
    }
}
