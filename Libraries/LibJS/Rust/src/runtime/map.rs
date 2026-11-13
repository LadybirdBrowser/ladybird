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
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;
use crate::runtime::value_traits::ValueTraitsKey;

/// What a keyed collection keeps with each of its keys: a Map keeps a value, and a Set keeps nothing.
pub(crate) trait StoredValue: Copy + Trace {
    /// What the entry of a removed key holds.
    const REMOVED: Self;
}

impl StoredValue for Value {
    const REMOVED: Self = Value::UNDEFINED;
}

/// What a Set keeps with each of its values.
#[derive(Clone, Copy, Trace)]
pub(crate) struct NoValue;

impl StoredValue for NoValue {
    const REMOVED: Self = NoValue;
}

#[derive(Trace)]
struct StoredEntry<V: StoredValue> {
    key: Value,
    value: V,
    insertion_id: u64,
}

impl<V: StoredValue> StoredEntry<V> {
    fn is_removed(&self) -> bool {
        self.key.is_empty()
    }
}

/// A collection with at most this many entries (holes included) has no indices, and finds its keys with a linear search
/// through the entries. Most collections on the web hold only a few entries, and for those the hash table costs more
/// memory than the entries themselves.
const MAX_ENTRY_COUNT_WITHOUT_INDICES: usize = 8;

type Indices = HashMap<ValueTraitsKey, usize, foldhash::fast::RandomState>;

/// The entries of a Map or a Set.
pub(crate) struct Storage<V: StoredValue> {
    /// All entries in insertion order. Removed entries stay in place (as holes) until the next compaction, so the
    /// positions held by iterators stay valid. Compacting or clearing moves entries, which bumps generation.
    entries: Vec<StoredEntry<V>>,
    /// The position of every live entry by its key. None while there are few enough entries to search through.
    indices: Option<Box<Indices>>,
    removed_entry_count: usize,
    next_insertion_id: u64,
    generation: u64,
}

// SAFETY: Visits the key and value of every entry. The keys of the indices are the keys of the live entries.
unsafe impl<V: StoredValue> Trace for Storage<V> {
    fn trace(&self, visitor: &mut Visitor) {
        self.entries.trace(visitor);
    }
}

impl<V: StoredValue> Storage<V> {
    pub(crate) fn new(_: &DeferGc<'_>) -> Self {
        Self {
            entries: Vec::new(),
            indices: None,
            removed_entry_count: 0,
            next_insertion_id: 0,
            generation: 0,
        }
    }

    /// Frees the entries of a collection that is finalized.
    pub(crate) fn release(storage: &GcRefCell<Self>) {
        drop(storage.replace(Self {
            entries: Vec::new(),
            indices: None,
            removed_entry_count: 0,
            next_insertion_id: 0,
            generation: 0,
        }));
    }

    /// Changes the storage of a collection, and tells the heap how much the memory of the storage changed.
    pub(crate) fn modify<R>(storage: &GcRefCell<Self>, vm: &Vm, modify: impl FnOnce(&mut Self) -> R) -> R {
        let mut storage = storage.borrow_mut(vm);
        let old_size = storage.external_memory_size();
        let result = modify(&mut storage);
        let new_size = storage.external_memory_size();
        drop(storage);
        vm.heap().account_external_memory_change(old_size, new_size);
        result
    }

    /// Calls the callback with the key and value of every entry, in insertion order.
    /// The callback must not modify the collection.
    pub(crate) fn for_each_entry(storage: &GcRefCell<Self>, mut callback: impl FnMut(Value, V)) {
        let (generation, entry_count) = {
            let storage = storage.borrow();
            (storage.generation, storage.entries.len())
        };
        for index in 0..entry_count {
            let (key, value) = {
                let entry = &storage.borrow().entries[index];
                (entry.key, entry.value)
            };
            if !key.is_empty() {
                callback(key, value);
            }
        }
        let storage = storage.borrow();
        assert!(generation == storage.generation && entry_count == storage.entries.len());
    }

    pub(crate) fn external_memory_size(&self) -> usize {
        let indices_size = self.indices.as_ref().map_or(0, |indices| {
            size_of::<Indices>().saturating_add(indices.allocation_size())
        });
        (self.entries.capacity() * size_of::<StoredEntry<V>>()).saturating_add(indices_size)
    }

    pub(crate) fn len(&self) -> usize {
        self.entries.len() - self.removed_entry_count
    }

    pub(crate) fn clear(&mut self) {
        self.entries = Vec::new();
        self.indices = None;
        self.removed_entry_count = 0;
        self.generation += 1;
    }

    pub(crate) fn remove(&mut self, key: Value) -> bool {
        let index = match &mut self.indices {
            Some(indices) => indices.remove(&ValueTraitsKey(key)),
            None => self.index_of(key),
        };
        let Some(index) = index else {
            return false;
        };

        let entry = &mut self.entries[index];
        entry.key = Value::EMPTY;
        entry.value = V::REMOVED;
        self.removed_entry_count += 1;

        // Compact once removed entries outnumber the live ones, so removal stays amortized O(1).
        const MINIMUM_REMOVED_ENTRY_COUNT_FOR_COMPACTION: usize = 8;
        if self.removed_entry_count >= MINIMUM_REMOVED_ENTRY_COUNT_FOR_COMPACTION
            && self.removed_entry_count > self.len()
        {
            self.compact_entries();
        }
        true
    }

    pub(crate) fn get(&self, key: Value) -> Option<V> {
        let index = self.index_of(key)?;
        Some(self.entries[index].value)
    }

    pub(crate) fn contains(&self, key: Value) -> bool {
        self.index_of(key).is_some()
    }

    pub(crate) fn set(&mut self, key: Value, value: V) {
        if let Some(indices) = &mut self.indices {
            let new_index = self.entries.len();
            let index = *indices.entry(ValueTraitsKey(key)).or_insert(new_index);
            if index != new_index {
                self.entries[index].value = value;
                return;
            }
        } else {
            if let Some(index) = self.index_of(key) {
                self.entries[index].value = value;
                return;
            }
            if self.entries.len() == MAX_ENTRY_COUNT_WITHOUT_INDICES {
                if self.removed_entry_count > 0 {
                    self.compact_entries();
                } else {
                    self.build_indices();
                    let new_index = self.entries.len();
                    self.indices
                        .as_mut()
                        .expect("the indices were just built")
                        .insert(ValueTraitsKey(key), new_index);
                }
            }
        }
        self.reserve_one_entry();
        let insertion_id = self.next_insertion_id;
        self.next_insertion_id += 1;
        self.entries.push(StoredEntry {
            key,
            value,
            insertion_id,
        });
    }

    fn index_of(&self, key: Value) -> Option<usize> {
        let key = ValueTraitsKey(key);
        match &self.indices {
            Some(indices) => indices.get(&key).copied(),
            None => {
                // NB: The key of a removed entry is the empty value, so an empty key would find a removed entry.
                assert!(!key.0.is_empty());

                // Most keys are found by identity. Only strings and BigInts can be the same value with different
                // bits: every NaN is the canonical NaN.
                if let Some(index) = self.entries.iter().position(|entry| entry.key.0 == key.0.0) {
                    return Some(index);
                }
                if !(key.0.is_string() || key.0.is_bigint()) {
                    return None;
                }
                self.entries
                    .iter()
                    .position(|entry| !entry.is_removed() && ValueTraitsKey(entry.key) == key)
            }
        }
    }

    /// Makes room for one more entry. The entries of a small collection grow one step at a time (1, 2, 4), so that a
    /// collection with one or two entries does not pay for four.
    fn reserve_one_entry(&mut self) {
        let capacity = self.entries.capacity();
        if self.entries.len() == capacity && capacity < 4 {
            self.entries.reserve_exact(capacity.max(1));
        }
    }

    fn build_indices(&mut self) {
        // NB: The indices hold keys, so they are built in place, where the entries keep those keys alive.
        let indices = self.indices.insert(Box::new(Indices::with_capacity_and_hasher(
            self.entries.len() + 1,
            Default::default(),
        )));
        for (index, entry) in self.entries.iter().enumerate() {
            if !entry.is_removed() {
                indices.insert(ValueTraitsKey(entry.key), index);
            }
        }
    }

    fn index_of_first_entry_not_inserted_before(&self, insertion_id: u64) -> usize {
        // Entries are stored in insertion order, so their insertion IDs are increasing.
        self.entries.partition_point(|entry| entry.insertion_id < insertion_id)
    }

    fn compact_entries(&mut self) {
        let live_entry_count = self.len();

        // NB: A collection that keeps its indices until it is well below the limit does not build them again and again
        //     when its size moves around the limit.
        if live_entry_count <= MAX_ENTRY_COUNT_WITHOUT_INDICES / 2 {
            self.indices = None;
        }

        if let Some(indices) = &mut self.indices {
            let mut new_indices = vec![0usize; self.entries.len()];
            let mut new_entry_count = 0;
            for (i, new_index) in new_indices.iter_mut().enumerate() {
                if self.entries[i].is_removed() {
                    continue;
                }
                *new_index = new_entry_count;
                self.entries.swap(new_entry_count, i);
                new_entry_count += 1;
            }
            self.entries.truncate(new_entry_count);
            for index in indices.values_mut() {
                *index = new_indices[*index];
            }
        } else {
            self.entries.retain(|entry| !entry.is_removed());
        }
        assert!(self.entries.len() == live_entry_count);
        if self.entries.capacity() > live_entry_count * 2 {
            self.entries.shrink_to_fit();
        }

        self.removed_entry_count = 0;
        self.generation += 1;
    }
}

/// A position in the entries of a collection that stays valid while the collection is modified, with the visiting rules
/// of the spec's index-based loops: entries added during iteration are visited, removed entries are skipped, and
/// entries that were moved by a compaction or cleared are found again by their insertion ID.
#[derive(Trace)]
pub(crate) struct Position {
    /// The position of the current entry in the entries. Only meaningful while generation matches the storage.
    index: Cell<usize>,
    generation: Cell<u64>,

    /// Every entry with a smaller insertion ID has already been visited or skipped.
    next_insertion_id: Cell<u64>,
    current_insertion_id: Cell<Option<u64>>,
}

impl Position {
    pub(crate) fn first<V: StoredValue>(storage: &Storage<V>) -> Self {
        Self {
            index: Cell::new(0),
            generation: Cell::new(storage.generation),
            next_insertion_id: Cell::new(0),
            current_insertion_id: Cell::new(None),
        }
    }

    pub(crate) fn is_end<V: StoredValue>(&self, storage: &Storage<V>) -> bool {
        self.find_current_entry(storage);
        self.index.get() >= storage.entries.len()
    }

    /// This moves past the entry that is_end() or current() found last, even if that entry has been removed since, so
    /// removing the current entry during iteration does not skip the entry after it.
    pub(crate) fn advance<V: StoredValue>(&self, storage: &Storage<V>) {
        if self.current_insertion_id.get().is_none() {
            self.find_current_entry(storage);
            if self.current_insertion_id.get().is_none() {
                return;
            }
        }
        let current_insertion_id = self
            .current_insertion_id
            .take()
            .expect("the current entry was just found");
        self.next_insertion_id.set(current_insertion_id + 1);
        let index = self.index.get();
        if self.generation.get() == storage.generation
            && index < storage.entries.len()
            && storage.entries[index].insertion_id < self.next_insertion_id.get()
        {
            self.index.set(index + 1);
        }
    }

    pub(crate) fn current<V: StoredValue>(&self, storage: &Storage<V>) -> (Value, V) {
        self.find_current_entry(storage);
        let entry = &storage.entries[self.index.get()];
        (entry.key, entry.value)
    }

    fn find_current_entry<V: StoredValue>(&self, storage: &Storage<V>) {
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
    storage: GcRefCell<Storage<Value>>,
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
            .saturating_add(self.storage.borrow().external_memory_size())
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
        Storage::release(&self.storage);
    }
}

impl Map {
    pub fn new(defer_gc: &DeferGc<'_>, vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>) -> Map {
        Map {
            base: Object::new_with_prototype(
                vm,
                Self::CLASS,
                realm,
                Some(prototype),
                MayInterfereWithIndexedPropertyAccess::No,
            ),
            storage: GcRefCell::new(Storage::new(defer_gc)),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Map> {
        realm.create_object(vm, |defer_gc| {
            Map::new(defer_gc, vm, realm, realm.intrinsics().map_prototype(vm))
        })
    }

    // 24.1.3.1 Map.prototype.clear ( ), https://tc39.es/ecma262/#sec-map.prototype.clear
    pub fn map_clear(&self, vm: &Vm) {
        Storage::modify(&self.storage, vm, Storage::clear);
    }

    // 24.1.3.3 Map.prototype.delete ( key ), https://tc39.es/ecma262/#sec-map.prototype.delete
    pub fn map_remove(&self, vm: &Vm, key: Value) -> bool {
        Storage::modify(&self.storage, vm, |storage| storage.remove(key))
    }

    // 24.1.3.6 Map.prototype.get ( key ), https://tc39.es/ecma262/#sec-map.prototype.get
    pub fn map_get(&self, key: Value) -> Option<Value> {
        self.storage.borrow().get(key)
    }

    // 24.1.3.7 Map.prototype.has ( key ), https://tc39.es/ecma262/#sec-map.prototype.has
    pub fn map_has(&self, key: Value) -> bool {
        self.storage.borrow().contains(key)
    }

    // 24.1.3.9 Map.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-map.prototype.set
    pub fn map_set(&self, vm: &Vm, key: Value, value: Value) {
        Storage::modify(&self.storage, vm, |storage| storage.set(key, value));
    }

    pub fn map_size(&self) -> usize {
        self.storage.borrow().len()
    }

    /// Calls the callback with the key and value of every entry, in insertion order.
    /// The callback must not modify the map.
    pub fn for_each_entry(&self, callback: impl FnMut(Value, Value)) {
        Storage::for_each_entry(&self.storage, callback);
    }

    pub fn begin(&self) -> ConstIterator {
        ConstIterator {
            map: Gc::from_ref(self),
            position: Position::first(&self.storage.borrow()),
        }
    }
}

/// An iterator that stays valid while the map is modified, with the visiting rules of Position.
#[derive(Trace)]
pub struct ConstIterator {
    map: Gc<Map>,
    position: Position,
}

impl ConstIterator {
    pub fn is_end(&self) -> bool {
        self.position.is_end(&self.map.storage.borrow())
    }

    /// operator++.
    pub fn advance(&self) {
        self.position.advance(&self.map.storage.borrow());
    }

    /// operator*.
    pub fn current(&self) -> Entry {
        let (key, value) = self.position.current(&self.map.storage.borrow());
        Entry { key, value }
    }
}
