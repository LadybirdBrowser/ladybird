/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of IndexedDB for LibWeb's C++ code: Libraries/LibWeb/IndexedDB/IDBFactory.cpp and Inspection.cpp.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Realm, Vm};
use libwebcommon_rust::indexed_db::Name;
use libwebcommon_rust::indexed_db::backend::DatabaseMetadata;
use libwebcommon_rust::indexed_db::backend::inspection::{
    InspectedRecord, Inspection, InspectionResult, InspectionStatus,
};

use crate::bindings::CallContext;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::interface::cell_into_abi;
use crate::bindings::interface::with_temporary_execution_context_and_callbacks;
use crate::bindings::lent_string::LentUtf16String;
use crate::html::structured_serialize::structured_serialize_for_storage;
use crate::indexed_db::client;
use crate::indexed_db::cpp::DomException;
use crate::indexed_db::cpp::json_value_of_key;
use crate::indexed_db::idb_factory::IDBFactory;
use crate::indexed_db::idb_record::StorageSerializationRecord;
use crate::indexed_db::idb_transaction::TransactionState;
use crate::indexed_db::internal::algorithms::{
    KeyPath, abort_a_transaction, cleanup_indexed_database_transactions, close_a_database_connection,
    deserialize_a_stored_record,
};
use crate::indexed_db::remote::{self, key_path_from, name_from, string_from};
use crate::indexed_db::remote_ops::inject_a_generated_key;
use std::rc::Rc;

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
/// unset, and lets the realm's client go once what it started is done.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_indexed_db_close_all_connections(vm: &Vm, realm: Gc<Realm>) {
    // AD-HOC: An upgrade under way when its document unloads aborts, rather than committing without the upgradeneeded
    //         listener that nothing will run any more. That includes one whose connection script already closed. One
    //         that's committing already asked the backend to commit, which it may have done.
    let upgrades = MarkedVec::new(vm);
    remote::for_each_transaction(vm, |transaction| {
        if transaction.realm() == realm
            && transaction.is_upgrade_transaction()
            && matches!(
                transaction.state(),
                TransactionState::Active | TransactionState::Inactive
            )
        {
            upgrades.push(transaction);
        }
    });
    for upgrade in upgrades.values().iter().copied() {
        abort_a_transaction(
            vm,
            upgrade,
            Some(DomException::create("AbortError", "The document unloaded")),
        );
    }
    for connection in remote::connections_of_realm(vm, realm).values().iter().copied() {
        if !connection.close_pending() {
            close_a_database_connection(vm, connection, None, false);
        }
    }
    remote::forget_realm_when_idle(vm, realm);
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

/// What the backend answers about the databases of a realm's storage key, through the realm's client.
fn inspect(vm: &Vm, realm: Gc<Realm>, inspection: Inspection) -> Option<InspectionResult> {
    let client_id = remote::client_for(vm, realm)?;
    client::inspect(client_id, &inspection)
}

fn databases(vm: &Vm, realm: Gc<Realm>) -> Vec<DatabaseMetadata> {
    match inspect(vm, realm, Inspection::Databases) {
        Some(InspectionResult::Databases(databases)) => databases,
        _ => Vec::new(),
    }
}

fn status_of(result: Option<InspectionResult>) -> InspectionStatus {
    match result {
        Some(InspectionResult::Status(status)) => status,
        _ => InspectionStatus::NoDatabase,
    }
}

/// Calls `on_database` with each database of the realm's storage key: its name, its version, and the names of its
/// object stores, which the call borrows.
///
/// # Safety
///
/// The VM and the realm must be live, and `on_database` must not keep what it borrows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_inspect_databases(
    vm: &Vm,
    realm: Gc<Realm>,
    ctx: *mut c_void,
    on_database: unsafe extern "C" fn(*mut c_void, usize, u64, *const usize, usize),
) {
    for database in databases(vm, realm) {
        let names: Vec<Utf16String> = database
            .object_stores
            .iter()
            .map(|store| string_from(&store.name))
            .collect();
        let raw_names: Vec<usize> = names.iter().map(Utf16String::raw_identity).collect();
        let name = string_from(&database.name);
        // SAFETY: The strings are lent for the call.
        unsafe {
            on_database(
                ctx,
                name.raw_identity(),
                database.version,
                raw_names.as_ptr(),
                raw_names.len(),
            );
        }
    }
}

/// Calls `on_object_store` with each object store of the database with the name of the realm's storage key, and then
/// `on_index` with each of its indexes. Returns whether the database exists.
///
/// # Safety
///
/// The VM and the realm must be live, the name lent, and the callbacks must not keep what they borrow.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_inspect_object_stores(
    vm: &Vm,
    realm: Gc<Realm>,
    database_name: LentUtf16String<'_>,
    ctx: *mut c_void,
    on_object_store: unsafe extern "C" fn(*mut c_void, usize, *const KeyPathAbi, bool),
    on_index: unsafe extern "C" fn(*mut c_void, usize, *const KeyPathAbi, bool, bool),
) -> bool {
    let database_name = name_from(&database_name.to_utf16_string());
    let Some(database) = databases(vm, realm)
        .into_iter()
        .find(|database| database.name == database_name)
    else {
        return false;
    };
    for object_store in &database.object_stores {
        let name = string_from(&object_store.name);
        let key_path = object_store.key_path.as_ref().map(key_path_from);
        with_key_path_abi(key_path.as_ref(), |key_path| {
            // SAFETY: What the callback receives is lent for the call.
            unsafe { on_object_store(ctx, name.raw_identity(), key_path, object_store.auto_increment) };
        });
        for index in &object_store.indexes {
            let index_name = string_from(&index.name);
            with_key_path_abi(Some(&key_path_from(&index.key_path)), |key_path| {
                // SAFETY: What the callback receives is lent for the call.
                unsafe {
                    on_index(
                        ctx,
                        index_name.raw_identity(),
                        key_path,
                        index.unique,
                        index.multi_entry,
                    );
                };
            });
        }
    }
    true
}

/// The records of the object store with the name in the database with the name of the realm's storage key.
fn records(vm: &Vm, realm: Gc<Realm>, database: Name, store: Name) -> Vec<InspectedRecord> {
    match inspect(vm, realm, Inspection::Records { database, store }) {
        Some(InspectionResult::Records(records)) => records,
        _ => Vec::new(),
    }
}

/// Calls `on_record` with each record of the object store with the name in the database with the name of the realm's
/// storage key: its key, a JsonValue that the callback takes over, and the bytes of its value, which the call borrows.
///
/// # Safety
///
/// The VM and the realm must be live, the names lent, and the callback must not keep the bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_inspect_records(
    vm: &Vm,
    realm: Gc<Realm>,
    database_name: LentUtf16String<'_>,
    object_store_name: LentUtf16String<'_>,
    ctx: *mut c_void,
    on_record: unsafe extern "C" fn(*mut c_void, *mut c_void, ak::Span<'_, u8>),
) {
    let (database, store) = (
        name_from(&database_name.to_utf16_string()),
        name_from(&object_store_name.to_utf16_string()),
    );
    // NB: The backend keeps a record's value as the agent stored it, without the key it generated for it, which the
    //     value shows here as it does to script.
    let generated_key_path = databases(vm, realm)
        .into_iter()
        .find(|metadata| metadata.name == database)
        .and_then(|metadata| {
            metadata
                .object_stores
                .into_iter()
                .find(|metadata| metadata.name == store)
        })
        .filter(|metadata| metadata.auto_increment)
        .and_then(|metadata| metadata.key_path.as_ref().map(key_path_from));
    for record in records(vm, realm, database, store) {
        let value = match &generated_key_path {
            Some(key_path) => with_temporary_execution_context_and_callbacks(realm, || {
                let cx = CallContext::new(vm, realm);
                let serialized: StorageSerializationRecord = Rc::from(record.value.as_slice());
                deserialize_a_stored_record(&cx, &serialized).ok().and_then(|value| {
                    inject_a_generated_key(&cx, value, key_path, &record.key);
                    structured_serialize_for_storage(&cx, value).ok()
                })
            })
            .unwrap_or_else(|| record.value.clone()),
            None => record.value.clone(),
        };
        // SAFETY: The callback takes over the key and borrows the bytes for the call.
        unsafe { on_record(ctx, json_value_of_key(&record.key), ak::Span::new(&value)) };
    }
}

/// Deletes the database of the realm's storage key with the name, unless a connection to it is open.
///
/// # Safety
///
/// The VM and the realm must be live, and the name lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_delete_database_for_inspection(
    vm: &Vm,
    realm: Gc<Realm>,
    database_name: LentUtf16String<'_>,
) -> InspectionStatus {
    let database = name_from(&database_name.to_utf16_string());
    status_of(inspect(vm, realm, Inspection::DeleteDatabase { database }))
}

/// Clears the object store with the name of the database of the realm's storage key with the name.
///
/// # Safety
///
/// The VM and the realm must be live, and the names lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_clear_object_store_for_inspection(
    vm: &Vm,
    realm: Gc<Realm>,
    database_name: LentUtf16String<'_>,
    object_store_name: LentUtf16String<'_>,
) -> InspectionStatus {
    let (database, store) = (
        name_from(&database_name.to_utf16_string()),
        name_from(&object_store_name.to_utf16_string()),
    );
    status_of(inspect(vm, realm, Inspection::ClearObjectStore { database, store }))
}

/// Deletes the first record of the object store with the name of the database of the realm's storage key with the
/// name whose key `matches` accepts. `matches` takes over the key it is given.
///
/// # Safety
///
/// The VM and the realm must be live, and the names lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_delete_record_for_inspection(
    vm: &Vm,
    realm: Gc<Realm>,
    database_name: LentUtf16String<'_>,
    object_store_name: LentUtf16String<'_>,
    ctx: *mut c_void,
    matches: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
) -> InspectionStatus {
    let (database, store) = (
        name_from(&database_name.to_utf16_string()),
        name_from(&object_store_name.to_utf16_string()),
    );
    let Some(metadata) = databases(vm, realm)
        .into_iter()
        .find(|metadata| metadata.name == database)
    else {
        return InspectionStatus::NoDatabase;
    };
    if !metadata
        .object_stores
        .iter()
        .any(|object_store| object_store.name == store)
    {
        return InspectionStatus::NoObjectStore;
    }
    let key = records(vm, realm, database.clone(), store.clone())
        .into_iter()
        .map(|record| record.key)
        // SAFETY: The callback takes over the key.
        .find(|key| unsafe { matches(ctx, json_value_of_key(key)) });
    let Some(key) = key else {
        return InspectionStatus::NoRecord;
    };
    status_of(inspect(vm, realm, Inspection::DeleteRecord { database, store, key }))
}
