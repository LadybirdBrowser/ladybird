/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of IndexedDB for LibWeb's C++ code: Libraries/LibWeb/IndexedDB/IDBFactory.cpp and Inspection.cpp.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Vm};
use liburl_rust::FfiOrigin;

use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::interface::cell_into_abi;
use crate::bindings::lent_string::LentUtf16String;
use crate::indexed_db::cpp::json_value_of_key;
use crate::indexed_db::idb_factory::IDBFactory;
use crate::indexed_db::idb_key_range::IDBKeyRange;
use crate::indexed_db::internal::algorithms::{
    KeyPath, cleanup_indexed_database_transactions, clear_an_object_store, close_a_database_connection,
    delete_records_from_an_object_store,
};
use crate::indexed_db::internal::database::Database;
use crate::storage_api::storage_key::StorageKey;

/// # Safety
///
/// The origin must be one that LibWeb lends.
unsafe fn storage_key_from_abi(origin: *const FfiOrigin) -> StorageKey {
    // SAFETY: The caller lends a live origin.
    StorageKey {
        origin: unsafe { (*origin).to_origin() },
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_indexed_db_create_factory(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(IDBFactory::create(vm, realm))
}

/// https://w3c.github.io/IndexedDB/#cleanup-indexed-database-transactions
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_indexed_db_cleanup_indexed_database_transactions(
    vm: &Vm,
    event_loop: &LentCppCell<cpp_class::EventLoop>,
) -> bool {
    let event_loop = CppCell::from_lent(event_loop);
    cleanup_indexed_database_transactions(vm, &event_loop)
}

/// Runs close a database connection with every connection of the realm's global object whose close pending flag is
/// unset.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_indexed_db_close_all_connections(vm: &Vm, realm: Gc<Realm>) {
    Database::for_each_database(vm, |database| {
        for connection in database.associated_connections(vm).values().iter() {
            if connection.close_pending() {
                continue;
            }
            if connection.realm() == realm {
                close_a_database_connection(vm, *connection, None, false);
            }
        }
    });
}

/// A key path as the inspection callbacks receive it: none, a string, or a list of strings.
#[repr(C)]
pub struct KeyPathAbi {
    kind: u8,
    strings: *const usize,
    count: usize,
}

fn with_key_path_abi<R>(key_path: Option<&KeyPath>, function: impl FnOnce(&KeyPathAbi) -> R) -> R {
    match key_path {
        None => function(&KeyPathAbi {
            kind: 0,
            strings: core::ptr::null(),
            count: 0,
        }),
        Some(KeyPath::String(string)) => {
            let raw = [string.raw_identity()];
            function(&KeyPathAbi {
                kind: 1,
                strings: raw.as_ptr(),
                count: 1,
            })
        }
        Some(KeyPath::Sequence(strings)) => {
            let raw: Vec<usize> = strings.iter().map(Utf16String::raw_identity).collect();
            function(&KeyPathAbi {
                kind: 2,
                strings: raw.as_ptr(),
                count: raw.len(),
            })
        }
    }
}

/// Calls `on_database` with each database of the storage key: its name, its version, and the names of its object
/// stores, which the call borrows.
///
/// # Safety
///
/// The VM must be live, the origin lent, and `on_database` must not keep what it borrows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_inspect_databases(
    vm: &Vm,
    origin: *const FfiOrigin,
    ctx: *mut c_void,
    on_database: unsafe extern "C" fn(*mut c_void, usize, u64, *const usize, usize),
) {
    // SAFETY: The caller lends the origin.
    let storage_key = unsafe { storage_key_from_abi(origin) };
    for database in Database::for_key(vm, &storage_key).values().iter() {
        let names: Vec<Utf16String> = database
            .object_stores(vm)
            .values()
            .iter()
            .map(|object_store| object_store.name())
            .collect();
        let raw_names: Vec<usize> = names.iter().map(Utf16String::raw_identity).collect();
        let name = database.name();
        // SAFETY: The strings are lent for the call.
        unsafe {
            on_database(
                ctx,
                name.raw_identity(),
                database.version(),
                raw_names.as_ptr(),
                raw_names.len(),
            );
        }
    }
}

/// Calls `on_object_store` with each object store of the database of the storage key with the name, and then
/// `on_index` with each of its indexes. Returns whether the database exists.
///
/// # Safety
///
/// The VM must be live, the origin and the name lent, and the callbacks must not keep what they borrow.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_inspect_object_stores(
    vm: &Vm,
    origin: *const FfiOrigin,
    database_name: LentUtf16String<'_>,
    ctx: *mut c_void,
    on_object_store: unsafe extern "C" fn(*mut c_void, usize, *const KeyPathAbi, bool),
    on_index: unsafe extern "C" fn(*mut c_void, usize, *const KeyPathAbi, bool, bool),
) -> bool {
    // SAFETY: The caller lends the origin.
    let (storage_key, database_name) = unsafe { (storage_key_from_abi(origin), database_name.to_utf16_string()) };
    let Some(database) = Database::for_key_and_name(vm, &storage_key, &database_name) else {
        return false;
    };
    for object_store in database.object_stores(vm).values().iter() {
        let name = object_store.name();
        with_key_path_abi(object_store.key_path(), |key_path| {
            // SAFETY: What the callback receives is lent for the call.
            unsafe { on_object_store(ctx, name.raw_identity(), key_path, object_store.uses_a_key_generator()) };
        });
        for index in object_store.indexes(vm).values().iter() {
            let index_name = index.name();
            with_key_path_abi(Some(index.key_path()), |key_path| {
                // SAFETY: What the callback receives is lent for the call.
                unsafe {
                    on_index(
                        ctx,
                        index_name.raw_identity(),
                        key_path,
                        index.unique(),
                        index.multi_entry(),
                    );
                }
            });
        }
    }
    true
}

/// Calls `on_record` with each record of the object store with the name in the database of the storage key with the
/// name: its key, a JsonValue that the callback takes over, and the bytes of its value, which the call borrows.
///
/// # Safety
///
/// The VM must be live, the origin and the names lent, and the callback must not keep the bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_inspect_records(
    vm: &Vm,
    origin: *const FfiOrigin,
    database_name: LentUtf16String<'_>,
    object_store_name: LentUtf16String<'_>,
    ctx: *mut c_void,
    on_record: unsafe extern "C" fn(*mut c_void, *mut c_void, ak::Span<'_, u8>),
) {
    // SAFETY: The caller lends the origin.
    let (storage_key, database_name, object_store_name) = unsafe {
        (
            storage_key_from_abi(origin),
            database_name.to_utf16_string(),
            object_store_name.to_utf16_string(),
        )
    };
    let Some(database) = Database::for_key_and_name(vm, &storage_key, &database_name) else {
        return;
    };
    let Some(object_store) = database.object_store_with_name(&object_store_name) else {
        return;
    };
    for record in object_store.records().iter() {
        // SAFETY: The callback takes over the key and borrows the bytes for the call.
        unsafe { on_record(ctx, json_value_of_key(&record.key), ak::Span::new(&record.value)) };
    }
}

/// The outcome of an inspection operation.
#[repr(u8)]
pub enum InspectionStatus {
    Done = 0,
    NoDatabase = 1,
    NoObjectStore = 2,
    NoRecord = 3,
    Blocked = 4,
}

/// Deletes the database of the storage key with the name, unless a connection to it is open.
///
/// # Safety
///
/// The VM must be live, and the origin and the name lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_delete_database_for_inspection(
    vm: &Vm,
    origin: *const FfiOrigin,
    database_name: LentUtf16String<'_>,
) -> InspectionStatus {
    // SAFETY: The caller lends the origin.
    let (storage_key, database_name) = unsafe { (storage_key_from_abi(origin), database_name.to_utf16_string()) };
    if let Some(database) = Database::for_key_and_name(vm, &storage_key, &database_name)
        && !database.associated_connections(vm).is_empty()
    {
        return InspectionStatus::Blocked;
    }

    Database::delete_for_key_and_name(vm, &storage_key, &database_name);
    InspectionStatus::Done
}

/// Clears the object store with the name of the database of the storage key with the name.
///
/// # Safety
///
/// The VM must be live, and the origin and the names lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_clear_object_store_for_inspection(
    vm: &Vm,
    origin: *const FfiOrigin,
    database_name: LentUtf16String<'_>,
    object_store_name: LentUtf16String<'_>,
) -> InspectionStatus {
    // SAFETY: The caller lends the origin.
    let (storage_key, database_name, object_store_name) = unsafe {
        (
            storage_key_from_abi(origin),
            database_name.to_utf16_string(),
            object_store_name.to_utf16_string(),
        )
    };
    let Some(database) = Database::for_key_and_name(vm, &storage_key, &database_name) else {
        return InspectionStatus::NoDatabase;
    };
    let Some(object_store) = database.object_store_with_name(&object_store_name) else {
        return InspectionStatus::NoObjectStore;
    };

    clear_an_object_store(vm, object_store);
    InspectionStatus::Done
}

/// Deletes the first record of the object store with the name of the database of the storage key with the name whose
/// key `matches` accepts. `matches` takes over the key it is given.
///
/// # Safety
///
/// The VM must be live, and the origin and the names lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_delete_record_for_inspection(
    vm: &Vm,
    origin: *const FfiOrigin,
    database_name: LentUtf16String<'_>,
    object_store_name: LentUtf16String<'_>,
    ctx: *mut c_void,
    matches: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
) -> InspectionStatus {
    // SAFETY: The caller lends the origin.
    let (storage_key, database_name, object_store_name) = unsafe {
        (
            storage_key_from_abi(origin),
            database_name.to_utf16_string(),
            object_store_name.to_utf16_string(),
        )
    };
    let Some(database) = Database::for_key_and_name(vm, &storage_key, &database_name) else {
        return InspectionStatus::NoDatabase;
    };
    let Some(object_store) = database.object_store_with_name(&object_store_name) else {
        return InspectionStatus::NoObjectStore;
    };

    let key = object_store
        .records()
        .iter()
        .map(|record| record.key.clone())
        // SAFETY: The callback takes over the key.
        .find(|key| unsafe { matches(ctx, json_value_of_key(key)) });
    let Some(key) = key else {
        return InspectionStatus::NoRecord;
    };

    let range = IDBKeyRange::create(vm, Some(key.clone()), Some(key), false, false);
    delete_records_from_an_object_store(vm, object_store, &range);
    InspectionStatus::Done
}
