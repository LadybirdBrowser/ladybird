/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Object stores, https://w3c.github.io/IndexedDB/#object-store-construct

use core::cell::{Cell, Ref, RefCell, RefMut};

use ak::Utf16String;
use libjs_rust::api::{DeferGc, EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, Trace, Vm};

use crate::indexed_db::idb_key_range::IDBKeyRange;
use crate::indexed_db::idb_record::ObjectStoreRecord;
use crate::indexed_db::internal::algorithms::KeyPath;
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::index::IndexRef;
use crate::indexed_db::internal::key::{Key, KeyType};
use crate::indexed_db::internal::key_generator::{KeyGenerator, MAX_KEY_GENERATOR_VALUE};
use crate::indexed_db::internal::mutation_log::MutationLogRef;
use crate::indexed_db::internal::record_range::{KeyedRecord, record_range_for_key_range};

impl KeyedRecord for ObjectStoreRecord {
    fn key(&self) -> &Key {
        &self.key
    }
}

// https://w3c.github.io/IndexedDB/#object-store-construct
#[derive(Trace)]
#[gc(crate = libjs_rust)]
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
    #[gc(untraced)]
    key_generator: RefCell<Option<KeyGenerator>>,

    // An object store has a list of records
    #[gc(untraced)]
    records: RefCell<Vec<ObjectStoreRecord>>,

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
            key_generator: RefCell::new(auto_increment.then(KeyGenerator::new)),
            records: RefCell::new(Vec::new()),
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

    pub(crate) fn key_generator(&self) -> RefMut<'_, KeyGenerator> {
        RefMut::map(self.key_generator.borrow_mut(), |generator| {
            generator.as_mut().expect("the object store has a key generator")
        })
    }

    pub(crate) fn uses_a_key_generator(&self) -> bool {
        self.key_generator.borrow().is_some()
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

    pub(crate) fn records(&self) -> Ref<'_, Vec<ObjectStoreRecord>> {
        self.records.borrow()
    }

    // Mutation log access. The log is set externally by the readwrite transaction that owns it.
    pub(crate) fn mutation_log(&self) -> Option<MutationLogRef> {
        self.mutation_log.get()
    }

    pub(crate) fn set_mutation_log(&self, log: Option<MutationLogRef>) {
        self.mutation_log.set(log);
    }

    pub(crate) fn mutation_log_position(&self) -> usize {
        match self.mutation_log.get() {
            None => 0,
            Some(log) => log.position(),
        }
    }

    pub(crate) fn revert_mutations_from(this: ObjectStoreRef, vm: &Vm, position: usize) {
        if let Some(log) = this.mutation_log.get() {
            log.revert_from(vm, this, position);
        }
    }

    pub(crate) fn remove_records_in_range(&self, vm: &Vm, range: &IDBKeyRange) {
        let mut records = self.records.borrow_mut();
        if records.is_empty() {
            return;
        }

        // Since records are sorted by key, records in range form a contiguous block.
        let record_range = record_range_for_key_range(&records, range);

        if record_range.start < record_range.end {
            let deleted: Vec<ObjectStoreRecord> = records.drain(record_range.start..record_range.end).collect();
            drop(records);
            if let Some(log) = self.mutation_log.get() {
                log.note_records_deleted(vm, deleted);
            }
        }
    }

    pub(crate) fn remove_record_with_key(&self, key: &Key) {
        let mut records = self.records.borrow_mut();
        if let Ok(index) = records.binary_search_by(|record| Key::ordering(&record.key, key)) {
            records.remove(index);
        }
    }

    pub(crate) fn has_record_with_key(&self, key: &Key) -> bool {
        self.records
            .borrow()
            .binary_search_by(|record| Key::ordering(&record.key, key))
            .is_ok()
    }

    pub(crate) fn store_a_record(&self, vm: &Vm, record: ObjectStoreRecord) {
        if let Some(log) = self.mutation_log.get() {
            log.note_record_stored(vm, record.key.clone());
        }

        // NOTE: The record is stored in the object store’s list of records such that the list is sorted according to the key of the records in ascending order.
        let mut records = self.records.borrow_mut();
        if records
            .last()
            .is_none_or(|last| Key::compare_two_keys(&last.key, &record.key) <= 0)
        {
            records.push(record);
            return;
        }

        let position = records.partition_point(|existing| Key::compare_two_keys(&existing.key, &record.key) < 0);
        records.insert(position, record);
    }

    pub(crate) fn count_records_in_range(&self, range: &IDBKeyRange) -> u64 {
        let records = self.records.borrow();
        let record_range = record_range_for_key_range(&records, range);
        (record_range.end - record_range.start) as u64
    }

    pub(crate) fn first_in_range(&self, range: &IDBKeyRange) -> Option<ObjectStoreRecord> {
        let records = self.records.borrow();
        let record_range = record_range_for_key_range(&records, range);
        if record_range.start == record_range.end {
            return None;
        }
        Some(records[record_range.start].clone())
    }

    pub(crate) fn clear_records(&self, vm: &Vm) {
        let deleted_records = core::mem::take(&mut *self.records.borrow_mut());
        if let Some(log) = self.mutation_log.get()
            && !deleted_records.is_empty()
        {
            log.note_records_deleted(vm, deleted_records);
        }
    }

    // https://w3c.github.io/IndexedDB/#generate-a-key
    pub(crate) fn generate_a_key(&self, vm: &Vm) -> Result<u64, &'static str> {
        // 1. Let generator be store's key generator.
        let mut generator = self.key_generator();

        // 2. Let key be generator's current number.
        let key = generator.current_number();

        // 3. If key is greater than 2^53 (9007199254740992), then return failure.
        #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss, reason = "2^53 is a u64")]
        if key > MAX_KEY_GENERATOR_VALUE as u64 {
            return Err("Key is greater than 2^53 while trying to generate a key");
        }

        // 4. Increase generator's current number by 1.
        if let Some(log) = self.mutation_log.get() {
            log.note_key_generator_changed(vm, key);
        }
        generator.increment(1);

        // 5. Return key.
        Ok(key)
    }

    // https://w3c.github.io/IndexedDB/#possibly-update-the-key-generator
    pub(crate) fn possibly_update_the_key_generator(&self, vm: &Vm, key: &Key) {
        // 1. If the type of key is not number, abort these steps.
        if key.r#type() != KeyType::Number {
            return;
        }

        // 2. Let value be the value of key.
        let mut value = key.value_as_double();

        // 3. Set value to the minimum of value and 2^53 (9007199254740992).
        value = value.min(MAX_KEY_GENERATOR_VALUE);

        // 4. Set value to the largest integer not greater than value.
        value = value.floor();

        // 5. Let generator be store's key generator.
        let mut generator = self.key_generator();

        // 6. If value is greater than or equal to generator's current number, then set generator's current number to value + 1.
        #[allow(clippy::cast_precision_loss, reason = "the current number is at most 2^53 + 1")]
        if value >= generator.current_number() as f64 {
            if let Some(log) = self.mutation_log.get() {
                log.note_key_generator_changed(vm, generator.current_number());
            }
            // NB: value + 1 is computed as an integer, as 2^53 + 1 does not fit in a double.
            #[allow(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                reason = "value is an integer in the range of the current number"
            )]
            generator.set(value as u64 + 1);
        }
    }

    pub(crate) fn first_n_in_range(&self, range: &IDBKeyRange, count: Option<u32>) -> Vec<ObjectStoreRecord> {
        let mut result = Vec::new();
        let records = self.records.borrow();
        let record_range = record_range_for_key_range(&records, range);
        for record in &records[record_range.start..record_range.end] {
            result.push(record.clone());

            if count.is_some_and(|count| result.len() >= count as usize) {
                break;
            }
        }

        result
    }

    pub(crate) fn last_n_in_range(&self, range: &IDBKeyRange, count: Option<u32>) -> Vec<ObjectStoreRecord> {
        let mut result = Vec::new();
        let records = self.records.borrow();
        let record_range = record_range_for_key_range(&records, range);
        for record in records[record_range.start..record_range.end].iter().rev() {
            result.push(record.clone());

            if count.is_some_and(|count| result.len() >= count as usize) {
                break;
            }
        }

        result
    }
}
