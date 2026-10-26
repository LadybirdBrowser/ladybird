/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The databases of the agent, by their storage key and name, and the connection queues of those,
//! https://w3c.github.io/IndexedDB/#connection-queues. They live as long as the thread of the agent.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, Trace, Vm};
use liburl_rust::origin::Origin;

use crate::indexed_db::internal::database::DatabaseRef;
use crate::indexed_db::internal::request_list::RequestList;

unsafe extern "C" {
    fn gc_root_create(cell: *mut c_void) -> *mut c_void;
}

// https://storage.spec.whatwg.org/#storage-keys
#[derive(Clone)]
pub(crate) struct StorageKey {
    // A storage key is a tuple consisting of an origin (an origin). [HTML]
    // NOTE: This is expected to change; see Client-Side Storage Partitioning https://privacycg.github.io/storage-partitioning/.
    pub(crate) origin: Origin,
}

impl PartialEq for StorageKey {
    fn eq(&self, other: &Self) -> bool {
        // To determine whether a storage key A equals storage key B, run these steps:
        // 1. If A’s origin is not same origin with B’s origin, then return false.
        // 2. Return true.
        self.origin.is_same_origin(&other.origin)
    }
}

// https://w3c.github.io/IndexedDB/#connection-queues
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ConnectionQueue {
    #[gc(untraced)]
    storage_key: StorageKey,
    name: Utf16String,
    pub(crate) request_list: RequestList,
}

static CONNECTION_QUEUE_CLASS: EmbedderClass<ConnectionQueue> = EmbedderClass::new("IndexedDBConnectionQueue");

impl EmbedderValue for ConnectionQueue {
    const CLASS: &'static EmbedderClass<Self> = &CONNECTION_QUEUE_CLASS;
}

pub(crate) type ConnectionQueueRef = Gc<EmbedderCell<ConnectionQueue>>;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct DatabasesOfKey {
    #[gc(untraced)]
    storage_key: StorageKey,
    databases: Vec<(Utf16String, DatabaseRef)>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Storage {
    databases: GcRefCell<Vec<DatabasesOfKey>>,
    connection_queues: GcRefCell<Vec<ConnectionQueueRef>>,
}

static STORAGE_CLASS: EmbedderClass<Storage> = EmbedderClass::new("IndexedDBStorage");

impl EmbedderValue for Storage {
    const CLASS: &'static EmbedderClass<Self> = &STORAGE_CLASS;
}

pub(crate) type StorageRef = Gc<EmbedderCell<Storage>>;

thread_local! {
    // NB: Keeping the root here lets the leak checker see it as reachable.
    static STORAGE: Cell<Option<(StorageRef, NonNull<c_void>)>> = const { Cell::new(None) };
}

/// The storage of the current thread's agent, created on first use.
pub(crate) fn storage(vm: &Vm) -> StorageRef {
    if let Some((storage, _)) = STORAGE.get() {
        return storage;
    }
    let storage = STORAGE_CLASS.allocate(vm, |_| Storage {
        databases: GcRefCell::new(Vec::new()),
        connection_queues: GcRefCell::new(Vec::new()),
    });
    // NB: The storage lives as long as the thread, which roots its cell.
    // SAFETY: The cell is a live cell of the heap.
    let root = NonNull::new(unsafe { gc_root_create(storage.as_ptr().cast()) }).expect("a root is allocated");
    STORAGE.set(Some((storage, root)));
    storage
}

impl Storage {
    pub(crate) fn for_each_database(&self, mut visitor: impl FnMut(DatabaseRef)) {
        for databases_of_key in self.databases.borrow().iter() {
            for (_, database) in &databases_of_key.databases {
                visitor(*database);
            }
        }
    }

    pub(crate) fn for_each_database_of_key(&self, key: &StorageKey, mut visitor: impl FnMut(DatabaseRef)) {
        for databases_of_key in self.databases.borrow().iter() {
            if databases_of_key.storage_key != *key {
                continue;
            }
            for (_, database) in &databases_of_key.databases {
                visitor(*database);
            }
        }
    }

    pub(crate) fn database(&self, key: &StorageKey, name: &Utf16String) -> Option<DatabaseRef> {
        let databases = self.databases.borrow();
        let databases_of_key = databases.iter().find(|entry| entry.storage_key == *key)?;
        databases_of_key
            .databases
            .iter()
            .find(|(database_name, _)| database_name == name)
            .map(|(_, database)| *database)
    }

    pub(crate) fn set_database(&self, vm: &Vm, key: &StorageKey, name: &Utf16String, database: DatabaseRef) {
        let mut databases = self.databases.borrow_mut(vm);
        let databases_of_key = match databases.iter().position(|entry| entry.storage_key == *key) {
            Some(position) => &mut databases[position],
            None => {
                databases.push(DatabasesOfKey {
                    storage_key: key.clone(),
                    databases: Vec::new(),
                });
                databases.last_mut().expect("an entry was just added")
            }
        };
        if let Some(entry) = databases_of_key
            .databases
            .iter_mut()
            .find(|(database_name, _)| database_name == name)
        {
            entry.1 = database;
            return;
        }
        databases_of_key.databases.push((name.clone(), database));
    }

    pub(crate) fn remove_database(&self, vm: &Vm, key: &StorageKey, name: &Utf16String) {
        let mut databases = self.databases.borrow_mut(vm);
        let Some(databases_of_key) = databases.iter_mut().find(|entry| entry.storage_key == *key) else {
            return;
        };
        if let Some(position) = databases_of_key
            .databases
            .iter()
            .position(|(database_name, _)| database_name == name)
        {
            databases_of_key.databases.remove(position);
        }
    }

    // https://w3c.github.io/IndexedDB/#connection-queue
    pub(crate) fn connection_queue_for_key_and_name(
        &self,
        vm: &Vm,
        key: &StorageKey,
        name: &Utf16String,
    ) -> ConnectionQueueRef {
        if let Some(queue) = self
            .connection_queues
            .borrow()
            .iter()
            .find(|queue| queue.storage_key == *key && queue.name == *name)
        {
            return *queue;
        }

        let new_queue = CONNECTION_QUEUE_CLASS.allocate(vm, |defer| ConnectionQueue {
            storage_key: key.clone(),
            name: name.clone(),
            request_list: RequestList::new(defer),
        });
        self.connection_queues.borrow_mut(vm).push(new_queue);
        new_queue
    }
}
