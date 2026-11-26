/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keeping a storage key's databases from one run to the next. The backend holds the databases it has loaded in
//! memory, loads a storage key's databases when the first client for the key connects, and hands each committed
//! change to its persistence as rows of opaque bytes: a row for each database's shape, and a row for each record, which
//! holds the record's index keys too, since computing those again takes the agent that ran script for them.

use std::collections::{BTreeSet, HashMap, HashSet};
use std::rc::Rc;

use libipc_rust::wire::{Decode, Decoder, Encode, Encoder};

use super::store::{Index, KeyPath, ObjectStore, Undo, Value};
use super::{Backend, Database, Transaction};
use crate::indexed_db::Name;
use crate::indexed_db::key::Key;
use crate::indexed_db::key_range::OrderedKey;

/// The version of the persisted rows' format, which a run that finds another one leaves alone.
const FORMAT: u32 = 1;

/// One change to the rows of a database.
pub enum Write {
    /// The database's shape: its version, and its object stores and indexes.
    Metadata(Vec<u8>),
    Record {
        store: u64,
        key: Vec<u8>,
        value: Value,
        index_keys: Vec<u8>,
    },
    DeleteRecord {
        store: u64,
        key: Vec<u8>,
    },
    /// Every record of an object store.
    DeleteStore(u64),
    /// The database, and all of its records.
    DeleteDatabase,
}

/// What receives a database's row as it loads: its name, and its shape.
pub type DatabaseRowSink<'a> = dyn FnMut(&[u8], &[u8]) + 'a;

/// What receives a record's row as it loads: its database's name, its object store's id, its key, its value, and its
/// index keys.
pub type RecordRowSink<'a> = dyn FnMut(&[u8], u64, &[u8], &[u8], &[u8]) + 'a;

/// Where a browsing session's databases are kept between runs.
pub trait Persistence {
    /// Hands over the rows of every database of a storage key: each database's name and shape, and then each record
    /// with the name of its database. Returns whether every row could be read; it hands over none if not.
    fn load(
        &mut self,
        storage_key: &str,
        on_database: &mut DatabaseRowSink<'_>,
        on_record: &mut RecordRowSink<'_>,
    ) -> bool;

    /// Applies the changes to one database's rows, all or none, and returns whether it did.
    fn write(&mut self, storage_key: &str, database: &[u8], writes: Vec<Write>) -> bool;
}

#[derive(Encode, Decode)]
struct PersistedIndex {
    id: u64,
    name: Name,
    key_path: KeyPath,
    unique: bool,
    multi_entry: bool,
}

#[derive(Encode, Decode)]
struct PersistedObjectStore {
    id: u64,
    name: Name,
    key_path: Option<KeyPath>,
    key_generator: Option<u64>,
    indexes: Vec<PersistedIndex>,
}

#[derive(Encode, Decode)]
struct PersistedDatabase {
    format: u32,
    version: u64,
    next_id: u64,
    object_stores: Vec<PersistedObjectStore>,
}

struct LoadedRecord {
    database: Vec<u8>,
    store: u64,
    key: Key,
    value: Value,
    index_keys: Vec<(u64, Vec<Key>)>,
}

#[derive(Encode, Decode)]
struct PersistedIndexKeys {
    index: u64,
    keys: Vec<Key>,
}

/// The value's bytes, or none if it can't be encoded, which fails the write that needs them.
fn encode<T: Encode + ?Sized>(value: &T) -> Option<Vec<u8>> {
    let mut encoder = Encoder::new();
    encoder.encode(value).ok()?;
    Some(encoder.finish().0)
}

fn decode<T: Decode>(bytes: &[u8]) -> Option<T> {
    Decoder::new(bytes, Vec::new()).decode::<T>().ok()
}

/// The database's shape, with the key generators of `committed` where they differ from the live ones.
fn metadata_of(database: &Database, committed: &HashMap<Name, u64>) -> Option<Vec<u8>> {
    encode(&PersistedDatabase {
        format: FORMAT,
        version: database.version,
        next_id: database.next_id,
        object_stores: database
            .object_stores
            .iter()
            .map(|store| PersistedObjectStore {
                id: store.id,
                name: store.name.clone(),
                key_path: store.key_path.clone(),
                key_generator: store.key_generator.as_ref().map(|generator| {
                    committed
                        .get(&store.name)
                        .copied()
                        .unwrap_or(generator.current_number())
                }),
                indexes: store
                    .indexes
                    .iter()
                    .map(|index| PersistedIndex {
                        id: index.id,
                        name: index.name.clone(),
                        key_path: index.key_path.clone(),
                        unique: index.unique,
                        multi_entry: index.multi_entry,
                    })
                    .collect(),
            })
            .collect(),
    })
}

fn database_from_metadata(name: Name, bytes: &[u8]) -> Option<Database> {
    let persisted = decode::<PersistedDatabase>(bytes)?;
    if persisted.format != FORMAT {
        return None;
    }
    let mut database = Database::new(name);
    database.version = persisted.version;
    database.next_id = persisted.next_id;
    for persisted_store in persisted.object_stores {
        let mut store = ObjectStore::new(
            persisted_store.id,
            persisted_store.name,
            persisted_store.key_path,
            persisted_store.key_generator.is_some(),
        );
        if let (Some(generator), Some(current_number)) = (store.key_generator.as_mut(), persisted_store.key_generator) {
            generator.set(current_number);
        }
        for index in persisted_store.indexes {
            store.indexes.push(Index::new(
                index.id,
                index.name,
                index.key_path,
                index.unique,
                index.multi_entry,
            ));
        }
        database.object_stores.push(store);
    }
    Some(database)
}

fn record_write(store: &ObjectStore, key: &OrderedKey) -> Option<Write> {
    Some(match store.records.get_key_value(key) {
        // NB: The key that the store holds is the one to persist: An equal key can differ in its bytes (0 and -0).
        Some((stored_key, value)) => Write::Record {
            store: store.id,
            key: encode(stored_key.key().as_ref())?,
            value: Rc::clone(value),
            index_keys: encode(
                &store
                    .index_keys_by_id(stored_key)
                    .into_iter()
                    .map(|(index, keys)| PersistedIndexKeys { index, keys })
                    .collect::<Vec<_>>(),
            )?,
        },
        None => Write::DeleteRecord {
            store: store.id,
            key: encode(key.key().as_ref())?,
        },
    })
}

/// The record writes for the records that undo entries name, each once, as the records are now.
fn touched_record_writes(
    database: &Database,
    undo: &[Undo],
    skip_stores: &HashSet<u64>,
    writes: &mut Vec<Write>,
) -> Option<()> {
    let mut seen: BTreeSet<(u64, OrderedKey)> = BTreeSet::new();
    for entry in undo {
        let Undo::Record { store_id, key, .. } = entry else {
            continue;
        };
        if skip_stores.contains(store_id) || !seen.insert((*store_id, key.clone())) {
            continue;
        }
        // NB: An object store that the transaction deleted lost its rows with it.
        if let Some(store) = database.object_stores.iter().find(|store| store.id == *store_id) {
            // NB: The first undo entry of a key holds the key as it was before, if the record existed. An equal key
            //     can differ in its bytes (0 and -0), so a record that's now under the other one leaves its old row.
            let old_key = encode(key.key().as_ref())?;
            if let Some((stored_key, _)) = store.records.get_key_value(key)
                && encode(stored_key.key().as_ref())? != old_key
            {
                writes.push(Write::DeleteRecord {
                    store: store.id,
                    key: old_key,
                });
            }
            writes.push(record_write(store, key)?);
        }
    }
    Some(())
}

impl Backend {
    /// Loads the databases of a storage key that the persistence holds, the first time a client for it connects.
    pub(super) fn load_storage_key(&mut self, storage_key: &str) {
        if self.loaded.contains(storage_key) {
            return;
        }
        let Some(persistence) = self.persistence.as_mut() else {
            self.loaded.insert(storage_key.to_owned());
            return;
        };
        let mut databases: Vec<(Vec<u8>, Database)> = Vec::new();
        let mut unreadable: Vec<Name> = Vec::new();
        let mut records: Vec<LoadedRecord> = Vec::new();
        let mut broken: HashSet<Vec<u8>> = HashSet::new();
        let loaded = persistence.load(
            storage_key,
            &mut |name, metadata| {
                let Some(name_value) = decode::<Name>(name) else {
                    return;
                };
                match database_from_metadata(name_value.clone(), metadata) {
                    Some(database) => databases.push((name.to_vec(), database)),
                    // NB: A database that another version of Ladybird wrote, in a format this one can't read, stays
                    //     where it is: Opening it fails, so nothing here replaces it, though deleting it removes its rows.
                    None => unreadable.push(name_value),
                }
            },
            &mut |database, store, key, value, index_keys| {
                let (Some(key), Some(index_keys)) = (decode::<Key>(key), decode::<Vec<PersistedIndexKeys>>(index_keys))
                else {
                    // NB: A database missing a record it has in the profile would replace the record's row on its
                    //     next write, so it's left alone like one whose shape can't be read.
                    broken.insert(database.to_vec());
                    return;
                };
                let index_keys = index_keys.into_iter().map(|entry| (entry.index, entry.keys)).collect();
                records.push(LoadedRecord {
                    database: database.to_vec(),
                    store,
                    key,
                    value: Rc::new(value.to_vec()),
                    index_keys,
                });
            },
        );
        if !loaded {
            // NB: Nothing of the storage key's databases may change while the profile has rows this didn't read. The
            //     next client for the storage key tries again.
            self.unavailable.insert(storage_key.to_owned());
            return;
        }
        self.loaded.insert(storage_key.to_owned());
        self.unavailable.remove(storage_key);
        databases.retain(|(name, database)| {
            if broken.contains(name) {
                unreadable.push(database.name.clone());
                return false;
            }
            true
        });

        let mut by_name: HashMap<Vec<u8>, usize> = HashMap::new();
        for (position, (name, _)) in databases.iter().enumerate() {
            by_name.insert(name.clone(), position);
        }
        for record in records {
            let Some(position) = by_name.get(&record.database) else {
                continue;
            };
            let database = &mut databases[*position].1;
            if let Some(store) = database.object_stores.iter_mut().find(|store| store.id == record.store) {
                store.load_record(record.key, record.value, record.index_keys);
            }
        }
        self.databases
            .entry(storage_key.to_owned())
            .or_default()
            .extend(databases.into_iter().map(|(_, database)| database));
        self.unreadable
            .extend(unreadable.into_iter().map(|name| (storage_key.to_owned(), name)));
    }

    /// Hands the writes to the persistence, and returns whether they were made: always, without a persistence.
    fn persist(&mut self, storage_key: &str, database: &Name, writes: Vec<Write>) -> bool {
        if writes.is_empty() {
            return true;
        }
        let Some(database) = encode(database) else {
            return false;
        };
        match self.persistence.as_mut() {
            Some(persistence) => persistence.write(storage_key, &database, writes),
            None => true,
        }
    }

    /// Persists what a transaction that commits changed, and returns whether it could.
    pub(super) fn persist_commit(&mut self, transaction: &Transaction) -> bool {
        if self.persistence.is_none() || transaction.mode == super::Mode::ReadOnly {
            return true;
        }
        let Some(database) = self.database(&transaction.storage_key, &transaction.database) else {
            return true;
        };
        // NB: Another transaction that's under way may have moved a key generator of an object store outside this
        //     one's scope; what's written is the number it had before, which its first undo entry for it holds.
        let mut committed: HashMap<Name, u64> = HashMap::new();
        for id in &database.transactions {
            let Some(other) = self.transactions.get(id) else {
                continue;
            };
            for undo in &other.undo {
                if let Undo::KeyGenerator { store, current_number } = undo {
                    committed.entry(store.clone()).or_insert(*current_number);
                }
            }
        }
        let Some(writes) = Self::commit_writes(database, transaction, &committed) else {
            return false;
        };
        let (storage_key, name) = (transaction.storage_key.clone(), transaction.database.clone());
        self.persist(&storage_key, &name, writes)
    }

    /// The writes that persist what a transaction that commits changed, or none if some can't be encoded.
    fn commit_writes(
        database: &Database,
        transaction: &Transaction,
        committed: &HashMap<Name, u64>,
    ) -> Option<Vec<Write>> {
        let mut writes = Vec::new();
        // NB: The shape row changes with an upgrade, and with a key generator that the transaction moved.
        let moved_a_key_generator = transaction
            .undo
            .iter()
            .any(|undo| matches!(undo, Undo::KeyGenerator { .. }));
        if transaction.snapshot.is_some() || moved_a_key_generator {
            writes.push(Write::Metadata(metadata_of(database, committed)?));
        }
        let mut rewritten: HashSet<u64> = HashSet::new();
        if let Some(snapshot) = &transaction.snapshot {
            // NB: An upgrade can create, delete and rename object stores and indexes. An object store that's new, or
            //     whose set of indexes changed, gets all of its rows again, since its records' index keys changed.
            for store in &database.object_stores {
                let before = snapshot.object_stores.iter().find(|before| before.id == store.id);
                let same_indexes = before.is_some_and(|before| {
                    let ids = |store: &ObjectStore| store.indexes.iter().map(|index| index.id).collect::<HashSet<_>>();
                    ids(before) == ids(store)
                });
                if same_indexes {
                    continue;
                }
                rewritten.insert(store.id);
                writes.push(Write::DeleteStore(store.id));
                for key in store.records.keys() {
                    writes.push(record_write(store, key)?);
                }
            }
            for before in &snapshot.object_stores {
                if !database.object_stores.iter().any(|store| store.id == before.id) {
                    rewritten.insert(before.id);
                    writes.push(Write::DeleteStore(before.id));
                }
            }
        }
        touched_record_writes(database, &transaction.undo, &rewritten, &mut writes)?;
        Some(writes)
    }

    /// Persists the records that changes outside of any transaction touched, and returns whether it could.
    pub(super) fn persist_records(&mut self, storage_key: &str, name: &Name, undo: &[Undo]) -> bool {
        if self.persistence.is_none() {
            return true;
        }
        let Some(database) = self.database(storage_key, name) else {
            return true;
        };
        let mut writes = Vec::new();
        if touched_record_writes(database, undo, &HashSet::new(), &mut writes).is_none() {
            return false;
        }
        self.persist(storage_key, name, writes)
    }

    pub(super) fn persist_deletion(&mut self, storage_key: &str, name: &Name) -> bool {
        self.persist(storage_key, name, vec![Write::DeleteDatabase])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // NB: The profile keeps these bytes from one version of Ladybird to the next, so a change to them, or to the wire
    //     encoding under them, must come with a new FORMAT and a way to read the old one.
    #[test]
    fn the_shape_and_index_key_rows_keep_their_format() {
        let shape = PersistedDatabase {
            format: FORMAT,
            version: 5,
            next_id: 3,
            object_stores: vec![PersistedObjectStore {
                id: 1,
                name: Name::from("s"),
                key_path: Some(KeyPath::String(Name::from("id"))),
                key_generator: Some(5),
                indexes: vec![PersistedIndex {
                    id: 2,
                    name: Name::from("i"),
                    key_path: KeyPath::Sequence(vec![Name::from("a")]),
                    unique: true,
                    multi_entry: false,
                }],
            }],
        };
        assert_eq!(
            encode(&shape).unwrap(),
            vec![
                1, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                0, 0, 0, 115, 1, 0, 1, 2, 0, 0, 0, 105, 100, 1, 5, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0,
                0, 0, 1, 1, 0, 0, 0, 105, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 97, 1, 0
            ]
        );

        let index_keys = vec![PersistedIndexKeys {
            index: 2,
            keys: vec![Key::create_number(7.0).as_ref().clone()],
        }];
        assert_eq!(
            encode(&index_keys).unwrap(),
            vec![
                1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 64
            ]
        );
    }
}
