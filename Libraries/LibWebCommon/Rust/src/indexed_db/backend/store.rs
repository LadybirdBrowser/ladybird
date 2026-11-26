/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Object stores and indexes as the backend holds them: records keyed in key order, with values that stay serialized

use std::collections::{BTreeMap, BTreeSet};
use std::rc::Rc;

use libipc_rust::wire::{Decode, Encode};

use crate::indexed_db::Name;

use crate::indexed_db::backend::{Error, ErrorKind};
use crate::indexed_db::key::{Key, KeyType, KeyValue};
use crate::indexed_db::key_generator::{KeyGenerator, MAX_KEY_GENERATOR_VALUE};
use crate::indexed_db::key_range::{KeyRange, OrderedKey};

/// A record's value, serialized by the agent that stored it, https://w3c.github.io/IndexedDB/#object-store-record
pub type Value = Rc<Vec<u8>>;

// https://w3c.github.io/IndexedDB/#key-path-construct
#[derive(Clone, Debug, PartialEq, Encode, Decode)]
pub enum KeyPath {
    String(Name),
    Sequence(Vec<Name>),
}

/// The binary key that an agent injects into a value in place of the key that the backend will generate for it,
/// before it extracts the value's index keys. The backend puts the generated key wherever an index key holds the
/// placeholder, since the agent can't know the key: it's only generated when the backend runs the operation.
pub const GENERATED_KEY_PLACEHOLDER: &[u8] = b"\xff\xfe\x00Ladybird generated key\x00\xfe\xff";

/// The keys an agent extracted from a value for one index, https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
#[derive(Clone, Encode, Decode)]
pub enum IndexKeys {
    /// The keys to store the record under, none when the index key path yields no valid key. A key that is, or holds,
    /// the generated-key placeholder gets the generated key there.
    Keys(Vec<Key>),
}

fn is_placeholder(key: &Key) -> bool {
    key.r#type() == KeyType::Binary && key.value_as_byte_buffer() == GENERATED_KEY_PLACEHOLDER
}

/// The index key with the generated key where the agent put the placeholder: where the index's key path is the object
/// store's, the whole key, or, for a sequence of key paths, the elements of the array key at the store's key path. The
/// agent leaves the placeholder at the store's key path only, so a binary key anywhere else is the application's.
fn with_generated_key(key: &Key, index_key_path: &KeyPath, store_key_path: &Name, generated: &Rc<Key>) -> Rc<Key> {
    match index_key_path {
        KeyPath::String(path) if path == store_key_path && is_placeholder(key) => generated.clone(),
        KeyPath::Sequence(paths) if key.r#type() == KeyType::Array && key.subkeys().len() == paths.len() => {
            Key::create_array(
                key.subkeys()
                    .iter()
                    .zip(paths)
                    .map(|(subkey, path)| {
                        if path == store_key_path && is_placeholder(subkey) {
                            generated.clone()
                        } else {
                            subkey.clone()
                        }
                    })
                    .collect(),
            )
        }
        _ => Rc::new(key.clone()),
    }
}

// https://w3c.github.io/IndexedDB/#index-construct
#[derive(Clone)]
pub struct Index {
    pub name: Name,
    pub key_path: KeyPath,
    pub unique: bool,
    pub multi_entry: bool,

    // The index's list of records, sorted on the index key and then on the referenced object store record's key.
    records: BTreeMap<OrderedKey, BTreeSet<OrderedKey>>,

    // The index keys of each referenced object store record, so removing a record's index records needs no scan.
    keys_of_record: BTreeMap<OrderedKey, Vec<OrderedKey>>,

    // The bytes that the index's records take, by the measure that storage quotas use.
    bytes: u64,
}

impl Index {
    pub fn new(name: Name, key_path: KeyPath, unique: bool, multi_entry: bool) -> Self {
        Self {
            name,
            key_path,
            unique,
            multi_entry,
            records: BTreeMap::new(),
            keys_of_record: BTreeMap::new(),
            bytes: 0,
        }
    }

    fn has_key_for_other_record(&self, key: &OrderedKey, primary_key: &OrderedKey) -> bool {
        self.records
            .get(key)
            .is_some_and(|referenced| referenced.iter().any(|entry| entry != primary_key))
    }

    fn insert(&mut self, key: OrderedKey, primary_key: OrderedKey) {
        if self.records.entry(key.clone()).or_default().insert(primary_key.clone()) {
            self.bytes += index_record_bytes(&key, &primary_key);
            self.keys_of_record.entry(primary_key).or_default().push(key);
        }
    }

    fn remove_records_referencing(&mut self, primary_key: &OrderedKey) {
        let Some(keys) = self.keys_of_record.remove(primary_key) else {
            return;
        };
        for key in keys {
            if let Some(referenced) = self.records.get_mut(&key) {
                if referenced.remove(primary_key) {
                    self.bytes -= index_record_bytes(&key, primary_key);
                }
                if referenced.is_empty() {
                    self.records.remove(&key);
                }
            }
        }
    }

    fn keys_of(&self, primary_key: &OrderedKey) -> Vec<OrderedKey> {
        self.keys_of_record.get(primary_key).cloned().unwrap_or_default()
    }

    /// The bytes that the index's records take, by the measure that storage quotas use.
    pub(crate) fn bytes(&self) -> u64 {
        self.bytes
    }

    /// The number of records in the index.
    pub fn len(&self) -> usize {
        self.keys_of_record.values().map(Vec::len).sum()
    }

    pub fn is_empty(&self) -> bool {
        self.records.is_empty()
    }

    /// The index records whose index keys are in a range, in index order (or the reverse), from the start of the range
    /// on: each index key, and the key of the object store record it references. Within the index key of `skip`, the
    /// records before its object store key (after it, in reverse) are left out, so a cursor that continues within a
    /// run of equal index keys needn't walk the run from its start again.
    pub fn records_in_range_from<'a>(
        &'a self,
        range: &KeyRange,
        reverse: bool,
        skip: Option<(OrderedKey, OrderedKey)>,
    ) -> Box<dyn Iterator<Item = (&'a OrderedKey, &'a OrderedKey)> + 'a> {
        use core::ops::Bound;
        if range.is_empty() {
            return Box::new(core::iter::empty());
        }
        let groups = self.records.range(range.bounds());
        let groups: Box<dyn Iterator<Item = (&'a OrderedKey, &'a BTreeSet<OrderedKey>)> + 'a> = if reverse {
            Box::new(groups.rev())
        } else {
            Box::new(groups)
        };
        Box::new(groups.flat_map(move |(key, referenced)| {
            let records: Box<dyn Iterator<Item = &'a OrderedKey> + 'a> = match &skip {
                Some((group, from)) if group == key => {
                    if reverse {
                        Box::new(
                            referenced
                                .range((Bound::Unbounded, Bound::Included(from.clone())))
                                .rev(),
                        )
                    } else {
                        Box::new(referenced.range((Bound::Included(from.clone()), Bound::Unbounded)))
                    }
                }
                _ if reverse => Box::new(referenced.iter().rev()),
                _ => Box::new(referenced.iter()),
            };
            records.map(move |primary_key| (key, primary_key))
        }))
    }

    /// The index records whose index keys are in a range, in index order: each index key, and the key of the object
    /// store record it references.
    pub fn records_in_range<'a>(
        &'a self,
        range: &KeyRange,
    ) -> Box<dyn DoubleEndedIterator<Item = (&'a OrderedKey, &'a OrderedKey)> + 'a> {
        if range.is_empty() {
            return Box::new(core::iter::empty());
        }
        Box::new(
            self.records
                .range(range.bounds())
                .flat_map(|(key, referenced)| referenced.iter().map(move |primary_key| (key, primary_key))),
        )
    }
}

/// The keys for one of the indexes of an object store that a record goes into.
#[derive(Clone, Encode, Decode)]
pub struct NamedIndexKeys {
    pub index: Name,
    pub keys: IndexKeys,
}

/// What undoes one change of a transaction, https://w3c.github.io/IndexedDB/#abort-a-transaction
pub(crate) enum Undo {
    /// Put back an object store's record, or remove the one a transaction added.
    Record {
        store: Name,
        key: OrderedKey,
        previous: Option<Value>,
        previous_index_records: Vec<(Name, Vec<OrderedKey>)>,
    },
    /// Put back a key generator's current number.
    KeyGenerator { store: Name, current_number: u64 },
}

// https://w3c.github.io/IndexedDB/#object-store-construct
#[derive(Clone)]
pub struct ObjectStore {
    pub name: Name,
    pub key_path: Option<KeyPath>,
    pub key_generator: Option<KeyGenerator>,
    // NB: Records only change through insert_record() and remove_record(), which keep the byte count.
    pub records: BTreeMap<OrderedKey, Value>,
    pub indexes: Vec<Index>,

    // The bytes that the store's records take, by the measure that storage quotas use.
    bytes: u64,

    // The indexes whose records an agent is computing from the records it was handed, and the keys of the records that
    // changed since, whose index records the agent's computation can't be trusted for.
    populating: Vec<(Name, BTreeSet<OrderedKey>)>,
}

impl ObjectStore {
    pub fn new(name: Name, key_path: Option<KeyPath>, auto_increment: bool) -> Self {
        Self {
            name,
            key_path,
            key_generator: auto_increment.then(KeyGenerator::new),
            records: BTreeMap::new(),
            indexes: Vec::new(),
            populating: Vec::new(),
            bytes: 0,
        }
    }

    /// The bytes that the store's records and its indexes' records take, by the measure that storage quotas use.
    pub(crate) fn bytes(&self) -> u64 {
        self.bytes + self.indexes.iter().map(Index::bytes).sum::<u64>()
    }

    /// The bytes that a record and its index records take, which replacing or deleting it gives back.
    pub(crate) fn bytes_of_record(&self, key: &OrderedKey) -> u64 {
        let Some((stored_key, value)) = self.records.get_key_value(key) else {
            return 0;
        };
        let index_bytes: u64 = self
            .indexes
            .iter()
            .flat_map(|index| index.keys_of(stored_key))
            .map(|index_key| index_record_bytes(&index_key, stored_key))
            .sum();
        record_bytes(stored_key, value) + index_bytes
    }

    fn insert_record(&mut self, key: OrderedKey, value: Value) {
        self.bytes += record_bytes(&key, &value);
        if let Some((old_key, old_value)) = self.records.remove_entry(&key) {
            self.bytes -= record_bytes(&old_key, &old_value);
        }
        self.records.insert(key, value);
    }

    fn remove_record(&mut self, key: &OrderedKey) -> Option<Value> {
        let (old_key, old_value) = self.records.remove_entry(key)?;
        self.bytes -= record_bytes(&old_key, &old_value);
        Some(old_value)
    }

    /// Starts tracking the records that change while an agent computes a new index's records.
    pub(crate) fn begin_population(&mut self, index: &Name) {
        self.populating.retain(|(name, _)| name != index);
        self.populating.push((index.clone(), BTreeSet::new()));
    }

    /// Keeps tracking a population under an index's new name.
    pub(crate) fn rename_population(&mut self, index: &Name, new_name: &Name) {
        for (name, _) in &mut self.populating {
            if name == index {
                *name = new_name.clone();
            }
        }
    }

    fn record_changed(&mut self, key: &OrderedKey) {
        for (_, changed) in &mut self.populating {
            changed.insert(key.clone());
        }
    }

    pub fn index(&self, name: &Name) -> Option<&Index> {
        self.indexes.iter().find(|index| index.name == *name)
    }

    pub fn index_mut(&mut self, name: &Name) -> Option<&mut Index> {
        self.indexes.iter_mut().find(|index| index.name == *name)
    }

    /// The records whose keys are in a range, in key order.
    pub fn records_in_range<'a>(
        &'a self,
        range: &KeyRange,
    ) -> Box<dyn DoubleEndedIterator<Item = (&'a OrderedKey, &'a Value)> + 'a> {
        if range.is_empty() {
            return Box::new(core::iter::empty());
        }
        Box::new(self.records.range(range.bounds()))
    }

    fn index_records_of(&self, primary_key: &OrderedKey) -> Vec<(Name, Vec<OrderedKey>)> {
        self.indexes
            .iter()
            .map(|index| (index.name.clone(), index.keys_of(primary_key)))
            .collect()
    }

    // https://w3c.github.io/IndexedDB/#generate-a-key
    fn generate_a_key(&mut self) -> Result<u64, Error> {
        // 1. Let generator be store's key generator.
        let generator = self.key_generator.as_mut().expect("the store uses a key generator");

        // 2. Let key be generator's current number.
        let key = generator.current_number();

        // 3. If key is greater than 2^53 (9007199254740992), then return failure.
        // NB: As a double, 2^53 + 1 would round to 2^53.
        #[allow(
            clippy::cast_possible_truncation,
            clippy::cast_sign_loss,
            reason = "2^53 is an integer"
        )]
        if key > MAX_KEY_GENERATOR_VALUE as u64 {
            return Err(Error::new(ErrorKind::Constraint, "Key is greater than 2^53"));
        }

        // 4. Increase generator's current number by 1.
        generator.increment(1);

        // 5. Return key.
        Ok(key)
    }

    // https://w3c.github.io/IndexedDB/#possibly-update-the-key-generator
    fn possibly_update_the_key_generator(&mut self, key: &Key) {
        // 1. If the type of key is not number, abort these steps.
        if key.r#type() != KeyType::Number {
            return;
        }

        // 2. Let value be the value of key.
        // 3. Set value to the minimum of value and 2^53 (9007199254740992).
        // 4. Set value to the largest integer not greater than value.
        let value = key.value_as_double().min(MAX_KEY_GENERATOR_VALUE).floor();

        // 5. Let generator be store's key generator.
        let generator = self.key_generator.as_mut().expect("the store uses a key generator");

        // 6. If value is greater than or equal to generator's current number, then set generator's current number to
        //    value + 1.
        if value >= generator.current_number() as f64 {
            generator.set(value as u64 + 1);
        }
    }

    // https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
    pub(crate) fn store_a_record(
        &mut self,
        value: Value,
        key: Option<Key>,
        index_keys: &[NamedIndexKeys],
        no_overwrite: bool,
        undo: &mut Vec<Undo>,
    ) -> Result<Key, Error> {
        let generator_before = self.key_generator.as_ref().map(KeyGenerator::current_number);
        // NB: Only the agent's index keys for a record whose key the backend generates, in a store with in-line keys,
        //     can hold the placeholder: The agent puts it there for the key it can't know. Any other record's index
        //     keys are the application's own.
        let generated_at = match (&self.key_path, key.is_none() && self.key_generator.is_some()) {
            (Some(KeyPath::String(path)), true) => Some(path.clone()),
            _ => None,
        };

        // 1. If store uses a key generator, then:
        let key = if self.key_generator.is_some() {
            match key {
                // 1. If key is undefined, then:
                None => {
                    // 1. Let key be the result of generating a key for store.
                    // 2. If key is failure, then this operation failed with a "ConstraintError" DOMException. Abort
                    //    this algorithm without taking any further steps.
                    // 3. If store also uses in-line keys, then run inject a key into a value using a key path with
                    //    value, key and store's key path.
                    // NB: The agent injects the key when it reads the value back, since only it can run script.
                    Key::create_number(self.generate_a_key()? as f64)
                }
                // 2. Otherwise, run possibly update the key generator for store with key.
                Some(key) => {
                    self.possibly_update_the_key_generator(&key);
                    Rc::new(key)
                }
            }
        } else {
            Rc::new(key.ok_or_else(|| Error::new(ErrorKind::Data, "The object store needs a key"))?)
        };
        let ordered_key = OrderedKey(key.clone());
        let key_for_index = key.clone();

        let index_keys_for = |index: &Index| -> Vec<OrderedKey> {
            let Some(entry) = index_keys.iter().find(|entry| entry.index == index.name) else {
                return Vec::new();
            };
            match &entry.keys {
                IndexKeys::Keys(keys) => keys
                    .iter()
                    .map(|key| {
                        OrderedKey(match &generated_at {
                            Some(store_key_path) => {
                                with_generated_key(key, &index.key_path, store_key_path, &key_for_index)
                            }
                            None => Rc::new(key.clone()),
                        })
                    })
                    .collect(),
            }
        };

        let restore_generator = |store: &mut ObjectStore| {
            if let (Some(generator), Some(number)) = (store.key_generator.as_mut(), generator_before) {
                generator.set(number);
            }
        };

        // 2. If the no-overwrite flag was given to these steps and is true, and a record already exists in store with
        //    its key equal to key, then this operation failed with a "ConstraintError" DOMException. Abort this
        //    algorithm without taking any further steps.
        if no_overwrite && self.records.contains_key(&ordered_key) {
            restore_generator(self);
            return Err(Error::new(
                ErrorKind::Constraint,
                "Record already exists for this key in the object store",
            ));
        }

        // 5. For each index which references store:
        //    5. If index's unique flag is true and index already contains a record with key equal to any of the keys
        //       in index key, then this operation failed with a "ConstraintError" DOMException. Abort this algorithm
        //       without taking any further steps.
        // NB: This checks before anything changes, so a failed request leaves the store as it was. A record that this
        //     one replaces doesn't count against it.
        for index in &self.indexes {
            if !index.unique {
                continue;
            }
            for index_key in index_keys_for(index) {
                if index.has_key_for_other_record(&index_key, &ordered_key) {
                    restore_generator(self);
                    return Err(Error::new(
                        ErrorKind::Constraint,
                        "Record already exists for this key in a unique index",
                    ));
                }
            }
        }

        if let Some(number) = generator_before
            && Some(number) != self.key_generator.as_ref().map(KeyGenerator::current_number)
        {
            undo.push(Undo::KeyGenerator {
                store: self.name.clone(),
                current_number: number,
            });
        }

        undo.push(Undo::Record {
            store: self.name.clone(),
            key: ordered_key.clone(),
            previous: self.records.get(&ordered_key).cloned(),
            previous_index_records: self.index_records_of(&ordered_key),
        });
        self.record_changed(&ordered_key);

        // 3. If a record already exists in store with its key equal to key, then remove the record from store using
        //    delete records from an object store.
        for index in &mut self.indexes {
            index.remove_records_referencing(&ordered_key);
        }

        // 4. Store a record in store containing key as its key and value as its value. The record is stored in the
        //    object store's list of records such that the list is sorted according to the key of the records in
        //    ascending order.
        self.insert_record(ordered_key.clone(), value);

        // 5. For each index which references store:
        //    6. If index's multiEntry flag is false, or if index key is not an array key, then store a record in index
        //       containing index key as its key and key as its value.
        //    7. If index's multiEntry flag is true and index key is an array key, then for each subkey of the subkeys
        //       of index key store a record in index containing subkey as its key and key as its value.
        // NB: The agent already split a multiEntry index's array key into its distinct subkeys.
        let keys_per_index: Vec<Vec<OrderedKey>> = self.indexes.iter().map(index_keys_for).collect();
        for (index, keys) in self.indexes.iter_mut().zip(keys_per_index) {
            for index_key in keys {
                index.insert(index_key, ordered_key.clone());
            }
        }

        // 6. Return key.
        Ok(key.as_ref().clone())
    }

    // https://w3c.github.io/IndexedDB/#delete-records-from-an-object-store
    pub(crate) fn delete_records(&mut self, range: &KeyRange, undo: &mut Vec<Undo>) {
        // 1. Remove all records, if any, from store's list of records with key in range.
        let keys: Vec<OrderedKey> = self.records_in_range(range).map(|(key, _)| key.clone()).collect();
        for key in keys {
            self.record_changed(&key);
            undo.push(Undo::Record {
                store: self.name.clone(),
                key: key.clone(),
                previous: self.remove_record(&key),
                previous_index_records: self.index_records_of(&key),
            });

            // 2. For each index which references store, remove every record from index's list of records whose value
            //    is in range, if any such records exist.
            for index in &mut self.indexes {
                index.remove_records_referencing(&key);
            }
        }
    }

    // https://w3c.github.io/IndexedDB/#clear-an-object-store
    pub(crate) fn clear(&mut self, undo: &mut Vec<Undo>) {
        // 1. Remove all records from store.
        // 2. In all indexes which reference store, remove all records.
        self.delete_records(&KeyRange::unbounded(), undo);
    }

    /// Puts a record back as an undo entry recorded it.
    pub(crate) fn restore_record(
        &mut self,
        key: OrderedKey,
        previous: Option<Value>,
        previous_index_records: Vec<(Name, Vec<OrderedKey>)>,
    ) {
        for index in &mut self.indexes {
            index.remove_records_referencing(&key);
        }
        match previous {
            Some(value) => {
                self.insert_record(key.clone(), value);
                for (name, keys) in previous_index_records {
                    if let Some(index) = self.index_mut(&name) {
                        for index_key in keys {
                            index.insert(index_key, key.clone());
                        }
                    }
                }
            }
            None => {
                self.remove_record(&key);
            }
        }
    }

    /// Ends the population of a new index: the index records an agent computed for the records it was handed. A record
    /// that changed or went away since got its index records when it changed, if it still exists.
    pub(crate) fn finish_population(&mut self, index_name: &Name, entries: &[(Key, Vec<Key>)]) -> Result<(), Error> {
        let changed = self
            .populating
            .iter()
            .position(|(name, _)| name == index_name)
            .map(|position| self.populating.remove(position).1)
            .unwrap_or_default();
        for (primary_key, keys) in entries {
            let ordered = OrderedKey(Rc::new(primary_key.clone()));
            if changed.contains(&ordered) || !self.records.contains_key(&ordered) {
                continue;
            }
            self.add_index_keys(index_name, primary_key, keys)?;
        }
        Ok(())
    }

    /// Adds the records of a new index for an existing record, as an agent populating the index computed them.
    pub(crate) fn add_index_keys(&mut self, index_name: &Name, primary_key: &Key, keys: &[Key]) -> Result<(), Error> {
        let primary_key = OrderedKey(Rc::new(primary_key.clone()));
        let index = self
            .index_mut(index_name)
            .ok_or_else(|| Error::new(ErrorKind::NotFound, "No index with this name"))?;
        for key in keys {
            let key = OrderedKey(Rc::new(key.clone()));
            if index.unique && index.has_key_for_other_record(&key, &primary_key) {
                return Err(Error::new(
                    ErrorKind::Constraint,
                    "Record already exists for this key in a unique index",
                ));
            }
            index.insert(key, primary_key.clone());
        }
        Ok(())
    }
}

/// The bytes that a key takes, by the measure that storage quotas use: A string counts two bytes a code unit, and an
/// array counts the keys it holds, and 8 bytes for each of them, so even an array of empty arrays takes room.
pub(crate) fn key_bytes(key: &Key) -> u64 {
    match key.value() {
        KeyValue::Double(_) => 8,
        KeyValue::String(string) => 2 * string.length_in_code_units() as u64,
        KeyValue::Bytes(bytes) => bytes.len() as u64,
        KeyValue::Keys(keys) => keys.iter().map(|key| 8 + key_bytes(key)).sum(),
    }
}

fn record_bytes(key: &OrderedKey, value: &Value) -> u64 {
    key_bytes(key.key()) + value.len() as u64
}

fn index_record_bytes(key: &OrderedKey, primary_key: &OrderedKey) -> u64 {
    key_bytes(key.key()) + key_bytes(primary_key.key())
}
