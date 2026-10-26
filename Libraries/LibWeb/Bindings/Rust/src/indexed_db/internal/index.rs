/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Indexes, https://w3c.github.io/IndexedDB/#index-construct

use core::cell::{Cell, Ref, RefCell};
use core::cmp::Ordering;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Vm};

use crate::indexed_db::idb_key_range::IDBKeyRange;
use crate::indexed_db::idb_record::{IndexRecord, StorageSerializationRecord};
use crate::indexed_db::internal::algorithms::KeyPath;
use crate::indexed_db::internal::key::Key;
use crate::indexed_db::internal::object_store::ObjectStoreRef;
use crate::indexed_db::internal::record_range::{KeyedRecord, record_range_for_key_range};

impl KeyedRecord for IndexRecord {
    fn key(&self) -> &Key {
        &self.key
    }
}

fn compare_index_records(a: &IndexRecord, b: &IndexRecord) -> Ordering {
    let key_comparison = Key::compare_two_keys(&a.key, &b.key);
    if key_comparison != 0 {
        return key_comparison.cmp(&0);
    }

    Key::compare_two_keys(&a.value, &b.value).cmp(&0)
}

// https://w3c.github.io/IndexedDB/#index-construct
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Index {
    // An index [...] has a referenced object store.
    object_store: ObjectStoreRef,

    // The index has a list of records which hold the data stored in the index.
    #[gc(untraced)]
    records: RefCell<Vec<IndexRecord>>,

    // An index has a name, which is a name. At any one time, the name is unique within index’s referenced object store.
    #[gc(untraced)]
    name: RefCell<Utf16String>,

    // An index has a unique flag. When true, the index enforces that no two records in the index has the same key.
    unique: bool,

    // An index has a multiEntry flag. This flag affects how the index behaves when the result of evaluating the index’s key path yields an array key.
    multi_entry: bool,

    // The keys are derived from the referenced object store’s values using a key path.
    #[gc(untraced)]
    key_path: KeyPath,

    deleted: Cell<bool>,

    // NB: Whether the request that creates the index in its upgrade transaction has yet to be processed. Until then,
    //     storing records into the object store leaves the index alone.
    pending_creation: Cell<bool>,
}

static INDEX_CLASS: EmbedderClass<Index> = EmbedderClass::new("IndexedDBIndex");

impl EmbedderValue for Index {
    const CLASS: &'static EmbedderClass<Self> = &INDEX_CLASS;
}

pub(crate) type IndexRef = Gc<EmbedderCell<Index>>;

impl Index {
    pub(crate) fn create(
        vm: &Vm,
        store: ObjectStoreRef,
        name: &Utf16String,
        key_path: &KeyPath,
        unique: bool,
        multi_entry: bool,
    ) -> IndexRef {
        let index = INDEX_CLASS.allocate(vm, |_| Index {
            object_store: store,
            records: RefCell::new(Vec::new()),
            name: RefCell::new(name.clone()),
            unique,
            multi_entry,
            key_path: key_path.clone(),
            deleted: Cell::new(false),
            pending_creation: Cell::new(false),
        });
        store.index_set_set(vm, name.clone(), index);
        index
    }

    pub(crate) fn set_name(this: IndexRef, vm: &Vm, name: Utf16String) {
        // NOTE: Update the key in the map so it still matches the name
        let old_value = this
            .object_store
            .index_set_take(vm, &this.name())
            .expect("the index is in its object store's index set");
        this.object_store.index_set_set(vm, name.clone(), old_value);

        *this.name.borrow_mut() = name;
    }

    pub(crate) fn name(&self) -> Utf16String {
        self.name.borrow().clone()
    }

    pub(crate) fn unique(&self) -> bool {
        self.unique
    }

    pub(crate) fn multi_entry(&self) -> bool {
        self.multi_entry
    }

    pub(crate) fn object_store(&self) -> ObjectStoreRef {
        self.object_store
    }

    pub(crate) fn records(&self) -> Ref<'_, Vec<IndexRecord>> {
        self.records.borrow()
    }

    pub(crate) fn key_path(&self) -> &KeyPath {
        &self.key_path
    }

    pub(crate) fn is_deleted(&self) -> bool {
        self.deleted.get()
    }

    pub(crate) fn set_deleted(&self, deleted: bool) {
        self.deleted.set(deleted);
    }

    pub(crate) fn is_pending_creation(&self) -> bool {
        self.pending_creation.get()
    }

    pub(crate) fn set_pending_creation(&self, pending_creation: bool) {
        self.pending_creation.set(pending_creation);
    }

    pub(crate) fn has_record_with_key(&self, key: &Key) -> bool {
        self.records
            .borrow()
            .binary_search_by(|record| Key::ordering(&record.key, key))
            .is_ok()
    }

    // https://w3c.github.io/IndexedDB/#index-referenced-value
    pub(crate) fn referenced_value(&self, index_record: &IndexRecord) -> StorageSerializationRecord {
        // Records in an index are said to have a referenced value.
        // This is the value of the record in the index’s referenced object store which has a key equal to the index’s record’s value.
        let store_records = self.object_store.records();
        let position = store_records
            .binary_search_by(|record| Key::ordering(&record.key, &index_record.value))
            .expect("the referenced object store has a record with the index record's value as its key");
        store_records[position].value.clone()
    }

    pub(crate) fn clear_records(this: IndexRef, vm: &Vm) {
        let deleted = core::mem::take(&mut *this.records.borrow_mut());
        if let Some(log) = this.object_store.mutation_log()
            && !deleted.is_empty()
        {
            log.note_index_records_deleted(vm, this, deleted);
        }
    }

    pub(crate) fn first_in_range(&self, range: &IDBKeyRange) -> Option<IndexRecord> {
        let records = self.records.borrow();
        let record_range = record_range_for_key_range(&records, range);
        if record_range.start == record_range.end {
            return None;
        }
        Some(records[record_range.start].clone())
    }

    pub(crate) fn first_n_in_range(&self, range: &IDBKeyRange, count: Option<u32>) -> Vec<IndexRecord> {
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

    pub(crate) fn last_n_in_range(&self, range: &IDBKeyRange, count: Option<u32>) -> Vec<IndexRecord> {
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

    pub(crate) fn count_records_in_range(&self, range: &IDBKeyRange) -> u64 {
        let records = self.records.borrow();
        let record_range = record_range_for_key_range(&records, range);
        (record_range.end - record_range.start) as u64
    }

    pub(crate) fn store_a_record(this: IndexRef, vm: &Vm, record: &IndexRecord) {
        if let Some(log) = this.object_store.mutation_log() {
            log.note_index_record_stored(vm, this, record.clone());
        }

        // NOTE: The record is stored in index’s list of records such that the list is sorted primarily on the records keys, and secondarily on the records values, in ascending order.
        let mut records = this.records.borrow_mut();
        if records
            .last()
            .is_none_or(|last| compare_index_records(last, record) != Ordering::Greater)
        {
            records.push(record.clone());
            return;
        }

        let position = records.partition_point(|existing| compare_index_records(existing, record) == Ordering::Less);
        records.insert(position, record.clone());
    }

    pub(crate) fn remove_record(&self, record: &IndexRecord) {
        let mut records = self.records.borrow_mut();
        let position = records.partition_point(|existing| compare_index_records(existing, record) == Ordering::Less);
        if position < records.len() && compare_index_records(&records[position], record) == Ordering::Equal {
            records.remove(position);
        }
    }

    pub(crate) fn remove_records_with_value_in_range(this: IndexRef, vm: &Vm, range: &IDBKeyRange) {
        let log = this.object_store.mutation_log();
        let mut removed_records = Vec::new();
        let mut records = this.records.borrow_mut();
        let mut i = 0;
        while i < records.len() {
            if range.is_in_range(&records[i].value) {
                let removed_record = records.remove(i);
                if log.is_some() {
                    removed_records.push(removed_record);
                }
                continue;
            }
            i += 1;
        }
        drop(records);
        if !removed_records.is_empty() {
            log.expect("records are only collected for a mutation log")
                .note_index_records_deleted(vm, this, removed_records);
        }
    }
}
