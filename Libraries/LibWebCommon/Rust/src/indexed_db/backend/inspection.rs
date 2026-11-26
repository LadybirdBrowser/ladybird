/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the developer tools read and change of a storage key's databases, outside of any transaction.

use libipc_rust::wire::{Decode, Encode};

use super::store::Undo;
use super::{Backend, ClientId, DatabaseMetadata};
use crate::indexed_db::Name;
use crate::indexed_db::key::Key;
use crate::indexed_db::key_range::KeyRange;

/// A question or a change the developer tools ask of the databases of a client's storage key.
#[derive(Clone, Encode, Decode)]
pub enum Inspection {
    Databases,
    Records { database: Name, store: Name },
    DeleteDatabase { database: Name },
    ClearObjectStore { database: Name, store: Name },
    DeleteRecord { database: Name, store: Name, key: Key },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum InspectionStatus {
    Done = 0,
    NoDatabase = 1,
    NoObjectStore = 2,
    NoRecord = 3,
    Blocked = 4,
    Failed = 5,
}

#[derive(Clone, Encode, Decode)]
pub struct InspectedRecord {
    pub key: Key,
    pub value: Vec<u8>,
}

#[derive(Clone, Encode, Decode)]
pub enum InspectionResult {
    Databases(Vec<DatabaseMetadata>),
    Records(Vec<InspectedRecord>),
    Status(InspectionStatus),
}

impl Backend {
    pub fn inspect(&mut self, client: ClientId, inspection: Inspection) -> InspectionResult {
        let Some(storage_key) = self.clients.get(&client).cloned() else {
            return InspectionResult::Status(InspectionStatus::NoDatabase);
        };
        match inspection {
            Inspection::Databases => {
                // NB: A database shows as its last upgrade that committed left it, so one whose first upgrade hasn't
                //     committed has version 0, and doesn't show yet.
                let databases = self
                    .databases
                    .get(&storage_key)
                    .map(|databases| {
                        databases
                            .iter()
                            .map(|database| database.committed_metadata(&self.transactions))
                            .filter(|metadata| metadata.version > 0)
                            .collect()
                    })
                    .unwrap_or_default();
                InspectionResult::Databases(databases)
            }
            Inspection::Records { database, store } => {
                let records = self
                    .database(&storage_key, &database)
                    .and_then(|database| {
                        database
                            .object_stores
                            .iter()
                            .find(|object_store| object_store.name == store)
                    })
                    .map(|object_store| {
                        object_store
                            .records
                            .iter()
                            .map(|(key, value)| InspectedRecord {
                                key: key.key().as_ref().clone(),
                                value: value.as_ref().clone(),
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                InspectionResult::Records(records)
            }
            Inspection::DeleteDatabase { database } => {
                InspectionResult::Status(self.delete_database_for_inspection(&storage_key, &database))
            }
            Inspection::ClearObjectStore { database, store } => {
                InspectionResult::Status(self.change_records_for_inspection(&storage_key, &database, &store, None))
            }
            Inspection::DeleteRecord { database, store, key } => {
                InspectionResult::Status(self.change_records_for_inspection(&storage_key, &database, &store, Some(key)))
            }
        }
    }

    /// Deletes a database that no connection, and no request in its connection queue, is using.
    fn delete_database_for_inspection(&mut self, storage_key: &str, name: &Name) -> InspectionStatus {
        let unreadable = (storage_key.to_owned(), name.clone());
        if self.unreadable.contains(&unreadable) {
            if !self.persist_deletion(storage_key, name) {
                return InspectionStatus::Failed;
            }
            self.unreadable.remove(&unreadable);
            return InspectionStatus::Done;
        }
        let Some(database) = self.database(storage_key, name) else {
            return InspectionStatus::NoDatabase;
        };
        let queued = self
            .queues
            .get(&(storage_key.to_owned(), name.clone()))
            .is_some_and(|queue| !queue.is_empty());
        if !database.connections.is_empty() || queued {
            return InspectionStatus::Blocked;
        }
        if !self.remove_database(storage_key, name) {
            return InspectionStatus::Failed;
        }
        InspectionStatus::Done
    }

    /// Clears an object store, or deletes one record of it, unless a transaction that could see the change is under
    /// way: Its undo entries would no longer describe the store.
    fn change_records_for_inspection(
        &mut self,
        storage_key: &str,
        database_name: &Name,
        store_name: &Name,
        key: Option<Key>,
    ) -> InspectionStatus {
        let Some(database) = self.database(storage_key, database_name) else {
            return InspectionStatus::NoDatabase;
        };
        if !database
            .object_stores
            .iter()
            .any(|object_store| object_store.name == *store_name)
        {
            return InspectionStatus::NoObjectStore;
        }
        let in_use = database.transactions.iter().any(|id| {
            self.transactions
                .get(id)
                .is_some_and(|transaction| transaction.snapshot.is_some() || transaction.scope.contains(store_name))
        });
        if in_use {
            return InspectionStatus::Blocked;
        }

        let database = self
            .database_mut(storage_key, database_name)
            .expect("the database exists");
        let object_store = database
            .object_stores
            .iter_mut()
            .find(|object_store| object_store.name == *store_name)
            .expect("the object store exists");
        let range = match key {
            Some(key) => {
                let range = KeyRange::only(key);
                if object_store.records_in_range(&range).next().is_none() {
                    return InspectionStatus::NoRecord;
                }
                range
            }
            None => KeyRange::unbounded(),
        };
        let mut undo = Vec::new();
        object_store.delete_records(&range, &mut undo);
        if !self.persist_records(storage_key, database_name, &undo) {
            // NB: The profile still has the records, so they come back.
            let database = self
                .database_mut(storage_key, database_name)
                .expect("the database exists");
            let object_store = database
                .object_stores
                .iter_mut()
                .find(|object_store| object_store.name == *store_name)
                .expect("the object store exists");
            for entry in undo.into_iter().rev() {
                if let Undo::Record {
                    key,
                    previous,
                    previous_index_records,
                    ..
                } = entry
                {
                    object_store.restore_record(key, previous, previous_index_records);
                }
            }
            return InspectionStatus::Failed;
        }
        InspectionStatus::Done
    }
}
