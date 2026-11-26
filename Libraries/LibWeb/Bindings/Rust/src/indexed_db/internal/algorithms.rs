/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The algorithms of the Indexed Database API, https://w3c.github.io/IndexedDB/#algorithms

use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::{
    Array, ArrayBuffer, ArrayBufferShared, CanonicalIndexMode, Date, Gc, MarkedVec, Must, Object, PrimitiveString,
    PropertyKey, Realm, Trace, TypedArrayBase, Value, Vm, canonical_numeric_index_string, is_host_instance_of,
    length_of_array_like,
};
use libunicode_rust::character_types::{
    code_point_has_identifier_continue_property, code_point_has_identifier_start_property,
};

use crate::bindings::conversions::{ToJs, buffer_source_type_of};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEvent, CppEventTarget};
use crate::bindings::interface::{
    Interface, Ref, implementation_if, implementation_of, with_temporary_execution_context_and_callbacks,
};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::dom::event::EventInit;
use crate::file_api::blob::{AnyBlob, Blob};
use crate::file_api::file::File;
use crate::html::dom_string_list::DOMStringList;
use crate::html::structured_serialize::{structured_deserialize_for_storage, structured_serialize_for_storage};
use crate::indexed_db::cpp::DomException;
use crate::indexed_db::idb_cursor::IDBCursorDirection;
use crate::indexed_db::idb_database::{ConnectionState, IDBDatabase, IDBTransactionDurability};
use crate::indexed_db::idb_key_range::{IDBKeyRange, key_range_from_value};
use crate::indexed_db::idb_object_store::{IDBGetAllOptions, IDBObjectStore};
use crate::indexed_db::idb_record::StorageSerializationRecord;
use crate::indexed_db::idb_request::{AnyIDBRequest, IDBRequest, IDBRequestSource};
use crate::indexed_db::idb_transaction::{IDBTransaction, IDBTransactionMode, TransactionState};
use crate::indexed_db::idb_version_change_event::{IDBVersionChangeEvent, IDBVersionChangeEventInit};
use crate::indexed_db::internal::function::{
    Conversion, Function, FunctionWith, NoCells, convert_result, create_function, create_function_with,
};
use crate::indexed_db::internal::index::IndexRef;
use crate::indexed_db::internal::key::{Key, KeyType, code_unit_order};
use crate::indexed_db::internal::object_store::ObjectStoreRef;
use crate::indexed_db::remote;
use crate::indexed_db::remote_ops;
use crate::storage_api::storage_key::StorageKey;
use libwebcommon_rust::indexed_db::backend::{Error as BackendError, Operation as BackendOperation, OperationResult};

/// A key path, https://w3c.github.io/IndexedDB/#key-path-construct: a string or a list of strings.
#[derive(Clone, PartialEq)]
pub(crate) enum KeyPath {
    String(Utf16String),
    Sequence(Vec<Utf16String>),
}

fn strictly_split_key_path(key_path: &Utf16String) -> Vec<Utf16String> {
    key_path
        .to_utf16()
        .split(|code_unit| *code_unit == u16::from(b'.'))
        .map(Utf16String::from_utf16)
        .collect()
}

pub(crate) type OpenResult = Result<Ref<IDBDatabase>, DomException>;

// https://w3c.github.io/IndexedDB/#open-a-database-connection
// NB: The backend runs the steps that involve the database and its other connections, wherever those live, and its
//     events drive the rest: see remote.rs.
pub(crate) fn open_a_database_connection(
    vm: &Vm,
    realm: Gc<Realm>,
    _storage_key: StorageKey,
    name: Utf16String,
    maybe_version: Option<u64>,
    request: AnyIDBRequest,
    on_complete: FunctionWith<OpenResult>,
) {
    let Some(client_id) = remote::client_for(vm, realm) else {
        on_complete.call(
            vm,
            Err(DomException::create("UnknownError", "The databases can't be reached")),
        );
        return;
    };
    remote::open_a_database_connection(vm, realm, client_id, &name, maybe_version, request, on_complete);
}

/// What runs when the backend asks an open request for an upgrade: https://w3c.github.io/IndexedDB/#open-a-database-connection
/// steps 10.6 to 11.
pub(crate) fn run_upgrade_for_backend(
    vm: &Vm,
    realm: Gc<Realm>,
    connection: Ref<IDBDatabase>,
    version: u64,
    transaction_id: u64,
    request: AnyIDBRequest,
    on_complete: FunctionWith<OpenResult>,
) {
    // 6. Run upgrade a database using connection, version and request.
    upgrade_a_database(
        vm,
        realm,
        connection,
        version,
        transaction_id,
        request,
        create_function(
            vm,
            (connection, request, on_complete),
            |vm, &(connection, request, on_complete), ()| {
                // 7. If connection was closed, return a newly created "AbortError" DOMException and abort these steps.
                if connection.state() == ConnectionState::Closed {
                    on_complete.call(vm, Err(DomException::create("AbortError", "Connection was closed")));
                    return;
                }

                // 8. If request's error is set, run the steps to close a database connection with connection, return a
                //    newly created "AbortError" DOMException and abort these steps.
                if request.has_error() {
                    close_a_database_connection(
                        vm,
                        connection,
                        Some(create_function(vm, on_complete, |vm, &on_complete, ()| {
                            on_complete.call(
                                vm,
                                Err(DomException::create("AbortError", "Upgrade transaction was aborted")),
                            );
                        })),
                        false,
                    );
                    return;
                }

                // 11. Return connection.
                on_complete.call(vm, Ok(connection));
            },
        ),
    );
}

// https://w3c.github.io/IndexedDB/#fire-a-version-change-event
pub(crate) fn fire_a_version_change_event(
    vm: &Vm,
    realm: Gc<Realm>,
    event_name: &str,
    target: &CppEventTarget,
    old_version: u64,
    new_version: Option<u64>,
) -> bool {
    let event_init = IDBVersionChangeEventInit {
        // 3. Set event’s bubbles and cancelable attributes to false.
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        // 4. Set event’s oldVersion attribute to oldVersion.
        old_version,
        // 5. Set event’s newVersion attribute to newVersion.
        new_version,
    };

    // 1. Let event be the result of creating an event using IDBVersionChangeEvent.
    // 2. Set event’s type attribute to e.
    let event = IDBVersionChangeEvent::create(vm, realm, &Utf16String::from_utf8(event_name), &event_init);

    // 6. Let legacyOutputDidListenersThrowFlag be false.
    // 7. Dispatch event at target with legacyOutputDidListenersThrowFlag.
    // 8. Return legacyOutputDidListenersThrowFlag.
    target.dispatch_event_with_legacy_output_did_listeners_throw_flag(event.event())
}

// https://w3c.github.io/IndexedDB/#convert-value-to-key
pub(crate) fn convert_a_value_to_a_key(
    cx: &CallContext,
    input: Value,
    seen_input: Option<&MarkedVec<'_, Value>>,
) -> WebIdlResult<Rc<Key>> {
    let vm = cx.vm();

    // 1. If seen was not given, then let seen be a new empty set.
    let seen = MarkedVec::new(vm);
    if let Some(seen_input) = seen_input {
        seen.extend_from_slice(&seen_input.values());
    }

    // 2. If seen contains input, then return "invalid value".
    if seen.values().iter().any(|value| value.to_raw() == input.to_raw()) {
        return Ok(Key::create_invalid("Already seen key"));
    }

    // AD-HOC: The backend refuses a key whose arrays nest more deeply than this, as it reads keys from other processes
    //         without trusting them, so such a key is invalid here rather than taking the process down there.
    if seen.len() > libwebcommon_rust::indexed_db::key::MAX_DECODED_KEY_DEPTH {
        return Ok(Key::create_invalid("Key nested too deeply"));
    }

    // 3. Jump to the appropriate step below:

    // - If Type(input) is Number
    if input.is_number() {
        // 1. If input is NaN then return "invalid value".
        if input.is_nan() {
            return Ok(Key::create_invalid("NaN key"));
        }

        // 2. Otherwise, return a new key with type number and value input.
        return Ok(Key::create_number(input.as_f64()));
    }

    // - If input is a Date (has a [[DateValue]] internal slot)
    if input.is_object()
        && let Some(date) = input.as_object().downcast::<Date>()
    {
        // 1. Let ms be the value of input’s [[DateValue]] internal slot.
        let ms = date.date_value();

        // 2. If ms is NaN then return "invalid value".
        if ms.is_nan() {
            return Ok(Key::create_invalid("NaN key"));
        }

        // 3. Otherwise, return a new key with type date and value ms.
        return Ok(Key::create_date(ms));
    }

    // - If Type(input) is String
    if input.is_string() {
        // 1. Return a new key with type string and value input.
        return Ok(Key::create_string(input.as_string().utf16_string()));
    }

    // - If input is a buffer source type
    if let Some(buffer_source) = buffer_source_type_of(input) {
        // 1. If input is detached then return "invalid value".
        if buffer_source.extent().buffer.is_detached() {
            return Ok(Key::create_invalid("Detached buffer is not supported as key"));
        }

        // 2. Let bytes be the result of getting a copy of the bytes held by the buffer source input.
        // SAFETY: Copying the bytes neither runs script nor allocates.
        let data_buffer = unsafe { buffer_source.with_bytes(<[u8]>::to_vec) };

        // 3. Return a new key with type binary and value bytes.
        return Ok(Key::create_binary(data_buffer));
    }

    // - If input is an Array exotic object
    if input.is_object()
        && let Some(array) = input.as_object().downcast::<Array>()
    {
        let array: Gc<Object> = array.upcast();

        // 1. Let len be ? ToLength( ? Get(input, "length")).
        let length = length_of_array_like(vm, &array)?;

        // 2. Append input to seen.
        seen.push(input);

        // 3. Let keys be a new empty list.
        let mut keys = Vec::new();

        // 4. Let index be 0.
        let mut index: u64 = 0;

        // 5. While index is less than len:
        while index < length {
            let property_key = PropertyKey::from_number(index);

            // 1. Let hop be ? HasOwnProperty(input, index).
            let hop = array.has_own_property(vm, &property_key)?;

            // 2. If hop is false, return "invalid value".
            if !hop {
                return Ok(Key::create_invalid("Array-like object has no property"));
            }

            // 3. Let entry be ? Get(input, index).
            let entry = array.get(vm, &property_key)?;

            // 4. Let key be the result of converting a value to a key with arguments entry and seen.
            // 5. ReturnIfAbrupt(key).
            let key = convert_a_value_to_a_key(cx, entry, Some(&seen))?;

            // 6. If key is "invalid value" or "invalid type" abort these steps and return "invalid value".
            if key.is_invalid() {
                return Ok(key);
            }

            // 7. Append key to keys.
            keys.push(key);

            // 8. Increase index by 1.
            index += 1;
        }

        // 6. Return a new array key with value keys.
        return Ok(Key::create_array(keys));
    }

    // - Otherwise
    // Return "invalid type".
    Ok(Key::create_invalid(
        "Unable to convert value to key. Its not of a known type",
    ))
}

// https://w3c.github.io/IndexedDB/#close-a-database-connection
pub(crate) fn close_a_database_connection(
    vm: &Vm,
    connection: Ref<IDBDatabase>,
    on_complete: Option<Function>,
    forced: bool,
) {
    // 1. Set connection’s close pending flag to true.
    // NB: The backend waits for the connection's transactions as well, and only closes it then.
    if !connection.close_pending() {
        remote::close(vm, connection);
    }
    connection.set_close_pending(true);

    // 2. If the forced flag is true, then for each transaction created using connection run abort a transaction with transaction and newly created "AbortError" DOMException.
    if forced {
        for transaction in connection.transactions(vm).values().iter() {
            // NB: A transaction that the backend already committed finishes as committed: Its writes are saved.
            if transaction.commit_confirmed() {
                continue;
            }
            abort_a_transaction(
                vm,
                *transaction,
                Some(DomException::create("AbortError", "Connection was closed")),
            );
        }
    }

    // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is closed.
    let transactions = connection.transactions(vm);
    connection.wait_for_transactions_to_finish(
        vm,
        &transactions.values(),
        create_function(
            vm,
            (connection, forced, on_complete),
            |vm, &(connection, forced, on_complete), ()| {
                connection.set_state(ConnectionState::Closed);

                // 4. If the forced flag is true, then fire an event named close at connection.
                if forced {
                    connection.event_target().dispatch_event(&CppEvent::create(
                        connection.realm(),
                        "close",
                        false,
                        false,
                        false,
                    ));
                }

                if let Some(on_complete) = on_complete {
                    queue_a_database_task(vm, on_complete);
                }
            },
        ),
    );
}

// https://w3c.github.io/IndexedDB/#upgrade-a-database
pub(crate) fn upgrade_a_database(
    vm: &Vm,
    realm: Gc<Realm>,
    connection: Ref<IDBDatabase>,
    version: u64,
    transaction_id: u64,
    request: AnyIDBRequest,
    on_complete: Function,
) {
    // 1. Let db be connection’s database.
    let db = connection.associated_database();

    // 2. Let transaction be a new upgrade transaction with connection used as connection.
    // 3. Set transaction’s scope to connection’s object store set.
    let object_store_set = connection.object_store_set(vm);
    let transaction = IDBTransaction::create(
        vm,
        connection,
        IDBTransactionMode::Versionchange,
        IDBTransactionDurability::Default,
        &object_store_set.values(),
    );
    IDBTransaction::set_remote_id(vm, transaction, transaction_id);

    // 4. Set db’s upgrade transaction to transaction.
    db.set_upgrade_transaction(Some(transaction));

    // 5. Set transaction’s state to inactive.
    transaction.set_state(TransactionState::Inactive);

    // FIXME: 6. Start transaction.

    // 7. Let old version be db’s version.
    let old_version = db.version();

    // AD-HOC: Set up per-store mutation logs. This also records the current database version
    //         so it can be restored if the transaction is aborted.
    transaction.set_up_mutation_logs(vm);

    // 8. Set db’s version to version. This change is considered part of the transaction, and so if the transaction is aborted, this change is reverted.
    db.set_version(version);

    // 9. Set request’s processed flag to true.
    request.set_processed(true);

    // 10. Queue a database task to run these steps:
    queue_a_database_task(
        vm,
        create_function(
            vm,
            (
                realm,
                request,
                connection,
                transaction,
                old_version,
                version,
                on_complete,
            ),
            |vm, &(realm, request, connection, transaction, old_version, version, on_complete), ()| {
                // NB: An upgrade that aborted before this task ran, as its document unloaded or the backend closed its
                //     connection, fires no upgradeneeded event: Its open fails instead. The request and the transaction
                //     are still bound, as the abort's own task, queued after this one, expects.
                if transaction.is_finished() {
                    request.set_transaction(Some(transaction));
                    transaction.set_associated_request(Some(request));
                    request.set_done(true);
                    request.set_error(
                        vm,
                        Some(DomException::create("AbortError", "Upgrade transaction was aborted")),
                    );
                    connection.wait_for_transactions_to_finish(vm, &[transaction], on_complete);
                    return;
                }

                // 1. Set request’s result to connection.
                set_idb_request_result(vm, realm, request, connection);

                // 2. Set request’s transaction to transaction.
                // NOTE: We need to do a two-way binding here.
                request.set_transaction(Some(transaction));
                transaction.set_associated_request(Some(request));

                // 3. Set request’s done flag to true.
                request.set_done(true);

                // 4. Set transaction’s state to active.
                transaction.set_state(TransactionState::Active);

                // 5. Let didThrow be the result of firing a version change event named upgradeneeded at request with old version and version.
                let did_throw = fire_a_version_change_event(
                    vm,
                    realm,
                    "upgradeneeded",
                    request.event_target(),
                    old_version,
                    Some(version),
                );

                // 6. If transaction’s state is active, then:
                if transaction.state() == TransactionState::Active {
                    // 1. Set transaction’s state to inactive.
                    transaction.set_state(TransactionState::Inactive);

                    // 2. If didThrow is true, run abort a transaction with transaction and a newly created "AbortError" DOMException.
                    if did_throw {
                        abort_a_transaction(
                            vm,
                            transaction,
                            Some(DomException::create(
                                "AbortError",
                                "Version change event threw an exception",
                            )),
                        );
                    }

                    // https://w3c.github.io/IndexedDB/#transaction-commit
                    // The implementation must attempt to commit an inactive transaction when all requests placed
                    // against the transaction have completed and their returned results handled, no new requests have
                    // been placed against the transaction, and the transaction has not been aborted
                    if transaction.state() == TransactionState::Inactive
                        && transaction.request_list().is_empty()
                        && !transaction.aborted()
                    {
                        commit_a_transaction(vm, transaction);
                    }
                }

                // 11. Wait for transaction to finish.
                connection.wait_for_transactions_to_finish(vm, &[transaction], on_complete);
            },
        ),
    );
}

pub(crate) type DeleteResult = Result<u64, DomException>;

// https://w3c.github.io/IndexedDB/#deleting-a-database
pub(crate) fn delete_a_database(
    vm: &Vm,
    realm: Gc<Realm>,
    _storage_key: StorageKey,
    name: Utf16String,
    request: AnyIDBRequest,
    on_complete: FunctionWith<DeleteResult>,
) {
    // NB: The backend runs the steps, and waits for the database's connections wherever those live.
    let Some(client_id) = remote::client_for(vm, realm) else {
        on_complete.call(
            vm,
            Err(DomException::create("UnknownError", "The databases can't be reached")),
        );
        return;
    };
    remote::delete_a_database(vm, realm, client_id, &name, request, on_complete);
}

// https://w3c.github.io/IndexedDB/#abort-an-upgrade-transaction
fn abort_an_upgrade_transaction(vm: &Vm, transaction: Ref<IDBTransaction>) {
    // 1. Let connection be transaction’s connection.
    let connection = transaction.connection_object();

    // 2. Let database be connection’s database.
    let database = connection.associated_database();

    // 3. Set connection’s version to database’s version if database previously existed, or 0 (zero) if database was
    //    newly created.
    connection.set_version(database.version());

    // 4. Set connection’s object store set to the set of object stores in database if database previously existed, or
    //    the empty set if database was newly created.
    // NB: MutationLog reverts all changes to the connection's object store set alongside the deletion/creation of the
    //     underlying ObjectStore instances.
    //     However, the transaction scope will still be outdated here. Upgrade transactions are always scoped to the
    //     entire database, so this is easy.
    let object_store_set = connection.object_store_set(vm);
    transaction.set_scope(vm, &object_store_set.values());

    // 5. For each object store handle handle associated with transaction, including those for object stores that were
    //    created or deleted during transaction:
    let database_object_stores = database.object_stores(vm);
    for handle in transaction.object_store_handles(vm).values().iter() {
        // 1. If handle’s object store was not newly created during transaction, set handle’s name to its object
        //    store’s name.
        if database_object_stores.values().contains(&handle.store()) {
            handle.update_name();
        }

        // 2. Set handle’s index set to the set of indexes that reference its object store.
        handle.update_index_set(vm);
    }

    // 6. For each index handle handle associated with transaction, including those for indexes that were created or
    //    deleted during transaction:
    for handle in transaction.index_handles(vm).values().iter() {
        // 1. If handle’s index was not newly created during transaction, set handle’s name to its index’s name.
        if handle
            .index()
            .object_store()
            .index_set_get(&handle.index().name())
            .is_some()
        {
            handle.update_name();
        }
    }
}

// https://w3c.github.io/IndexedDB/#abort-a-transaction
pub(crate) fn abort_a_transaction(vm: &Vm, transaction: Ref<IDBTransaction>, error: Option<DomException>) {
    // NOTE: This is not spec'ed anywhere, but we need to know IF the transaction was aborted.
    transaction.set_aborted(true);

    // 1. If transaction’s state is finished, abort these steps.
    if transaction.is_finished() {
        return;
    }

    // 2. All the changes made to the database by the transaction are reverted.
    //    For upgrade transactions this includes changes to the set of object stores and indexes, as well as the change to the version.
    //    Any object stores and indexes which were created during the transaction are now considered deleted for the purposes of other algorithms.
    // NB: The backend reverts the changes to the database, and the agent those to its view of the database's shape.
    remote::abort(vm, transaction);
    transaction.revert_all_mutations(vm);
    transaction.discard_mutation_logs(vm);

    // 3. If transaction is an upgrade transaction, run the steps to abort an upgrade transaction with transaction.
    if transaction.is_upgrade_transaction() {
        abort_an_upgrade_transaction(vm, transaction);
    }

    // 4. Set transaction’s state to finished.
    IDBTransaction::set_finished(vm, transaction);

    // AD-HOC: Clear the cleanup event loop so that the microtask checkpoint doesn't set the state to inactive on a
    //         finished transaction.
    //         https://github.com/w3c/IndexedDB/issues/489
    transaction.set_cleanup_event_loop(vm, None);

    // 5. Set transaction’s error to error.
    transaction.set_error(vm, error);

    // 6. For each request of transaction’s request list, abort the steps to asynchronously execute a request for request,
    //    set request’s processed flag to true, and queue a database task to run these steps:
    for request in transaction.request_list().requests(vm).values().iter() {
        let request = *request;
        request.set_aborted(true);
        request.set_processed(true);
        queue_a_database_task(
            vm,
            create_function(vm, request, |vm, &request, ()| {
                // 1. Set request’s done flag to true.
                request.set_done(true);

                // 2. Set request’s result to undefined.
                request.set_result(Value::UNDEFINED);

                // 3. Set request’s error to a newly created "AbortError" DOMException.
                request.set_error(vm, Some(DomException::create("AbortError", "Transaction was aborted")));

                // 4. Fire an event named error at request with its bubbles and cancelable attributes initialized to true.
                request
                    .event_target()
                    .dispatch_event(&CppEvent::create(request.realm(), "error", true, true, false));
            }),
        );
    }

    // NB: Clear the commit wait callback, since the transaction is no longer committing.
    transaction.request_list().clear_on_all_processed();

    // 7. Queue a database task to run these steps:
    queue_a_database_task(
        vm,
        create_function(vm, transaction, |vm, &transaction, ()| {
            // 1. If transaction is an upgrade transaction, then set transaction’s connection's associated database's upgrade transaction to null.
            if transaction.is_upgrade_transaction() {
                transaction
                    .connection_object()
                    .associated_database()
                    .set_upgrade_transaction(None);
            }

            // 2. Fire an event named abort at transaction with its bubbles attribute initialized to true.
            transaction.event_target().dispatch_event(&CppEvent::create(
                transaction.realm(),
                "abort",
                true,
                false,
                false,
            ));

            // 3. If transaction is an upgrade transaction, then:
            if transaction.is_upgrade_transaction() {
                // 1. Let request be the open request associated with transaction.
                let request = transaction
                    .associated_request()
                    .expect("an upgrade transaction has an open request");

                // 2. Set request’s transaction to null.
                // NOTE: Clear the two-way binding.
                request.set_transaction(None);
                transaction.set_associated_request(None);

                // 3. Set request’s result to undefined.
                request.set_result(Value::UNDEFINED);

                // FIXME: https://github.com/w3c/IndexedDB/issues/473
                // x. Set transaction's associated request's error to a newly created "AbortError" DOMException.
                request.set_error(
                    vm,
                    Some(DomException::create("AbortError", "Upgrade transaction was aborted")),
                );

                // 4. Set request’s processed flag to false.
                // FIXME: request->set_processed(false);

                // 5. Set request’s done flag to false.
                request.set_done(false);
            }

            // AD-HOC: Now that all abort tasks are queued, notify pending transaction waits.
            //         This is done after step 7 so that the abort cleanup tasks above are queued
            //         before any waiting transactions (e.g. upgrade completion callbacks) proceed.
            transaction.connection_object().check_pending_transaction_waits(vm);
        }),
    );
}

// https://w3c.github.io/IndexedDB/#convert-a-key-to-a-value
pub(crate) fn convert_a_key_to_a_value(cx: &CallContext, key: &Key) -> Value {
    let vm = cx.vm();
    let realm = cx.realm();

    // 1. Let type be key’s type.
    let r#type = key.r#type();

    // 2. Let value be key’s value.
    // NB: The steps read the value for its type below.

    // 3. Switch on type:
    match r#type {
        KeyType::Number => {
            // Return an ECMAScript Number value equal to value
            Value::from_f64(key.value_as_double())
        }

        KeyType::String => {
            // Return an ECMAScript String value equal to value
            Value::from_string(PrimitiveString::create(vm, key.value_as_string().clone()))
        }

        KeyType::Date => {
            // 1. Let date be the result of executing the ECMAScript Date constructor with the single argument value.
            let date = Date::create(vm, realm, key.value_as_double());

            // 2. Assert: date is not an abrupt completion.
            // NOTE: This is not possible in our implementation.

            // 3. Return date.
            Value::from_object(date)
        }

        KeyType::Binary => {
            let buffer = key.value_as_byte_buffer();

            // 1. Let len be value’s length.
            let len = buffer.len();

            // 2. Let buffer be the result of executing the ECMAScript ArrayBuffer constructor with len.
            // 3. Assert: buffer is not an abrupt completion.
            // 4. Set the entries in buffer’s [[ArrayBufferData]] internal slot to the entries in value.
            let array_buffer = ArrayBuffer::try_create_written_by(vm, realm, len, ArrayBufferShared::No, |output| {
                for (byte, value) in output.iter_mut().zip(buffer) {
                    byte.write(*value);
                }
                len
            })
            .must();

            // 5. Return buffer.
            Value::from_object(array_buffer)
        }

        KeyType::Array => {
            let data = key.value_as_vector();

            // 1. Let array be the result of executing the ECMAScript Array constructor with no arguments.
            // 2. Assert: array is not an abrupt completion.
            let array = Array::create(vm, realm, 0, None).must();

            // 3. Let len be value’s size.
            let len = data.len();

            // 4. Let index be 0.
            let mut index: usize = 0;

            // 5. While index is less than len:
            while index < len {
                // 1. Let entry be the result of converting a key to a value with value[index].
                let entry = convert_a_key_to_a_value(cx, &data[index]);

                // 2. Let status be CreateDataProperty(array, index, entry).
                let status = array
                    .create_data_property(vm, &PropertyKey::from_number(index as u64), entry, None, None)
                    .must();

                // 3. Assert: status is true.
                assert!(status);

                // 4. Increase index by 1.
                index += 1;
            }

            // 6. Return array.
            Value::from_object(array)
        }
        KeyType::Invalid => unreachable!("an invalid key is not converted to a value"),
    }
}

// https://tc39.es/ecma262/#prod-IdentifierName
fn is_identifier_name(string: &[u16]) -> bool {
    // FIXME: Accept the UnicodeEscapeSequences that IdentifierStart and IdentifierPart allow.
    if string.is_empty() {
        return false;
    }

    let mut is_first_code_point = true;
    for code_point in char::decode_utf16(string.iter().copied()) {
        let code_point = code_point.map_or(0xFFFD, u32::from);
        // IdentifierStartChar ::
        //     UnicodeIDStart
        //     $
        //     _
        // IdentifierPartChar ::
        //     UnicodeIDContinue
        //     $
        //     <ZWNJ>
        //     <ZWJ>
        let mut is_valid = code_point == u32::from('$') || code_point == u32::from('_');
        if is_first_code_point {
            is_valid = is_valid || code_point_has_identifier_start_property(code_point);
        } else {
            is_valid = is_valid
                || code_point == 0x200C
                || code_point == 0x200D
                || code_point_has_identifier_continue_property(code_point);
        }
        if !is_valid {
            return false;
        }
        is_first_code_point = false;
    }
    true
}

// https://w3c.github.io/IndexedDB/#valid-key-path
fn is_valid_key_path_string(value: &Utf16String) -> bool {
    // A valid key path is one of:

    // * An empty string.
    if value.is_empty() {
        return true;
    }

    // * An identifier, which is a string matching the IdentifierName production from the ECMAScript Language
    //   Specification [ECMA-262].
    // * A string consisting of two or more identifiers separated by periods (U+002E FULL STOP).
    let view = value.to_utf16();
    let mut identifier_start = 0;
    for index in 0..=view.len() {
        if index < view.len() && view[index] != u16::from(b'.') {
            continue;
        }
        if !is_identifier_name(&view[identifier_start..index]) {
            return false;
        }
        identifier_start = index + 1;
    }
    true
}

// https://w3c.github.io/IndexedDB/#valid-key-path
pub(crate) fn is_valid_key_path(path: &KeyPath) -> bool {
    match path {
        KeyPath::String(value) => is_valid_key_path_string(value),
        // * A non-empty list containing only strings conforming to the above requirements.
        KeyPath::Sequence(values) => !values.is_empty() && values.iter().all(is_valid_key_path_string),
    }
}

// https://w3c.github.io/IndexedDB/#create-a-sorted-name-list
pub(crate) fn create_a_sorted_name_list(vm: &Vm, mut names: Vec<Utf16String>) -> Ref<DOMStringList> {
    // 1. Let sorted be names sorted in ascending order with the code unit less than algorithm.
    names.sort_by(code_unit_order);

    // 2. Return a new DOMStringList associated with sorted.
    DOMStringList::create(vm, names)
}

// https://w3c.github.io/IndexedDB/#commit-a-transaction
pub(crate) fn commit_a_transaction(vm: &Vm, transaction: Ref<IDBTransaction>) {
    // 1. Set transaction’s state to committing.
    transaction.set_state(TransactionState::Committing);

    // AD-HOC: Clear the cleanup event loop so that the microtask checkpoint
    //         does not reset the state back to inactive.
    //         https://github.com/w3c/IndexedDB/issues/489
    transaction.set_cleanup_event_loop(vm, None);

    // 2. Run the following steps in parallel:
    //     1. Wait until every item in transaction’s request list is processed.
    transaction.request_list().set_on_all_processed(
        vm,
        create_function(vm, transaction, |vm, &transaction, ()| {
            // 2. If transaction’s state is no longer committing, then terminate these steps.
            if transaction.state() != TransactionState::Committing {
                return;
            }

            // 3. Attempt to write any outstanding changes made by transaction to the database, considering transaction’s durability hint.
            // FIXME: 4. If an error occurs while writing the changes to the database, then run abort a transaction with transaction and an appropriate type for the error, for example a QuotaExceededError or an "UnknownError" DOMException, and terminate these steps.
            // NB: The backend writes the changes, and the steps go on once it has.
            remote::commit(
                vm,
                transaction,
                create_function(vm, transaction, |vm, &transaction, ()| {
                    // 5. Queue a database task to run these steps:
                    queue_a_database_task(
                        vm,
                        create_function(vm, transaction, |vm, &transaction, ()| {
                            // NB: An abort that came after the backend committed already finished the transaction and
                            //     fired its abort event, so it gets no complete event too.
                            if transaction.state() != TransactionState::Committing {
                                return;
                            }

                            // 1. If transaction is an upgrade transaction, then set transaction’s connection’s associated database’s upgrade transaction to null.
                            if transaction.is_upgrade_transaction() {
                                transaction
                                    .connection_object()
                                    .associated_database()
                                    .set_upgrade_transaction(None);
                            }

                            // AD-HOC: Discard mutation logs now that changes are permanent.
                            transaction.notify_devtools_of_committed_changes();
                            transaction.discard_mutation_logs(vm);

                            // 2. Set transaction’s state to finished.
                            IDBTransaction::set_finished(vm, transaction);

                            // 3. Fire an event named complete at transaction.
                            transaction.event_target().dispatch_event(&CppEvent::create(
                                transaction.realm(),
                                "complete",
                                false,
                                false,
                                false,
                            ));

                            // 4. If transaction is an upgrade transaction, then let request be the request associated with transaction and set request’s transaction to null.
                            if transaction.is_upgrade_transaction() {
                                let request = transaction
                                    .associated_request()
                                    .expect("an upgrade transaction has an open request");
                                request.set_transaction(None);

                                // Ad-hoc: Clear the two-way binding.
                                transaction.set_associated_request(None);
                            }

                            // AD-HOC: Notify pending transaction waits after the complete event fires, so that
                            //         upgrade completion callbacks are queued after any tasks JS creates during
                            //         the complete event handler.
                            transaction.connection_object().check_pending_transaction_waits(vm);
                        }),
                    );
                }),
            );
        }),
    );
}

// https://w3c.github.io/IndexedDB/#clone
pub(crate) fn clone_in_realm(cx: &CallContext, value: Value, transaction: Ref<IDBTransaction>) -> WebIdlResult<Value> {
    // 1. Assert: transaction’s state is active.
    assert!(transaction.state() == TransactionState::Active);

    // 2. Set transaction’s state to inactive.
    transaction.set_state(TransactionState::Inactive);

    // AD-HOC: Always reset the transaction state to active. Otherwise, passing an unserializable value to put()
    //         will prevent any further requests on the transaction.
    //         https://github.com/w3c/IndexedDB/issues/490
    let result = (|| {
        // 3. Let serialized be ? StructuredSerializeForStorage(value).
        let serialized = structured_serialize_for_storage(cx, value)?;

        // 4. Let clone be ? StructuredDeserialize(serialized, targetRealm).
        let clone = structured_deserialize_for_storage(cx, &serialized)?;

        // 5. Set transaction’s state to active.
        // NB: This is handled by the scope guard after step 2.

        // 6. Return clone.
        Ok(clone)
    })();
    transaction.set_state(TransactionState::Active);
    result
}

// https://w3c.github.io/IndexedDB/#convert-a-value-to-a-multientry-key
pub(crate) fn convert_a_value_to_a_multi_entry_key(cx: &CallContext, value: Value) -> WebIdlResult<Rc<Key>> {
    let vm = cx.vm();

    // 1. If input is an Array exotic object, then:
    if value.is_object()
        && let Some(array) = value.as_object().downcast::<Array>()
    {
        let array: Gc<Object> = array.upcast();

        // 1. Let len be ? ToLength( ? Get(input, "length")).
        let len = length_of_array_like(vm, &array)?;

        // 2. Let seen be a new set containing only input.
        let seen = MarkedVec::new(vm);
        seen.push(value);

        // 3. Let keys be a new empty list.
        let mut keys: Vec<Rc<Key>> = Vec::new();

        // 4. Let index be 0.
        let mut index: u64 = 0;

        // 5. While index is less than len:
        while index < len {
            // 1. Let entry be Get(input, index).
            let maybe_entry = array.get(vm, &PropertyKey::from_number(index));

            // 2. If entry is not an abrupt completion, then:
            if let Ok(entry) = maybe_entry {
                // 1. Let key be the result of converting a value to a key with arguments entry and seen.
                let completion_key = convert_a_value_to_a_key(cx, entry, Some(&seen));

                // 2. If key is not "invalid value" or "invalid type" or an abrupt completion, and there is no item in keys equal to key, then append key to keys.
                if let Ok(key) = completion_key
                    && !key.is_invalid()
                    && !keys.iter().any(|other| Key::equals(other, &key))
                {
                    keys.push(key);
                }
            }

            // 3. Increase index by 1.
            index += 1;
        }

        // 6. Return a new array key with value set to keys.
        return Ok(Key::create_array(keys));
    }

    // 2. Otherwise, return the result of converting a value to a key with argument input. Rethrow any exceptions.
    convert_a_value_to_a_key(cx, value, None)
}

pub(crate) struct KeyPathFailure;

// https://w3c.github.io/IndexedDB/#evaluate-a-key-path-on-a-value
pub(crate) fn evaluate_key_path_on_a_value(
    cx: &CallContext,
    mut value: Value,
    key_path: &KeyPath,
) -> WebIdlResult<Result<Value, KeyPathFailure>> {
    let vm = cx.vm();

    let key_path_string = match key_path {
        // 1. If keyPath is a list of strings, then:
        KeyPath::Sequence(key_path_list) => {
            // 1. Let result be a new Array object created as if by the expression [].
            let result = Array::create(vm, cx.realm(), 0, None).must();

            // 2. Let i be 0.
            let mut i: u64 = 0;

            // 3. For each item of keyPath:
            #[allow(clippy::explicit_counter_loop, reason = "the steps count i themselves")]
            for item in key_path_list {
                // 1. Let key be the result of recursively evaluating a key path on a value with item and value.
                let completion_key = evaluate_key_path_on_a_value(cx, value, &KeyPath::String(item.clone()));

                // 2. Assert: key is not an abrupt completion.
                // 3. If key is failure, abort the overall algorithm and return failure.
                let Ok(key) = completion_key.must() else {
                    return Ok(Err(KeyPathFailure));
                };

                // 4. Let p be ! ToString(i).
                let p = PropertyKey::from_number(i);

                // 5. Let status be CreateDataProperty(result, p, key).
                let status = result.create_data_property(vm, &p, key, None, None).must();

                // 6. Assert: status is true.
                assert!(status);

                // 7. Increase i by 1.
                i += 1;
            }

            // 4. Return result.
            return Ok(Ok(Value::from_object(result)));
        }
        KeyPath::String(key_path_string) => key_path_string,
    };

    // 2. If keyPath is the empty string, return value and skip the remaining steps.
    if key_path_string.is_empty() {
        return Ok(Ok(value));
    }

    // 3. Let identifiers be the result of strictly splitting keyPath on U+002E FULL STOP characters (.).
    // 4. For each identifier of identifiers, jump to the appropriate step below:
    for identifier in strictly_split_key_path(key_path_string) {
        let is_length = identifier == Utf16String::from_utf8("length");

        // If Type(value) is String, and identifier is "length"
        if value.is_string() && is_length {
            // Let value be a Number equal to the number of elements in value.
            #[allow(clippy::cast_precision_loss, reason = "a string has fewer than 2^53 code units")]
            {
                value = Value::from_f64(value.as_string().utf16_string().length_in_code_units() as f64);
            }
        }
        // If value is an Array and identifier is "length"
        else if value.is_object() && value.as_object().downcast::<Array>().is_some() && is_length {
            // Let value be ! ToLength(! Get(value, "length")).
            #[allow(clippy::cast_precision_loss, reason = "a length is at most 2^53 - 1")]
            {
                value = Value::from_f64(length_of_array_like(vm, &value.as_object()).must() as f64);
            }
        }
        // If value is a Blob and identifier is "size"
        //     Let value be a Number equal to value’s size.
        // If value is a Blob and identifier is "type"
        //     Let value be a String equal to value’s type.
        // If value is a File and identifier is "name"
        //     Let value be a String equal to value’s name.
        // If value is a File and identifier is "lastModified"
        //     Let value be a Number equal to value’s lastModified.
        else if let Some(key_path_value) = key_path_value_for_blob_or_file(cx, value, &identifier) {
            value = key_path_value;
        }
        // Otherwise
        else {
            // 1. If Type(value) is not Object, return failure.
            if !value.is_object() {
                return Ok(Err(KeyPathFailure));
            }

            let identifier_property = PropertyKey::from_utf16_string(&identifier);

            // 2. Let hop be ! HasOwnProperty(value, identifier).
            let hop = value.as_object().has_own_property(vm, &identifier_property).must();

            // 3. If hop is false, return failure.
            if !hop {
                return Ok(Err(KeyPathFailure));
            }

            // 4. Let value be ! Get(value, identifier).
            value = value.as_object().get(vm, &identifier_property).must();

            // 5. If value is undefined, return failure.
            if value.is_undefined() {
                return Ok(Err(KeyPathFailure));
            }
        }
    }

    // 5. Assert: value is not an abrupt completion.
    // NOTE: Step 4 above makes this assertion via MUST

    // 6. Return value.
    Ok(Ok(value))
}

// https://w3c.github.io/IndexedDB/#extract-a-key-from-a-value-using-a-key-path
pub(crate) fn extract_a_key_from_a_value_using_a_key_path(
    cx: &CallContext,
    value: Value,
    key_path: &KeyPath,
    multi_entry: bool,
) -> WebIdlResult<Result<Rc<Key>, KeyPathFailure>> {
    // 1. Let r be the result of evaluating a key path on a value with value and keyPath. Rethrow any exceptions.
    // 2. If r is failure, return failure.
    let r = match evaluate_key_path_on_a_value(cx, value, key_path)? {
        Ok(r) => r,
        Err(failure) => return Ok(Err(failure)),
    };

    // 3. Let key be the result of converting a value to a key with r if the multiEntry flag is false,
    //    and the result of converting a value to a multiEntry key with r otherwise. Rethrow any exceptions.
    // 4. If key is "invalid value" or "invalid type", return invalid.
    // 5. Return key.
    Ok(Ok(if multi_entry {
        convert_a_value_to_a_multi_entry_key(cx, r)?
    } else {
        convert_a_value_to_a_key(cx, r, None)?
    }))
}

/// AD-HOC: Whether CreateDataProperty(value, identifier, key) would succeed, which inject a key into a value using a
///         key path asserts: a TypedArray refuses a canonical numeric string, such as "Infinity", that is not an
///         index of it.
fn can_create_data_property(value: Gc<Object>, identifier: &Utf16String) -> bool {
    value.downcast::<TypedArrayBase>().is_none()
        || canonical_numeric_index_string(
            &PropertyKey::from_utf16_string(identifier),
            CanonicalIndexMode::DetectNumericRoundtrip,
        )
        .is_undefined()
}

// https://w3c.github.io/IndexedDB/#check-that-a-key-could-be-injected-into-a-value
pub(crate) fn check_that_a_key_could_be_injected_into_a_value(
    cx: &CallContext,
    mut value: Value,
    key_path: &KeyPath,
) -> bool {
    let vm = cx.vm();

    // NOTE: The key paths used in this section are always strings and never sequences
    let KeyPath::String(key_path) = key_path else {
        unreachable!("the key path is a string");
    };

    // 1. Let identifiers be the result of strictly splitting keyPath on U+002E FULL STOP characters (.).
    let mut identifiers = strictly_split_key_path(key_path);

    // 2. Assert: identifiers is not empty.
    assert!(!identifiers.is_empty());

    // 3. Remove the last item of identifiers.
    let last = identifiers.pop().expect("identifiers is not empty");

    // 4. For each remaining identifier of identifiers, if any:
    for identifier in &identifiers {
        // 1. If value is not an Object or an Array, return false.
        if !(value.is_object() || value.is_array(vm).must()) {
            return false;
        }

        let identifier_property = PropertyKey::from_utf16_string(identifier);

        // 2. Let hop be ! HasOwnProperty(value, identifier).
        let hop = value.as_object().has_own_property(vm, &identifier_property).must();

        // 3. If hop is false, return true.
        if !hop {
            return can_create_data_property(value.as_object(), identifier);
        }

        // 4. Let value be ! Get(value, identifier).
        value = value.as_object().get(vm, &identifier_property).must();
    }

    // 5. Return true if value is an Object or an Array, or false otherwise.
    (value.is_object() || value.is_array(vm).must()) && can_create_data_property(value.as_object(), &last)
}

// https://w3c.github.io/IndexedDB/#fire-an-error-event
pub(crate) fn fire_an_error_event(vm: &Vm, request: AnyIDBRequest) {
    // 1. Let event be the result of creating an event using Event.
    // 2. Set event’s type attribute to "error".
    // 3. Set event’s bubbles and cancelable attributes to true.
    let event = CppEvent::create(request.realm(), "error", true, true, false);

    // 4. Let transaction be request’s transaction.
    let transaction = request.transaction_object().expect("the request has a transaction");

    // 5. Let legacyOutputDidListenersThrowFlag be initially false.
    // 6. If transaction’s state is inactive, then set transaction’s state to active.
    if transaction.state() == TransactionState::Inactive {
        transaction.set_state(TransactionState::Active);
    }

    // 7. Dispatch event at request with legacyOutputDidListenersThrowFlag.
    let legacy_output_did_listeners_throw_flag = request
        .event_target()
        .dispatch_event_with_legacy_output_did_listeners_throw_flag(&event);

    // 8. If transaction’s state is active, then:
    if transaction.state() == TransactionState::Active {
        // 1. Set transaction’s state to inactive.
        transaction.set_state(TransactionState::Inactive);

        // 2. If legacyOutputDidListenersThrowFlag is true, then run abort a transaction with transaction and a newly created "AbortError" DOMException and terminate these steps.
        //    This is done even if event’s canceled flag is false.
        if legacy_output_did_listeners_throw_flag {
            abort_a_transaction(
                vm,
                transaction,
                Some(DomException::create(
                    "AbortError",
                    "Error event interrupted by exception",
                )),
            );
            return;
        }

        // 3. If event’s canceled flag is false, then run abort a transaction using transaction and request's error, and terminate these steps.
        if !event.canceled() {
            abort_a_transaction(vm, transaction, request.error_object());
            return;
        }

        // 4. If transaction’s request list is empty, then run commit a transaction with transaction.
        if transaction.request_list().is_empty() {
            commit_a_transaction(vm, transaction);
        }
    }
}

// https://w3c.github.io/IndexedDB/#fire-a-success-event
pub(crate) fn fire_a_success_event(vm: &Vm, request: AnyIDBRequest) {
    // 1. Let event be the result of creating an event using Event.
    // 2. Set event’s type attribute to "success".
    // 3. Set event’s bubbles and cancelable attributes to false.
    let event = CppEvent::create(request.realm(), "success", false, false, false);

    // 4. Let transaction be request’s transaction.
    let transaction = request.transaction_object().expect("the request has a transaction");

    // 5. Let legacyOutputDidListenersThrowFlag be initially false.
    // 6. If transaction’s state is inactive, then set transaction’s state to active.
    if transaction.state() == TransactionState::Inactive {
        transaction.set_state(TransactionState::Active);
    }

    // 7. Dispatch event at request with legacyOutputDidListenersThrowFlag.
    let legacy_output_did_listeners_throw_flag = request
        .event_target()
        .dispatch_event_with_legacy_output_did_listeners_throw_flag(&event);

    // 8. If transaction’s state is active, then:
    if transaction.state() == TransactionState::Active {
        // 1. Set transaction’s state to inactive.
        transaction.set_state(TransactionState::Inactive);

        // 2. If legacyOutputDidListenersThrowFlag is true, then run abort a transaction with transaction and a newly created "AbortError" DOMException.
        if legacy_output_did_listeners_throw_flag {
            abort_a_transaction(
                vm,
                transaction,
                Some(DomException::create("AbortError", "An error occurred")),
            );
            return;
        }

        // 3. If transaction’s request list is empty, then run commit a transaction with transaction.
        if transaction.request_list().is_empty() {
            commit_a_transaction(vm, transaction);
        }
    }
}

// https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
// NB: The backend performs the operation, in the order the agent sent the operations of the transaction, and the
//     conversion turns what it found into the request's result, in the request's realm.
pub(crate) fn asynchronously_execute_a_remote_request(
    vm: &Vm,
    source: IDBRequestSource,
    operation: BackendOperation,
    conversion: Conversion,
    request_input: Option<AnyIDBRequest>,
) -> AnyIDBRequest {
    // 1. Let transaction be the transaction associated with source.
    let transaction = match source {
        IDBRequestSource::Empty => unreachable!("a request with a transaction has a source"),
        IDBRequestSource::ObjectStore(object_store) => object_store.transaction_object(),
        IDBRequestSource::Index(index) => index.transaction_object(),
        IDBRequestSource::Cursor(cursor) => cursor.transaction_object(),
    };

    // 2. Assert: transaction’s state is active.
    assert!(transaction.state() == TransactionState::Active);

    // 3. If request was not given, let request be a new request with source as source.
    let request = request_input.unwrap_or_else(|| {
        AnyIDBRequest::IDBRequest(IDBRequest::create(vm, transaction.connection_object().realm(), source))
    });

    // Set the two-way binding. (Missing spec step)
    // FIXME: https://github.com/w3c/IndexedDB/issues/433
    request.set_transaction(Some(transaction));

    // 4. Add request to the end of transaction’s request list.
    transaction.request_list().enqueue_remote(vm, request);

    // 5. Run these steps in parallel:
    //     1. Wait until request is the first item in transaction’s request list that is not processed.
    //     2. Let result be the result of performing operation.
    // NB: The backend answers the operations of a transaction in order, so when the result arrives, every request
    //     before this one was processed.
    remote::perform(
        vm,
        transaction,
        operation,
        create_function_with(
            vm,
            (transaction, request, conversion),
            |vm, &(transaction, request, conversion), result: Result<OperationResult, BackendError>| {
                if request.aborted() {
                    return;
                }
                let result = match result {
                    Ok(result) => convert_result(vm, conversion, result).map_err(DomException::from_exception),
                    Err(error) => Err(remote::dom_exception_from(&error)),
                };

                // 3. If result is an error and transaction’s state is committing, then run abort a transaction with
                //    transaction and result, and terminate these steps.
                // NB: The events of the requests processed before this one are queued already, and fire first, as
                //     they would if the backend performed this operation only after they did.
                if let Err(error) = &result
                    && transaction.state() == TransactionState::Committing
                {
                    abort_after_queued_events(vm, transaction, error.clone());
                    return;
                }

                // 4. If result is an error, then revert all changes made by operation.
                // NB: An operation of the backend that fails changes nothing.

                // 5. Set request’s processed flag to true.
                request.set_processed(true);

                // 6. Queue a database task to run these steps:
                queue_a_database_task(
                    vm,
                    create_function(
                        vm,
                        (request, result, transaction),
                        |vm, &(request, ref result, transaction), ()| {
                            if request.aborted() {
                                return;
                            }

                            // 1. Remove request from transaction’s request list.
                            transaction.request_list().remove(vm, request);

                            // 2. Set request’s done flag to true.
                            request.set_done(true);

                            match result {
                                // 3. If result is an error, then:
                                Err(error) => {
                                    // 1. Set request’s result to undefined.
                                    request.set_result(Value::UNDEFINED);

                                    // 2. Set request’s error to result.
                                    request.set_error(vm, Some(error.clone()));

                                    // 3. Fire an error event at request.
                                    fire_an_error_event(vm, request);
                                }
                                // 4. Otherwise:
                                Ok(value) => {
                                    // 1. Set request’s result to result.
                                    request.set_result(*value);

                                    // 2. Set request’s error to undefined.
                                    request.set_error(vm, None);

                                    // 3. Fire a success event at request.
                                    fire_a_success_event(vm, request);
                                }
                            }

                            // NB: The commit wait is checked after the result event, so the commit completes after it.
                            transaction.request_list().check_all_processed(vm);
                        },
                    ),
                );
            },
        ),
    );

    // 6. Return request.
    request
}

// https://w3c.github.io/IndexedDB/#inject-a-key-into-a-value-using-a-key-path
pub(crate) fn inject_a_key_into_a_value_using_a_key_path(
    cx: &CallContext,
    mut value: Value,
    key: &Key,
    key_path: &KeyPath,
) {
    let vm = cx.vm();
    let realm = cx.realm();

    let KeyPath::String(key_path) = key_path else {
        unreachable!("the key path of an object store that uses a key generator and in-line keys is a string");
    };

    // 1. Let identifiers be the result of strictly splitting keyPath on U+002E FULL STOP characters (.).
    let mut identifiers = strictly_split_key_path(key_path);

    // 2. Assert: identifiers is not empty.
    assert!(!identifiers.is_empty());

    // 3. Let last be the last item of identifiers and remove it from the list.
    let last = identifiers.pop().expect("identifiers is not empty");

    // 4. For each remaining identifier of identifiers:
    for identifier in &identifiers {
        // 1. Assert: value is an Object or an Array.
        assert!(value.is_object() || value.is_array(vm).must());

        let identifier_property = PropertyKey::from_utf16_string(identifier);

        // 2. Let hop be ! HasOwnProperty(value, identifier).
        let hop = value.as_object().has_own_property(vm, &identifier_property).must();

        // 3. If hop is false, then:
        if !hop {
            // 1. Let o be a new Object created as if by the expression ({}).
            let o = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));

            // 2. Let status be CreateDataProperty(value, identifier, o).
            let status = value
                .as_object()
                .create_data_property(vm, &identifier_property, Value::from_object(o), None, None)
                .must();

            // 3. Assert: status is true.
            assert!(status);
        }

        // 4. Let value be ! Get(value, identifier).
        value = value.as_object().get(vm, &identifier_property).must();
    }

    // 5. Assert: value is an Object or an Array.
    assert!(value.is_object() || value.is_array(vm).must());

    // 6. Let keyValue be the result of converting a key to a value with key.
    let key_value = convert_a_key_to_a_value(cx, key);

    // 7. Let status be CreateDataProperty(value, last, keyValue).
    let status = value
        .as_object()
        .create_data_property(vm, &PropertyKey::from_utf16_string(&last), key_value, None, None)
        .must();

    // 8. Assert: status is true.
    assert!(status);
}

// AD-HOC: The spec deserializes stored records infallibly, but a record read back from storage can be
//         corrupt or written by a newer format version, so treat failure as the "error occurs while
//         reading the value from the underlying storage" case: a "NotReadableError" DOMException.
pub(crate) fn deserialize_a_stored_record(
    cx: &CallContext,
    serialized: &StorageSerializationRecord,
) -> WebIdlResult<Value> {
    structured_deserialize_for_storage(cx, serialized).map_err(|_| {
        Exception::Dom(
            "NotReadableError",
            Utf16String::from_utf8("Stored value could not be read"),
        )
    })
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
// AD-HOC: The index creation itself is processed as an asynchronous request within the upgrade transaction, so the
//         index gets the records that its object store has once the requests made before createIndex() are processed.
//         If those records violate the index's unique flag, the upgrade transaction is aborted with a "ConstraintError"
//         DOMException.
pub(crate) fn queue_the_creation_of_an_index(vm: &Vm, handle: Ref<IDBObjectStore>, index: IndexRef) {
    let realm = handle.realm();
    let transaction = handle.transaction_object();

    // NB: The request has no transaction, so that nothing observes it, and the events fired at it if the transaction
    //     aborts first go nowhere.
    let request = AnyIDBRequest::IDBRequest(IDBRequest::create(
        vm,
        transaction.connection_object().realm(),
        IDBRequestSource::ObjectStore(handle),
    ));
    transaction.request_list().enqueue_remote(vm, request);

    // NB: The backend hands over the records the object store has once the requests made before createIndex() are
    //     done, and the agent computes their index records, which only it can, as script may run to extract them. The
    //     backend skips the ones of a record that a later request changed meanwhile, which got its index records then.
    // NB: The requests made after createIndex() wait until the index has its records. So the index records go under the
    //     names that the backend has when the population starts, even if script renames the index or its object store
    //     before they come back: The backend renames them only after the population.
    let names = NoCells((
        remote::name_from(&index.object_store().name()),
        remote::name_from(&index.name()),
    ));
    remote::perform(
        vm,
        transaction,
        BackendOperation::PopulateIndex {
            store: names.0.0.clone(),
            index: names.0.1.clone(),
        },
        create_function_with(
            vm,
            (realm, transaction, index, request, names),
            |vm, (realm, transaction, index, request, names), result: Result<OperationResult, BackendError>| {
                let (realm, transaction, index, request) = (*realm, *transaction, *index, *request);
                if request.aborted() {
                    return;
                }
                let entries = match result {
                    Ok(OperationResult::Records(records)) => {
                        with_temporary_execution_context_and_callbacks(realm, || {
                            let cx = CallContext::new(vm, realm);
                            records
                                .iter()
                                .map(|record| {
                                    let value = remote_ops::read_record_value(&cx, index.object_store(), record)?;
                                    Ok(remote_ops::IndexKeyEntryOf::entry(
                                        &cx,
                                        index,
                                        value,
                                        &record.primary_key,
                                    ))
                                })
                                .collect::<WebIdlResult<Vec<_>>>()
                                .map_err(DomException::from_exception)
                        })
                    }
                    Ok(_) => Ok(Vec::new()),
                    Err(error) => Err(remote::dom_exception_from(&error)),
                };
                let entries = match entries {
                    Ok(entries) => entries,
                    Err(error) => {
                        abort_after_queued_events(vm, transaction, error);
                        return;
                    }
                };
                remote::perform_now(
                    vm,
                    transaction,
                    BackendOperation::AddIndexKeys {
                        store: names.0.0.clone(),
                        index: names.0.1.clone(),
                        entries,
                    },
                    create_function_with(
                        vm,
                        (transaction, request),
                        |vm, &(transaction, request), result: Result<OperationResult, BackendError>| {
                            if request.aborted() {
                                return;
                            }

                            // If the records violate the index's unique flag, the upgrade transaction is aborted with a
                            // "ConstraintError" DOMException.
                            // NB: The request stays unprocessed until the abort, so that a commit() can't go first.
                            if let Err(error) = result {
                                abort_after_queued_events(vm, transaction, remote::dom_exception_from(&error));
                                return;
                            }
                            request.set_processed(true);

                            // NB: Finish up like a request whose success event nobody listens to: the transaction
                            //     commits once its request list is empty, if it is inactive by then.
                            queue_a_database_task(
                                vm,
                                create_function(vm, (transaction, request), |vm, &(transaction, request), ()| {
                                    if request.aborted() {
                                        return;
                                    }
                                    transaction.request_list().remove(vm, request);
                                    if transaction.state() == TransactionState::Inactive
                                        && transaction.request_list().is_empty()
                                    {
                                        commit_a_transaction(vm, transaction);
                                    }
                                    transaction.request_list().check_all_processed(vm);
                                }),
                            );
                        },
                    ),
                );
                remote::release(transaction);
            },
        ),
    );
    remote::hold(transaction);
}

/// Aborts a transaction once the events of the requests that were processed before are fired, as they would be if the
/// backend performed the failed operation only after those events.
fn abort_after_queued_events(vm: &Vm, transaction: Ref<IDBTransaction>, error: DomException) {
    queue_a_database_task(
        vm,
        create_function(vm, (transaction, error), |vm, &(transaction, ref error), ()| {
            abort_a_transaction(vm, transaction, Some(error.clone()));
        }),
    );
}

// https://w3c.github.io/IndexedDB/#convert-a-value-to-a-key-range
pub(crate) fn convert_a_value_to_a_key_range(
    cx: &CallContext,
    value: Option<Value>,
    null_disallowed: bool,
) -> WebIdlResult<Ref<IDBKeyRange>> {
    // 1. If value is a key range, return value.
    if let Some(value) = value
        && let Some(key_range) = key_range_from_value(value)
    {
        return Ok(key_range);
    }

    // 2. If value is undefined or is null, then throw a "DataError" DOMException if null disallowed flag is true, or return an unbounded key range otherwise.
    let Some(value) = value.filter(|value| !value.is_nullish()) else {
        if null_disallowed {
            return Err(Exception::Dom(
                "DataError",
                Utf16String::from_utf8("Value is undefined or null"),
            ));
        }

        return Ok(IDBKeyRange::create(cx.vm(), None, None, false, false));
    };

    // 3. Let key be the result of converting a value to a key with value. Rethrow any exceptions.
    let key = convert_a_value_to_a_key(cx, value, None)?;

    // 4. If key is "invalid value" or "invalid type", throw a "DataError" DOMException.
    if key.is_invalid() {
        return Err(Exception::Dom("DataError", Utf16String::from_utf8("Value is invalid")));
    }

    // 5. Return a key range containing only key.
    Ok(IDBKeyRange::create(cx.vm(), Some(key.clone()), Some(key), false, false))
}

#[derive(Clone, Copy, PartialEq, Trace)]
pub(crate) enum RecordSource {
    ObjectStore(ObjectStoreRef),
    Index(IndexRef),
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum RecordKind {
    Key,
    Value,
    Record,
}

// https://w3c.github.io/IndexedDB/#queue-a-database-task
pub(crate) fn queue_a_database_task(vm: &Vm, steps: Function) {
    // To queue a database task, perform queue a task on the database access task source.
    crate::indexed_db::cpp::queue_a_task_on_the_database_access_task_source(vm, steps);
}

// https://w3c.github.io/IndexedDB/#cleanup-indexed-database-transactions
pub(crate) fn cleanup_indexed_database_transactions(vm: &Vm, event_loop: &CppCell) -> bool {
    let mut has_matching_event_loop = false;

    // NB: The agent's transactions are the ones its client holds; the backend holds the databases.
    remote::for_each_transaction(vm, |transaction| {
        // 2. For each transaction transaction with cleanup event loop matching the current event loop:
        if transaction.cleanup_event_loop() == Some(event_loop.as_ptr()) {
            has_matching_event_loop = true;

            // 1. Set transaction’s state to inactive.
            assert!(transaction.state() == TransactionState::Active);
            transaction.set_state(TransactionState::Inactive);

            // https://w3c.github.io/IndexedDB/#transaction-commit
            // The implementation must attempt to commit an inactive transaction when all requests placed
            // against the transaction have completed and their returned results handled, no new requests have
            // been placed against the transaction, and the transaction has not been aborted

            if transaction.request_list().is_empty() {
                commit_a_transaction(vm, transaction);
            }

            // 2. Clear transaction’s cleanup event loop.
            transaction.set_cleanup_event_loop(vm, None);
        }
    });

    // 1. If there are no transactions with cleanup event loop matching the current event loop, return false.
    // 3. Return true.
    has_matching_event_loop
}

// https://w3c.github.io/IndexedDB/#is-a-potentially-valid-key-range
pub(crate) fn is_a_potentially_valid_key_range(cx: &CallContext, value: Value) -> bool {
    // 1. If value is a key range, return true.
    if key_range_from_value(value).is_some() {
        return true;
    }

    // 2. Let key be the result of converting a value to a key with value.
    // 3. If key is "invalid type" return false.
    // 4. Else return true.
    // AD-HOC: The type of value is checked instead, which is what decides whether the conversion returns
    //         "invalid type", so that the getters of an array's elements do not run twice.
    if value.is_number() || value.is_string() {
        return true;
    }
    if value.is_object() && value.as_object().downcast::<Date>().is_some() {
        return true;
    }
    if buffer_source_type_of(value).is_some() {
        return true;
    }
    if value.is_object() && value.is_array(cx.vm()).must() {
        return true;
    }
    false
}

fn source_of_source_handle(source_handle: IDBRequestSource) -> (RecordSource, Ref<IDBTransaction>) {
    // 1. Let source be an index or an object store from sourceHandle.
    //    If sourceHandle is an index handle, then source is the index handle’s associated index.
    //    Otherwise, source is the object store handle’s associated object store.
    // 4. Let transaction be sourceHandle’s transaction.
    match source_handle {
        IDBRequestSource::Index(index) => (RecordSource::Index(index.index()), index.transaction_object()),
        IDBRequestSource::ObjectStore(object_store) => (
            RecordSource::ObjectStore(object_store.store()),
            object_store.transaction_object(),
        ),
        IDBRequestSource::Empty | IDBRequestSource::Cursor(_) => {
            unreachable!("the source handle is an index handle or an object store handle")
        }
    }
}

/// The operation that retrieves multiple items from an object store or an index, and the conversion of what it found.
fn retrieve_multiple_items_remote_operation(
    vm: &Vm,
    realm: Gc<Realm>,
    source: RecordSource,
    range: &IDBKeyRange,
    kind: RecordKind,
    direction: IDBCursorDirection,
    count: Option<u32>,
) -> (BackendOperation, Conversion) {
    let (store, from_index, backend_source) = match source {
        RecordSource::ObjectStore(store) => (store, false, remote_ops::store_source(store)),
        RecordSource::Index(index) => (index.object_store(), true, remote_ops::index_source(index)),
    };
    let operation = BackendOperation::GetAll {
        source: backend_source,
        range: remote_ops::backend_range(range),
        count,
        direction: remote_ops::backend_direction(direction),
        retrieve: remote_ops::backend_retrieve(kind),
    };
    (
        operation,
        remote_ops::records_conversion(vm, realm, store, kind, from_index),
    )
}

// https://w3c.github.io/IndexedDB/#create-a-request-to-retrieve-multiple-items
pub(crate) fn create_a_request_to_retrieve_multiple_items(
    cx: &CallContext,
    source_handle: IDBRequestSource,
    kind: RecordKind,
    query_or_options: Value,
    mut count: Option<u32>,
) -> WebIdlResult<AnyIDBRequest> {
    let vm = cx.vm();

    // 1. Let source be an index or an object store from sourceHandle.
    //    If sourceHandle is an index handle, then source is the index handle’s associated index.
    //    Otherwise, source is the object store handle’s associated object store.
    let (source, transaction) = source_of_source_handle(source_handle);

    // 2. If source has been deleted, throw an "InvalidStateError" DOMException.
    // 3. If source is an index and source’s object store has been deleted, throw an "InvalidStateError" DOMException.
    let is_source_or_object_store_deleted = match source {
        RecordSource::ObjectStore(object_store) => object_store.is_deleted(),
        RecordSource::Index(index) => index.is_deleted() || index.object_store().is_deleted(),
    };
    if is_source_or_object_store_deleted {
        return Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_utf8("Source or its object store has been deleted"),
        ));
    }

    // 4. Let transaction be sourceHandle’s transaction.
    // NB: source_of_source_handle() found the transaction with the source.

    // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if !transaction.is_active() {
        return Err(Exception::Dom(
            "TransactionInactiveError",
            Utf16String::from_utf8("Transaction is not active while creating retrieve multiple items request"),
        ));
    }

    // 6. Let range be a key range.
    let range;

    // 7. Let direction be a cursor direction.
    let mut direction = IDBCursorDirection::Next;

    // 8. If running is a potentially valid key range with queryOrOptions is true, then:
    // AD-HOC: Check if query_or_options is null following https://github.com/w3c/IndexedDB/issues/475
    if query_or_options.is_nullish() || is_a_potentially_valid_key_range(cx, query_or_options) {
        // 1. Set range to the result of converting a value to a key range with queryOrOptions. Rethrow any exceptions.
        range = convert_a_value_to_a_key_range(cx, Some(query_or_options), false)?;

        // 2. Set direction to "next".
        direction = IDBCursorDirection::Next;
    }
    // 9. Else:
    else {
        // 1. Set range to the result of converting a value to a key range with queryOrOptions["query"]. Rethrow any exceptions.
        range = convert_a_value_to_a_key_range(
            cx,
            Some(query_or_options.get(vm, &PropertyKey::from_utf8("query"))?),
            false,
        )?;

        // 2. Set count to queryOrOptions["count"].
        count = Some(query_or_options.get(vm, &PropertyKey::from_utf8("count"))?.to_u32(vm)?);

        // 3. Set direction to queryOrOptions["direction"].
        let direction_value = query_or_options
            .get(vm, &PropertyKey::from_utf8("direction"))?
            .to_utf16_string(vm)?;
        if direction_value == Utf16String::from_utf8("next") {
            direction = IDBCursorDirection::Next;
        } else if direction_value == Utf16String::from_utf8("nextunique") {
            direction = IDBCursorDirection::Nextunique;
        } else if direction_value == Utf16String::from_utf8("prev") {
            direction = IDBCursorDirection::Prev;
        } else if direction_value == Utf16String::from_utf8("prevunique") {
            direction = IDBCursorDirection::Prevunique;
        }
    }

    // 10. Let operation be an algorithm to run.
    // 11. If source is an index, set operation to retrieve multiple items from an index with targetRealm, source,
    //     range, kind, direction, and count if given.
    // 12. Else set operation to retrieve multiple items from an object store with targetRealm, source, range, kind,
    //     direction, and count if given.
    let (operation, conversion) =
        retrieve_multiple_items_remote_operation(vm, cx.realm(), source, &range, kind, direction, count);

    // 13. Return the result (an IDBRequest) of running asynchronously execute a request with sourceHandle and operation.
    Ok(asynchronously_execute_a_remote_request(
        vm,
        source_handle,
        operation,
        conversion,
        None,
    ))
}

// https://w3c.github.io/IndexedDB/#create-a-request-to-retrieve-multiple-items
pub(crate) fn create_a_request_to_retrieve_multiple_items_with_options(
    cx: &CallContext,
    source_handle: IDBRequestSource,
    kind: RecordKind,
    options: &IDBGetAllOptions,
) -> WebIdlResult<AnyIDBRequest> {
    let vm = cx.vm();

    // 1. Let source be an index or an object store from sourceHandle.
    //    If sourceHandle is an index handle, then source is the index handle’s associated index.
    //    Otherwise, source is the object store handle’s associated object store.
    let (source, transaction) = source_of_source_handle(source_handle);

    // 2. If source has been deleted, throw an "InvalidStateError" DOMException.
    // 3. If source is an index and source’s object store has been deleted, throw an "InvalidStateError" DOMException.
    let is_source_or_object_store_deleted = match source {
        RecordSource::ObjectStore(object_store) => object_store.is_deleted(),
        RecordSource::Index(index) => index.is_deleted() || index.object_store().is_deleted(),
    };
    if is_source_or_object_store_deleted {
        return Err(Exception::Dom(
            "InvalidStateError",
            Utf16String::from_utf8("Source or its object store has been deleted"),
        ));
    }

    // 4. Let transaction be sourceHandle’s transaction.
    // NB: source_of_source_handle() found the transaction with the source.

    // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if !transaction.is_active() {
        return Err(Exception::Dom(
            "TransactionInactiveError",
            Utf16String::from_utf8("Transaction is not active while creating retrieve multiple items request"),
        ));
    }

    // 6. Let range be a key range.
    let range = convert_a_value_to_a_key_range(cx, Some(options.query), false)?;

    // 10. Let operation be an algorithm to run.
    let (operation, conversion) = retrieve_multiple_items_remote_operation(
        vm,
        cx.realm(),
        source,
        &range,
        kind,
        options.direction,
        options.count,
    );

    // 13. Return the result (an IDBRequest) of running asynchronously execute a request with sourceHandle and operation.
    Ok(asynchronously_execute_a_remote_request(
        vm,
        source_handle,
        operation,
        conversion,
        None,
    ))
}

pub(crate) fn convert_a_key_path_to_a_value(cx: &CallContext, key_path: &KeyPath) -> Value {
    let vm = cx.vm();
    match key_path {
        KeyPath::String(value) => Value::from_string(PrimitiveString::create(vm, value.clone())),
        KeyPath::Sequence(values) => {
            let list = MarkedVec::with_capacity(vm, values.len());
            for entry in values {
                list.push(Value::from_string(PrimitiveString::create(vm, entry.clone())));
            }
            Value::from_object(Array::create_from_list(vm, cx.realm(), &list))
        }
    }
}

pub(crate) fn set_idb_request_result(vm: &Vm, realm: Gc<Realm>, request: AnyIDBRequest, database: Ref<IDBDatabase>) {
    let cx = CallContext::new(vm, realm);
    request.set_result(database.to_js(&cx).must());
}

/// The value of a special key path property of a Blob or a File, https://w3c.github.io/IndexedDB/#key-path-construct
fn key_path_value_for_blob_or_file(cx: &CallContext, value: Value, identifier: &Utf16String) -> Option<Value> {
    if !value.is_object() {
        return None;
    }
    let object = value.as_object();
    if !is_host_instance_of(&object, Blob::wrapper_class()) {
        return None;
    }
    let blob = match implementation_if::<Blob>(object) {
        Some(blob) => AnyBlob::Blob(blob),
        None => AnyBlob::File(implementation_of::<File>(object)),
    };
    if *identifier == Utf16String::from_utf8("size") {
        #[allow(
            clippy::cast_precision_loss,
            reason = "the C++ converted the size to a double as well"
        )]
        return Some(Value::from_f64(blob.raw_bytes().len() as f64));
    }
    if *identifier == Utf16String::from_utf8("type") {
        return Some(Value::from_string(PrimitiveString::create(
            cx.vm(),
            blob.type_string().clone(),
        )));
    }

    if let AnyBlob::File(file) = blob {
        if *identifier == Utf16String::from_utf8("name") {
            return Some(Value::from_string(PrimitiveString::create(
                cx.vm(),
                file.name_string().clone(),
            )));
        }
        if *identifier == Utf16String::from_utf8("lastModified") {
            #[allow(
                clippy::cast_precision_loss,
                reason = "the C++ converted the time to a double as well"
            )]
            return Some(Value::from_f64(file.last_modified_value() as f64));
        }
    }

    None
}
