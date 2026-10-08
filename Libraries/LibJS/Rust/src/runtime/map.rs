/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use hashbrown::HashMap;
use libjs_runtime_macros::Trace;

use crate::gc::class::{ExternalMemorySize, Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;
use crate::runtime::value_traits::ValueTraitsKey;

#[derive(Trace)]
struct StoredEntry {
    key: Value,
    value: Value,
    insertion_id: u64,
}

impl StoredEntry {
    fn is_removed(&self) -> bool {
        self.key.is_empty()
    }
}

#[derive(Default)]
struct MapStorage {
    /// All entries in insertion order. Removed entries stay in place (as holes) until the next compaction, so the
    /// positions held by iterators stay valid. Compacting or clearing moves entries, which bumps generation.
    entries: Vec<StoredEntry>,
    indices: HashMap<ValueTraitsKey, usize, foldhash::fast::RandomState>,
    removed_entry_count: usize,
    next_insertion_id: u64,
    generation: u64,
    external_memory_size: usize,
}

// SAFETY: Visits the key and value of every entry. The keys of the indices are the keys of the live entries.
unsafe impl Trace for MapStorage {
    fn trace(&self, visitor: &mut Visitor) {
        self.entries.trace(visitor);
    }
}

impl MapStorage {
    fn update_external_memory_size(&mut self) -> (usize, usize) {
        let old_size = self.external_memory_size;
        self.external_memory_size =
            (self.entries.capacity() * size_of::<StoredEntry>()).saturating_add(self.indices.allocation_size());
        (old_size, self.external_memory_size)
    }

    fn index_of_first_entry_not_inserted_before(&self, insertion_id: u64) -> usize {
        // Entries are stored in insertion order, so their insertion IDs are increasing.
        self.entries.partition_point(|entry| entry.insertion_id < insertion_id)
    }

    fn compact_entries(&mut self) {
        let mut new_indices = vec![0usize; self.entries.len()];

        let mut live_entry_count = 0;
        for (i, new_index) in new_indices.iter_mut().enumerate() {
            if self.entries[i].is_removed() {
                continue;
            }
            *new_index = live_entry_count;
            self.entries.swap(live_entry_count, i);
            live_entry_count += 1;
        }
        self.entries.truncate(live_entry_count);
        if self.entries.capacity() > live_entry_count * 2 {
            self.entries.shrink_to_fit();
        }

        for index in self.indices.values_mut() {
            *index = new_indices[*index];
        }

        self.removed_entry_count = 0;
        self.generation += 1;
    }
}

/// The key and value of a Map entry.
#[derive(Clone, Copy)]
pub struct Entry {
    pub key: Value,
    pub value: Value,
}

#[repr(C)]
#[derive(Trace)]
pub struct Map {
    base: Object,
    storage: GcRefCell<MapStorage>,
}

define_cell!(
    Map,
    Object,
    extends: [Object],
    finalize: finalize,
    external_memory_size: external_memory_size
);

impl ExternalMemorySize for Map {
    fn external_memory_size(&self) -> usize {
        self.base
            .external_memory_size()
            .saturating_add(self.storage.borrow().external_memory_size)
    }
}

impl Deref for Map {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for Map {
    fn finalize(&self) {
        drop(self.storage.replace(MapStorage::default()));
    }
}

impl Map {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> Map {
        Map {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            storage: GcRefCell::new(MapStorage::default()),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Map> {
        realm.create_object(vm, Map::new(vm, realm.intrinsics().map_prototype(vm)))
    }

    // 24.1.3.1 Map.prototype.clear ( ), https://tc39.es/ecma262/#sec-map.prototype.clear
    pub fn map_clear(&self, vm: &Vm) {
        let mut storage = self.storage.borrow_mut();
        storage.entries = Vec::new();
        storage.indices = HashMap::default();
        storage.removed_entry_count = 0;
        storage.generation += 1;
        let (old_size, new_size) = storage.update_external_memory_size();
        drop(storage);
        vm.heap().account_external_memory_change(old_size, new_size);
    }

    // 24.1.3.3 Map.prototype.delete ( key ), https://tc39.es/ecma262/#sec-map.prototype.delete
    pub fn map_remove(&self, vm: &Vm, key: Value) -> bool {
        let mut storage = self.storage.borrow_mut();
        let Some(index) = storage.indices.remove(&ValueTraitsKey(key)) else {
            return false;
        };

        let entry = &mut storage.entries[index];
        entry.key = Value::EMPTY;
        entry.value = Value::UNDEFINED;
        storage.removed_entry_count += 1;

        // Compact once removed entries outnumber the live ones, so removal stays amortized O(1).
        const MINIMUM_REMOVED_ENTRY_COUNT_FOR_COMPACTION: usize = 8;
        if storage.removed_entry_count >= MINIMUM_REMOVED_ENTRY_COUNT_FOR_COMPACTION
            && storage.removed_entry_count > storage.indices.len()
        {
            storage.compact_entries();
        }
        let (old_size, new_size) = storage.update_external_memory_size();
        drop(storage);
        vm.heap().account_external_memory_change(old_size, new_size);

        true
    }

    // 24.1.3.6 Map.prototype.get ( key ), https://tc39.es/ecma262/#sec-map.prototype.get
    pub fn map_get(&self, key: Value) -> Option<Value> {
        let storage = self.storage.borrow();
        let index = *storage.indices.get(&ValueTraitsKey(key))?;
        Some(storage.entries[index].value)
    }

    // 24.1.3.7 Map.prototype.has ( key ), https://tc39.es/ecma262/#sec-map.prototype.has
    pub fn map_has(&self, key: Value) -> bool {
        self.storage.borrow().indices.contains_key(&ValueTraitsKey(key))
    }

    // 24.1.3.9 Map.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-map.prototype.set
    pub fn map_set(&self, vm: &Vm, key: Value, value: Value) {
        let mut storage = self.storage.borrow_mut();
        let new_index = storage.entries.len();
        let index = *storage.indices.entry(ValueTraitsKey(key)).or_insert(new_index);
        if index != new_index {
            storage.entries[index].value = value;
            return;
        }
        let insertion_id = storage.next_insertion_id;
        storage.next_insertion_id += 1;
        storage.entries.push(StoredEntry {
            key,
            value,
            insertion_id,
        });
        let (old_size, new_size) = storage.update_external_memory_size();
        drop(storage);
        vm.heap().account_external_memory_change(old_size, new_size);
    }

    pub fn map_size(&self) -> usize {
        self.storage.borrow().indices.len()
    }

    /// Calls the callback with the key and value of every entry, in insertion order.
    /// The callback must not modify the map.
    pub fn for_each_entry(&self, mut callback: impl FnMut(Value, Value)) {
        let (generation, entry_count) = {
            let storage = self.storage.borrow();
            (storage.generation, storage.entries.len())
        };
        for index in 0..entry_count {
            let (key, value) = {
                let entry = &self.storage.borrow().entries[index];
                (entry.key, entry.value)
            };
            if !key.is_empty() {
                callback(key, value);
            }
        }
        let storage = self.storage.borrow();
        assert!(generation == storage.generation && entry_count == storage.entries.len());
    }

    pub fn begin(&self) -> ConstIterator {
        ConstIterator {
            // SAFETY: Maps only exist as cells, since every way to create one allocates it.
            map: unsafe { Gc::from_ref(self) },
            index: Cell::new(0),
            generation: Cell::new(self.storage.borrow().generation),
            next_insertion_id: Cell::new(0),
            current_insertion_id: Cell::new(None),
        }
    }
}

/// An iterator that stays valid while the map is modified, with the visiting rules of the spec's index-based loops:
/// entries added during iteration are visited, removed entries are skipped, and entries that were moved by a compaction
/// or cleared are found again by their insertion ID.
#[derive(Trace)]
pub struct ConstIterator {
    map: Gc<Map>,

    /// The position of the current entry in the entries. Only meaningful while generation matches the map.
    index: Cell<usize>,
    generation: Cell<u64>,

    /// Every entry with a smaller insertion ID has already been visited or skipped.
    next_insertion_id: Cell<u64>,
    current_insertion_id: Cell<Option<u64>>,
}

impl ConstIterator {
    pub fn is_end(&self) -> bool {
        self.find_current_entry();
        self.index.get() >= self.map.storage.borrow().entries.len()
    }

    /// operator++. This moves past the entry that is_end() or current() found last, even if that entry has been
    /// removed since, so removing the current entry during iteration does not skip the entry after it.
    pub fn advance(&self) {
        if self.current_insertion_id.get().is_none() {
            self.find_current_entry();
            if self.current_insertion_id.get().is_none() {
                return;
            }
        }
        let current_insertion_id = self
            .current_insertion_id
            .take()
            .expect("the current entry was just found");
        self.next_insertion_id.set(current_insertion_id + 1);
        let storage = self.map.storage.borrow();
        let index = self.index.get();
        if self.generation.get() == storage.generation
            && index < storage.entries.len()
            && storage.entries[index].insertion_id < self.next_insertion_id.get()
        {
            self.index.set(index + 1);
        }
    }

    /// operator*.
    pub fn current(&self) -> Entry {
        self.find_current_entry();
        let storage = self.map.storage.borrow();
        let entry = &storage.entries[self.index.get()];
        Entry {
            key: entry.key,
            value: entry.value,
        }
    }

    fn find_current_entry(&self) {
        let storage = self.map.storage.borrow();
        let entries = &storage.entries;
        if self.generation.get() != storage.generation {
            self.index
                .set(storage.index_of_first_entry_not_inserted_before(self.next_insertion_id.get()));
            self.generation.set(storage.generation);
        }
        let mut index = self.index.get();
        while index < entries.len() && entries[index].is_removed() {
            index += 1;
        }
        self.index.set(index);
        if index < entries.len() {
            self.current_insertion_id.set(Some(entries[index].insertion_id));
        } else {
            self.current_insertion_id.set(None);
        }
    }
}
