/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The algorithms of the Indexed Database API, https://w3c.github.io/IndexedDB/#algorithms

use std::rc::Rc;

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{
    Array, ArrayBuffer, ArrayBufferShared, CanonicalIndexMode, Date, EmbedderCell, EmbedderClass, EmbedderValue, Gc,
    MarkedVec, Must, Object, PrimitiveString, PropertyKey, Realm, Trace, TypedArrayBase, Value, Vm,
    canonical_numeric_index_string, is_host_instance_of, length_of_array_like,
};
use libunicode_rust::character_types::{
    code_point_has_identifier_continue_property, code_point_has_identifier_start_property,
};

use crate::bindings::conversions::{CppInterface, ToJs, buffer_source_type_of};
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::event_target::{CppEvent, CppEventTarget};
use crate::bindings::interface::{Interface, Ref, implementation_if, implementation_of};
use crate::bindings::{CallContext, Exception, MustNotThrow, WebIdlResult};
use crate::dom::event::EventInit;
use crate::file_api::blob::{AnyBlob, Blob};
use crate::file_api::file::File;
use crate::indexed_db::cpp::{
    DomException, create_dom_string_list, structured_deserialize, structured_serialize_for_storage,
};
use crate::indexed_db::idb_cursor::{AnyIDBCursor, IDBCursorDirection};
use crate::indexed_db::idb_database::{ConnectionState, IDBDatabase, IDBTransactionDurability};
use crate::indexed_db::idb_key_range::{IDBKeyRange, key_range_from_value};
use crate::indexed_db::idb_object_store::{IDBGetAllOptions, IDBObjectStore};
use crate::indexed_db::idb_record::{IDBRecord, IndexRecord, ObjectStoreRecord, StorageSerializationRecord};
use crate::indexed_db::idb_request::{AnyIDBRequest, IDBRequest, IDBRequestSource};
use crate::indexed_db::idb_transaction::{IDBTransaction, IDBTransactionMode, TransactionState};
use crate::indexed_db::idb_version_change_event::{IDBVersionChangeEvent, IDBVersionChangeEventInit};
use crate::indexed_db::internal::database::Database;
use crate::indexed_db::internal::function::{
    Function, FunctionWith, NoCells, Operation, create_function, create_operation, perform_operation,
};
use crate::indexed_db::internal::index::{Index, IndexRef};
use crate::indexed_db::internal::key::{Key, KeyType, code_unit_order};
use crate::indexed_db::internal::object_store::{ObjectStore, ObjectStoreRef};
use crate::indexed_db::internal::storage::{ConnectionQueueRef, StorageKey, storage};

/// A key path, https://w3c.github.io/IndexedDB/#key-path-construct: a string or a list of strings.
#[derive(Clone)]
pub(crate) enum KeyPath {
    String(Utf16String),
    Sequence(Vec<Utf16String>),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct TaskCounterState {
    remaining_tasks: Cell<usize>,
    after_all: Cell<Option<Function>>,
}

static TASK_COUNTER_STATE_CLASS: EmbedderClass<TaskCounterState> = EmbedderClass::new("IndexedDBTaskCounterState");

impl EmbedderValue for TaskCounterState {
    const CLASS: &'static EmbedderClass<Self> = &TASK_COUNTER_STATE_CLASS;
}

type TaskCounterStateRef = Gc<EmbedderCell<TaskCounterState>>;

impl TaskCounterState {
    fn create(vm: &Vm) -> TaskCounterStateRef {
        TASK_COUNTER_STATE_CLASS.allocate(vm, |_| TaskCounterState {
            remaining_tasks: Cell::new(0),
            after_all: Cell::new(None),
        })
    }

    fn decrement_remaining_tasks(&self, vm: &Vm) {
        assert!(self.remaining_tasks.get() > 0);
        self.remaining_tasks.set(self.remaining_tasks.get() - 1);
        if self.remaining_tasks.get() > 0 {
            return;
        }

        queue_a_database_task(
            vm,
            self.after_all.get().expect("the steps to run after all tasks are set"),
        );
    }
}

fn strictly_split_key_path(key_path: &Utf16String) -> Vec<Utf16String> {
    key_path
        .to_utf16()
        .split(|code_unit| *code_unit == u16::from(b'.'))
        .map(Utf16String::from_utf16)
        .collect()
}

pub(crate) type OpenResult = Result<Ref<IDBDatabase>, DomException>;

fn call_completion<A: 'static>(vm: &Vm, queue: ConnectionQueueRef, completion: FunctionWith<A>, result: A) {
    completion.call(vm, result);
    queue.request_list.on_request_processed(vm);
}

// https://w3c.github.io/IndexedDB/#open-a-database-connection
pub(crate) fn open_a_database_connection(
    vm: &Vm,
    realm: Gc<Realm>,
    storage_key: StorageKey,
    name: Utf16String,
    maybe_version: Option<u64>,
    request: AnyIDBRequest,
    on_complete: FunctionWith<OpenResult>,
) {
    // 1. Let queue be the connection queue for storageKey and name.
    let queue = storage(vm).connection_queue_for_key_and_name(vm, &storage_key, &name);

    // 2. Add request to queue.
    // 3. Wait until all previous requests in queue have been processed.
    let steps = create_function(
        vm,
        (
            realm,
            queue,
            NoCells(storage_key),
            name,
            maybe_version,
            request,
            on_complete,
        ),
        |vm, &(realm, queue, ref storage_key, ref name, maybe_version, request, on_complete), ()| {
            // 4. Let db be the database named name in storageKey, or null otherwise.
            let maybe_db = Database::for_key_and_name(vm, &storage_key.0, name);

            // 5. If version is undefined, let version be 1 if db is null, or db’s version otherwise.
            let version = maybe_version.unwrap_or_else(|| maybe_db.map_or(1, |db| db.version()));

            // 6. If db is null, let db be a new database with name name, version 0 (zero), and with no object stores.
            //    If this fails for any reason, return an appropriate error (e.g. a QuotaExceededError, or an "UnknownError" DOMException).
            let db = maybe_db.unwrap_or_else(|| Database::create_for_key_and_name(vm, &storage_key.0, name));

            // 7. If db’s version is greater than version, return a newly created "VersionError" DOMException and abort these steps.
            if db.version() > version {
                call_completion(
                    vm,
                    queue,
                    on_complete,
                    Err(DomException::create(
                        "VersionError",
                        "Database version is greater than the requested version",
                    )),
                );
                return;
            }

            // 8. Let connection be a new connection to db.
            let connection = IDBDatabase::create(vm, realm, db);

            // 9. Set connection’s version to version.
            connection.set_version(version);

            // 10. If db’s version is less than version, then:
            if db.version() < version {
                // 1. Let openConnections be the set of all connections, except connection, associated with db.
                let open_connections = db.associated_connections_except(vm, connection);

                // 2. For each entry of openConnections that does not have its close pending flag set to true,
                //    queue a database task to fire a version change event named versionchange at entry with db’s version and version.
                let mut task_counter_state: Option<TaskCounterStateRef> = None;
                for &entry in open_connections.values().iter() {
                    if !entry.close_pending() {
                        let state = *task_counter_state.get_or_insert_with(|| TaskCounterState::create(vm));

                        state.remaining_tasks.set(state.remaining_tasks.get() + 1);
                        queue_a_database_task(
                            vm,
                            create_function(
                                vm,
                                (realm, entry, db, version, state),
                                |vm, &(realm, entry, db, version, state), ()| {
                                    fire_a_version_change_event(
                                        vm,
                                        realm,
                                        "versionchange",
                                        entry.event_target(),
                                        db.version(),
                                        Some(version),
                                    );
                                    state.decrement_remaining_tasks(vm);
                                },
                            ),
                        );
                    }
                }

                // 3. Wait for all of the events to be fired.
                let after_all = create_function(
                    vm,
                    (
                        realm,
                        queue,
                        OpenConnections(open_connections.to_vec()),
                        db,
                        version,
                        connection,
                        request,
                        on_complete,
                    ),
                    |vm, &(realm, queue, ref open_connections, db, version, connection, request, on_complete), ()| {
                        // 4. If any of the connections in openConnections are still not closed,
                        //    queue a database task to fire a version change event named blocked at request with db’s version and version.
                        let any_connection_is_not_closed = open_connections
                            .0
                            .iter()
                            .any(|entry| entry.state() != ConnectionState::Closed);
                        if any_connection_is_not_closed {
                            queue_a_database_task(
                                vm,
                                create_function(
                                    vm,
                                    (realm, request, db, version),
                                    |vm, &(realm, request, db, version), ()| {
                                        fire_a_version_change_event(
                                            vm,
                                            realm,
                                            "blocked",
                                            request.event_target(),
                                            db.version(),
                                            Some(version),
                                        );
                                    },
                                ),
                            );
                        }

                        // 5. Wait until all connections in openConnections are closed.
                        db.wait_for_connections_to_close(
                            vm,
                            &open_connections.0,
                            create_function(
                                vm,
                                (realm, queue, connection, version, request, on_complete),
                                |vm, &(realm, queue, connection, version, request, on_complete), ()| {
                                    // 6. Run upgrade a database using connection, version and request.
                                    upgrade_a_database(
                                        vm,
                                        realm,
                                        connection,
                                        version,
                                        request,
                                        create_function(
                                            vm,
                                            (queue, connection, request, on_complete),
                                            |vm, &(queue, connection, request, on_complete), ()| {
                                                // 7. If connection was closed, return a newly created "AbortError" DOMException and abort these steps.
                                                if connection.state() == ConnectionState::Closed {
                                                    call_completion(
                                                        vm,
                                                        queue,
                                                        on_complete,
                                                        Err(DomException::create(
                                                            "AbortError",
                                                            "Connection was closed",
                                                        )),
                                                    );
                                                    return;
                                                }

                                                // 8. If request's error is set, run the steps to close a database connection with connection,
                                                //    return a newly created "AbortError" DOMException and abort these steps.
                                                if request.has_error() {
                                                    close_a_database_connection(
                                                        vm,
                                                        connection,
                                                        Some(create_function(
                                                            vm,
                                                            (queue, on_complete),
                                                            |vm, &(queue, on_complete), ()| {
                                                                call_completion(
                                                                    vm,
                                                                    queue,
                                                                    on_complete,
                                                                    Err(DomException::create(
                                                                        "AbortError",
                                                                        "Upgrade transaction was aborted",
                                                                    )),
                                                                );
                                                            },
                                                        )),
                                                        false,
                                                    );
                                                    return;
                                                }

                                                // 11. Return connection.
                                                call_completion(vm, queue, on_complete, Ok(connection));
                                            },
                                        ),
                                    );
                                },
                            ),
                        );
                    },
                );

                if let Some(state) = task_counter_state {
                    state.after_all.set(Some(after_all));
                } else {
                    queue_a_database_task(vm, after_all);
                }

                // NOTE: Because of the async nature of this function, we return here and call the on_complete function
                //       with the connection when necessary.
                return;
            }

            // 11. Return connection.
            call_completion(vm, queue, on_complete, Ok(connection));
        },
    );
    queue.request_list.enqueue(vm, request, steps);
}

/// The connections that an algorithm waits on, which it keeps alive while it waits.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct OpenConnections(Vec<Ref<IDBDatabase>>);

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
    connection.set_close_pending(true);

    // 2. If the forced flag is true, then for each transaction created using connection run abort a transaction with transaction and newly created "AbortError" DOMException.
    if forced {
        for transaction in connection.transactions(vm).values().iter() {
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

                let database = connection.associated_database();
                database.dissociate(connection);

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

                database.check_pending_connection_wait(vm);
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
    storage_key: StorageKey,
    name: Utf16String,
    request: AnyIDBRequest,
    on_complete: FunctionWith<DeleteResult>,
) {
    // 1. Let queue be the connection queue for storageKey and name.
    let queue = storage(vm).connection_queue_for_key_and_name(vm, &storage_key, &name);

    // 2. Add request to queue.
    // 3. Wait until all previous requests in queue have been processed.
    let steps = create_function(
        vm,
        (realm, queue, NoCells(storage_key), name, request, on_complete),
        |vm, &(realm, queue, ref storage_key, ref name, request, on_complete), ()| {
            // 4. Let db be the database named name in storageKey, if one exists. Otherwise, return 0 (zero).
            let Some(db) = Database::for_key_and_name(vm, &storage_key.0, name) else {
                call_completion(vm, queue, on_complete, Ok(0));
                return;
            };

            // 5. Let openConnections be the set of all connections associated with db.
            let open_connections = db.associated_connections(vm);

            // 6. For each entry of openConnections that does not have its close pending flag set to true,
            //    queue a database task to fire a version change event named versionchange at entry with db’s version and null.
            let mut task_counter_state: Option<TaskCounterStateRef> = None;
            for &entry in open_connections.values().iter() {
                if !entry.close_pending() {
                    let state = *task_counter_state.get_or_insert_with(|| TaskCounterState::create(vm));

                    state.remaining_tasks.set(state.remaining_tasks.get() + 1);
                    queue_a_database_task(
                        vm,
                        create_function(vm, (realm, entry, db, state), |vm, &(realm, entry, db, state), ()| {
                            fire_a_version_change_event(
                                vm,
                                realm,
                                "versionchange",
                                entry.event_target(),
                                db.version(),
                                None,
                            );
                            state.decrement_remaining_tasks(vm);
                        }),
                    );
                }
            }

            // 7. Wait for all of the events to be fired.
            let after_all = create_function(
                vm,
                (
                    realm,
                    queue,
                    OpenConnections(open_connections.to_vec()),
                    db,
                    NoCells(storage_key.0.clone()),
                    name.clone(),
                    request,
                    on_complete,
                ),
                |vm, &(realm, queue, ref open_connections, db, ref storage_key, ref name, request, on_complete), ()| {
                    // 8. If any of the connections in openConnections are still not closed, queue a database task to fire a
                    //    version change event named blocked at request with db’s version and null.
                    let any_connection_is_not_closed = open_connections
                        .0
                        .iter()
                        .any(|entry| entry.state() != ConnectionState::Closed);
                    if any_connection_is_not_closed {
                        queue_a_database_task(
                            vm,
                            create_function(vm, (realm, request, db), |vm, &(realm, request, db), ()| {
                                fire_a_version_change_event(
                                    vm,
                                    realm,
                                    "blocked",
                                    request.event_target(),
                                    db.version(),
                                    None,
                                );
                            }),
                        );
                    }

                    // 9. Wait until all connections in openConnections are closed.
                    db.wait_for_connections_to_close(
                        vm,
                        &open_connections.0,
                        create_function(
                            vm,
                            (queue, db, storage_key.clone(), name.clone(), on_complete),
                            |vm, &(queue, db, ref storage_key, ref name, on_complete), ()| {
                                // 10. Let version be db’s version.
                                let version = db.version();

                                // 11. Delete db. If this fails for any reason, return an appropriate error (e.g. a QuotaExceededError, or an "UnknownError" DOMException).
                                Database::delete_for_key_and_name(vm, &storage_key.0, name);

                                // 12. Return version.
                                call_completion(vm, queue, on_complete, Ok(version));
                            },
                        ),
                    );
                },
            );

            if let Some(state) = task_counter_state {
                state.after_all.set(Some(after_all));
            } else {
                queue_a_database_task(vm, after_all);
            }
        },
    );
    queue.request_list.enqueue(vm, request, steps);
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
    transaction.revert_all_mutations(vm);
    transaction.discard_mutation_logs(vm);

    // 3. If transaction is an upgrade transaction, run the steps to abort an upgrade transaction with transaction.
    if transaction.is_upgrade_transaction() {
        abort_an_upgrade_transaction(vm, transaction);
    }

    // 4. Set transaction’s state to finished.
    transaction.set_state(TransactionState::Finished);

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
pub(crate) fn create_a_sorted_name_list(cx: &CallContext, mut names: Vec<Utf16String>) -> CppInterface {
    // 1. Let sorted be names sorted in ascending order with the code unit less than algorithm.
    names.sort_by(code_unit_order);

    // 2. Return a new DOMStringList associated with sorted.
    create_dom_string_list(cx, &names)
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

            // FIXME: 3. Attempt to write any outstanding changes made by transaction to the database, considering transaction’s durability hint.
            // FIXME: 4. If an error occurs while writing the changes to the database, then run abort a transaction with transaction and an appropriate type for the error, for example a QuotaExceededError or an "UnknownError" DOMException, and terminate these steps.

            // 5. Queue a database task to run these steps:
            queue_a_database_task(
                vm,
                create_function(vm, transaction, |vm, &transaction, ()| {
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
                    transaction.set_state(TransactionState::Finished);

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
        let clone = structured_deserialize(cx, &serialized)?;

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
pub(crate) fn asynchronously_execute_a_request(
    vm: &Vm,
    source: IDBRequestSource,
    operation: Operation,
    request_input: Option<AnyIDBRequest>,
) -> AnyIDBRequest {
    // 1. Let transaction be the transaction associated with source.
    let transaction = match source {
        IDBRequestSource::Empty => unreachable!("a request with a transaction has a source"),
        IDBRequestSource::ObjectStore(object_store) => object_store.transaction_object(),
        IDBRequestSource::Index(index) => index.transaction_object(),
        IDBRequestSource::Cursor(cursor) => cursor.transaction_object(),
    };

    // AD-HOC: Determine the object store being operated on so that we can get the exact mutation log this
    //         operation will be writing to.
    let store = match source {
        IDBRequestSource::Empty => unreachable!("a request with a transaction has a source"),
        IDBRequestSource::ObjectStore(object_store) => object_store.store(),
        IDBRequestSource::Index(index) => index.object_store_handle().store(),
        IDBRequestSource::Cursor(cursor) => cursor.effective_object_store(),
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
    // 5. Run these steps in parallel:
    //     1. Wait until request is the first item in transaction’s request list that is not processed.
    transaction.request_list().enqueue(
        vm,
        request,
        create_function(
            vm,
            (transaction, store, operation, request),
            |vm, &(transaction, store, operation, request), ()| {
                if request.aborted() {
                    return;
                }

                // AD-HOC: Determine where the mutation logs for this operation begin in case we need to revert.
                let log_position = store.mutation_log_position();

                // 2. Let result be the result of performing operation.
                let result = perform_operation(vm, operation).map_err(DomException::from_exception);

                // 3. If result is an error and transaction’s state is committing, then run abort a transaction with transaction and result, and terminate these steps.
                if let Err(error) = &result
                    && transaction.state() == TransactionState::Committing
                {
                    abort_a_transaction(vm, transaction, Some(error.clone()));
                    return;
                }

                // 4. If result is an error, then revert all changes made by operation.
                if result.is_err() {
                    ObjectStore::revert_mutations_from(store, vm, log_position);
                }

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

                            // 3. If result is an error, then:
                            match result {
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

                            // NB: Check if the commit wait is satisfied after firing the result event, so that the commit completion
                            //     runs after it. This models the parallel nature of the processing according to the spec, where a
                            //     request being processed necessitates sending a message from the IDB worker thread/process to the
                            //     main thread (most likely also triggering this task), meaning that the above events will necessarily
                            //     be fired before the transaction can be committed.
                            transaction.request_list().check_all_processed(vm);
                        },
                    ),
                );

                // Allow the next operation in the queue to proceed. This runs after the above task to ensure that if another
                // request is ready, it will run after the success or failure events are fired. Otherwise, the next request may
                // throw an error and clobber the result of this request.
                transaction.request_list().on_request_processed(vm);
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

// https://w3c.github.io/IndexedDB/#delete-records-from-an-object-store
pub(crate) fn delete_records_from_an_object_store(vm: &Vm, store: ObjectStoreRef, range: &IDBKeyRange) -> Value {
    // 1. Remove all records, if any, from store’s list of records with key in range.
    store.remove_records_in_range(vm, range);

    // 2. For each index which references store, remove every record from index’s list of records whose value is in range, if any such records exist.
    for index in store.indexes(vm).values().iter().copied() {
        Index::remove_records_with_value_in_range(index, vm, range);
    }

    // 3. Return undefined.
    Value::UNDEFINED
}

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
// NB: The steps of storing a record into an object store for one index which references store, as step 5 runs them for
//     each such index.
fn store_a_record_into_an_index(cx: &CallContext, index: IndexRef, value: Value, key: &Rc<Key>) -> WebIdlResult<()> {
    let vm = cx.vm();

    // 1. Let index key be the result of extracting a key from a value using a key path with value, index’s key path, and index’s multiEntry flag.
    let completion_index_key =
        extract_a_key_from_a_value_using_a_key_path(cx, value, index.key_path(), index.multi_entry());

    // 2. If index key is an exception, or invalid, or failure, take no further actions for index, and continue these steps for the next index.
    let Ok(failure_index_key) = completion_index_key else {
        return Ok(());
    };

    let Ok(index_key) = failure_index_key else {
        return Ok(());
    };

    if index_key.is_invalid() {
        return Ok(());
    }

    let index_multi_entry = index.multi_entry();
    let index_key_is_array = index_key.r#type() == KeyType::Array;
    let index_is_unique = index.unique();

    // 3. If index’s multiEntry flag is false, or if index key is not an array key,
    //    and if index already contains a record with key equal to index key,
    //    and index’s unique flag is true,
    //    then this operation failed with a "ConstraintError" DOMException.
    //    Abort this algorithm without taking any further steps.
    if (!index_multi_entry || !index_key_is_array) && index_is_unique && index.has_record_with_key(&index_key) {
        return Err(Exception::Dom(
            "ConstraintError",
            Utf16String::from_utf8("Record already exists in index"),
        ));
    }

    // 4. If index’s multiEntry flag is true and index key is an array key,
    //    and if index already contains a record with key equal to any of the subkeys of index key,
    //    and index’s unique flag is true,
    //    then this operation failed with a "ConstraintError" DOMException.
    //    Abort this algorithm without taking any further steps.
    if index_multi_entry && index_key_is_array && index_is_unique {
        for subkey in index_key.subkeys() {
            if index.has_record_with_key(subkey) {
                return Err(Exception::Dom(
                    "ConstraintError",
                    Utf16String::from_utf8("Record already exists in index"),
                ));
            }
        }
    }

    // 5. If index’s multiEntry flag is false, or if index key is not an array key
    //    then store a record in index containing index key as its key and key as its value.
    //    The record is stored in index’s list of records such that the list is sorted primarily on the records keys,
    //    and secondarily on the records values, in ascending order.
    if !index_multi_entry || !index_key_is_array {
        let index_record = IndexRecord {
            key: index_key.clone(),
            value: key.clone(),
        };
        Index::store_a_record(index, vm, &index_record);
    }

    // 6. If index’s multiEntry flag is true and index key is an array key,
    //    then for each subkey of the subkeys of index key store a record in index containing subkey as its key and key as its value.
    //    The records are stored in index’s list of records such that the list is sorted primarily on the records keys,
    //    and secondarily on the records values, in ascending order.
    if index_multi_entry && index_key_is_array {
        for subkey in index_key.subkeys() {
            let index_record = IndexRecord {
                key: subkey.clone(),
                value: key.clone(),
            };
            Index::store_a_record(index, vm, &index_record);
        }
    }

    Ok(())
}

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
pub(crate) fn store_a_record_into_an_object_store(
    cx: &CallContext,
    store: ObjectStoreRef,
    value: Value,
    mut key: Option<Rc<Key>>,
    no_overwrite: bool,
) -> WebIdlResult<Option<Rc<Key>>> {
    let vm = cx.vm();

    // 1. If store uses a key generator, then:
    if store.uses_a_key_generator() {
        match &key {
            // 1. If key is undefined, then:
            None => {
                // 1. Let key be the result of generating a key for store.
                let maybe_key = store.generate_a_key(vm);

                // 2. If key is failure, then this operation failed with a "ConstraintError" DOMException. Abort this algorithm without taking any further steps.
                let generated_key = match maybe_key {
                    Ok(generated_key) => generated_key,
                    Err(error) => return Err(Exception::Dom("ConstraintError", Utf16String::from_utf8(error))),
                };

                #[allow(clippy::cast_precision_loss, reason = "a generated key is at most 2^53")]
                let generated_key = Key::create_number(generated_key as f64);
                key = Some(generated_key.clone());

                // 3. If store also uses in-line keys, then run inject a key into a value using a key path with value, key and store’s key path.
                if store.uses_inline_keys() {
                    inject_a_key_into_a_value_using_a_key_path(
                        cx,
                        value,
                        &generated_key,
                        store.key_path().expect("the store uses in-line keys"),
                    );
                }
            }

            // 2. Otherwise, run possibly update the key generator for store with key.
            Some(key) => {
                store.possibly_update_the_key_generator(vm, key);
            }
        }
    }
    let key = key.expect("the key was given or generated");

    // 2. If the no-overwrite flag was given to these steps and is true, and a record already exists in store with its key equal to key,
    //    then this operation failed with a "ConstraintError" DOMException. Abort this algorithm without taking any further steps.
    let has_record = store.has_record_with_key(&key);
    if no_overwrite && has_record {
        return Err(Exception::Dom(
            "ConstraintError",
            Utf16String::from_utf8("Record already exists"),
        ));
    }

    // 3. If a record already exists in store with its key equal to key, then remove the record from store using delete records from an object store.
    if has_record {
        let key_range = IDBKeyRange::create(vm, Some(key.clone()), Some(key.clone()), false, false);
        delete_records_from_an_object_store(vm, store, &key_range);
    }

    // 4. Store a record in store containing key as its key and ! StructuredSerializeForStorage(value) as its value.
    //    The record is stored in the object store’s list of records such that the list is sorted according to the key of the records in ascending order.
    let record = ObjectStoreRecord {
        key: key.clone(),
        value: structured_serialize_for_storage(cx, value).must(),
    };
    store.store_a_record(vm, record);

    // 5. For each index which references store:
    for index in store.indexes(vm).values().iter().copied() {
        // AD-HOC: An index whose creation request has not been processed yet gets the records of store once it is.
        if index.is_pending_creation() {
            continue;
        }

        store_a_record_into_an_index(cx, index, value, &key)?;
    }

    // 6. Return key.
    Ok(Some(key))
}

// AD-HOC: The spec deserializes stored records infallibly, but a record read back from storage can be
//         corrupt or written by a newer format version, so treat failure as the "error occurs while
//         reading the value from the underlying storage" case: a "NotReadableError" DOMException.
fn deserialize_a_stored_record(cx: &CallContext, serialized: &StorageSerializationRecord) -> WebIdlResult<Value> {
    structured_deserialize(cx, serialized).map_err(|_| {
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
    index.set_pending_creation(true);

    // NB: The request has no transaction, so that nothing observes it, and the events fired at it if the transaction
    //     aborts first go nowhere.
    let request = AnyIDBRequest::IDBRequest(IDBRequest::create(
        vm,
        transaction.connection_object().realm(),
        IDBRequestSource::ObjectStore(handle),
    ));
    transaction.request_list().enqueue(
        vm,
        request,
        create_function(
            vm,
            (realm, transaction, index, request),
            |vm, &(realm, transaction, index, request), ()| {
                if request.aborted() {
                    return;
                }

                index.set_pending_creation(false);
                let cx = CallContext::new(vm, realm);
                let store_the_records_into_the_index = || -> WebIdlResult<()> {
                    for record in index.object_store().records().iter() {
                        let value = deserialize_a_stored_record(&cx, &record.value)?;
                        store_a_record_into_an_index(&cx, index, value, &record.key)?;
                    }
                    Ok(())
                };
                let result = store_the_records_into_the_index();

                request.set_processed(true);

                if let Err(exception) = result {
                    abort_a_transaction(vm, transaction, Some(DomException::from_exception(exception)));
                    return;
                }

                // NB: Finish up like a request whose success event nobody listens to: the transaction commits once its request
                //     list is empty, if it is inactive by then.
                queue_a_database_task(
                    vm,
                    create_function(vm, (transaction, request), |vm, &(transaction, request), ()| {
                        if request.aborted() {
                            return;
                        }

                        transaction.request_list().remove(vm, request);
                        if transaction.state() == TransactionState::Inactive && transaction.request_list().is_empty() {
                            commit_a_transaction(vm, transaction);
                        }
                        transaction.request_list().check_all_processed(vm);
                    }),
                );

                transaction.request_list().on_request_processed(vm);
            },
        ),
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
#[gc(crate = libjs_rust)]
pub(crate) enum RecordSource {
    ObjectStore(ObjectStoreRef),
    Index(IndexRef),
}

// https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
pub(crate) fn count_the_records_in_a_range(source: RecordSource, range: &IDBKeyRange) -> Value {
    // 1. Let count be the number of records, if any, in source’s list of records with key in range.
    let count = match source {
        RecordSource::ObjectStore(object_store) => object_store.count_records_in_range(range),
        RecordSource::Index(index) => index.count_records_in_range(range),
    };

    // 2. Return count.
    #[allow(clippy::cast_precision_loss, reason = "a count of records is less than 2^53")]
    Value::from_f64(count as f64)
}

// https://w3c.github.io/IndexedDB/#retrieve-a-value-from-an-object-store
pub(crate) fn retrieve_a_value_from_an_object_store(
    cx: &CallContext,
    store: ObjectStoreRef,
    range: &IDBKeyRange,
) -> WebIdlResult<Value> {
    // 1. Let record be the first record in store’s list of records whose key is in range, if any.
    let record = store.first_in_range(range);

    // 2. If record was not found, return undefined.
    let Some(record) = record else {
        return Ok(Value::UNDEFINED);
    };

    // 3. Let serialized be record’s value. If an error occurs while reading the value from the underlying storage, return a newly created "NotReadableError" DOMException.
    let serialized = &record.value;

    // 4. Return ! StructuredDeserialize(serialized, targetRealm).
    deserialize_a_stored_record(cx, serialized)
}

#[derive(Clone)]
enum FoundRecord {
    ObjectStore(ObjectStoreRecord),
    Index(IndexRecord),
}

impl FoundRecord {
    fn key(&self) -> &Rc<Key> {
        match self {
            Self::ObjectStore(record) => &record.key,
            Self::Index(record) => &record.key,
        }
    }
}

#[derive(Clone, Copy)]
enum RecordInList<'a> {
    ObjectStore(&'a ObjectStoreRecord),
    Index(&'a IndexRecord),
}

impl RecordInList<'_> {
    fn key(&self) -> &Key {
        match self {
            Self::ObjectStore(record) => &record.key,
            Self::Index(record) => &record.key,
        }
    }

    fn index_record(&self) -> &IndexRecord {
        match self {
            Self::Index(record) => record,
            Self::ObjectStore(_) => unreachable!("the source of the cursor is an index"),
        }
    }

    fn to_found_record(self) -> FoundRecord {
        match self {
            Self::ObjectStore(record) => FoundRecord::ObjectStore(record.clone()),
            Self::Index(record) => FoundRecord::Index(record.clone()),
        }
    }
}

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
pub(crate) fn iterate_a_cursor(
    cx: &CallContext,
    cursor: AnyIDBCursor,
    key: Option<Rc<Key>>,
    primary_key: Option<Rc<Key>>,
    mut count: u64,
) -> WebIdlResult<Option<AnyIDBCursor>> {
    // 1. Let source be cursor’s source.
    let source = cursor.internal_source();

    // 2. Let direction be cursor’s direction.
    let direction = cursor.direction_value();

    // 3. Assert: if primaryKey is given, source is an index and direction is "next" or "prev".
    let direction_is_next_or_prev = direction == IDBCursorDirection::Next || direction == IDBCursorDirection::Prev;
    if primary_key.is_some() {
        assert!(matches!(source, RecordSource::Index(_)) && direction_is_next_or_prev);
    }

    // 4. Let records be the list of records in source.
    // NB: The records are borrowed from the source below, while they are searched.

    // 5. Let range be cursor’s range.
    let range = cursor.range();

    // 6. Let position be cursor’s position.
    let mut position = cursor.position();

    // 7. Let object store position be cursor’s object store position.
    let mut object_store_position = cursor.object_store_position();

    // 8. If count is not given, let count be 1.
    // NOTE: This is handled by the default parameter

    let source_is_object_store = matches!(source, RecordSource::ObjectStore(_));
    let source_is_index = matches!(source, RecordSource::Index(_));

    let next_requirements =
        |record: RecordInList<'_>, position: &Option<Rc<Key>>, object_store_position: &Option<Rc<Key>>| -> bool {
            // * If key is defined:
            if let Some(key) = &key {
                // * The record’s key is greater than or equal to key.
                if !Key::greater_than_or_equal(record.key(), key) {
                    return false;
                }
            }

            // * If primaryKey is defined:
            if let Some(primary_key) = &primary_key {
                let inner_record = record.index_record();
                let key = key.as_ref().expect("primaryKey is given with key");

                // * The record’s key is equal to key and the record’s value is greater than or equal to primaryKey
                if Key::equals(&inner_record.key, key) {
                    if !Key::greater_than_or_equal(&inner_record.value, primary_key) {
                        return false;
                    }
                }
                // * The record’s key is greater than key.
                else if !Key::greater_than(&inner_record.key, key) {
                    return false;
                }
            }

            // * If position is defined and source is an object store:
            if let Some(position) = position
                && source_is_object_store
            {
                // * The record’s key is greater than position.
                if !Key::greater_than(record.key(), position) {
                    return false;
                }
            }

            // * If position is defined and source is an index:
            if let Some(position) = position
                && source_is_index
            {
                let inner_record = record.index_record();
                let object_store_position = object_store_position
                    .as_ref()
                    .expect("a cursor of an index with a position has an object store position");

                // * The record’s key is equal to position and the record’s value is greater than object store position
                if Key::equals(&inner_record.key, position) {
                    if !Key::greater_than(&inner_record.value, object_store_position) {
                        return false;
                    }
                }
                // * The record’s key is greater than position.
                else if !Key::greater_than(&inner_record.key, position) {
                    return false;
                }
            }

            // * The record’s key is in range.
            range.is_in_range(record.key())
        };

    let next_unique_requirements = |record: RecordInList<'_>, position: &Option<Rc<Key>>| -> bool {
        // * If key is defined:
        if let Some(key) = &key {
            // * The record’s key is greater than or equal to key.
            if !Key::greater_than_or_equal(record.key(), key) {
                return false;
            }
        }

        // * If position is defined:
        if let Some(position) = position {
            // * The record’s key is greater than position.
            if !Key::greater_than(record.key(), position) {
                return false;
            }
        }

        // * The record’s key is in range.
        range.is_in_range(record.key())
    };

    let prev_requirements =
        |record: RecordInList<'_>, position: &Option<Rc<Key>>, object_store_position: &Option<Rc<Key>>| -> bool {
            // * If key is defined:
            if let Some(key) = &key {
                // * The record’s key is less than or equal to key.
                if !Key::less_than_or_equal(record.key(), key) {
                    return false;
                }
            }

            // * If primaryKey is defined:
            if let Some(primary_key) = &primary_key {
                let inner_record = record.index_record();
                let key = key.as_ref().expect("primaryKey is given with key");

                // * The record’s key is equal to key and the record’s value is less than or equal to primaryKey
                if Key::equals(&inner_record.key, key) {
                    if !Key::less_than_or_equal(&inner_record.value, primary_key) {
                        return false;
                    }
                }
                // * The record’s key is less than key.
                else if !Key::less_than(&inner_record.key, key) {
                    return false;
                }
            }

            // * If position is defined and source is an object store:
            if let Some(position) = position
                && source_is_object_store
            {
                // * The record’s key is less than position.
                if !Key::less_than(record.key(), position) {
                    return false;
                }
            }

            // * If position is defined and source is an index:
            if let Some(position) = position
                && source_is_index
            {
                let inner_record = record.index_record();
                let object_store_position = object_store_position
                    .as_ref()
                    .expect("a cursor of an index with a position has an object store position");

                // * The record’s key is equal to position and the record’s value is less than object store position
                if Key::equals(&inner_record.key, position) {
                    if !Key::less_than(&inner_record.value, object_store_position) {
                        return false;
                    }
                }
                // * The record’s key is less than position.
                else if !Key::less_than(&inner_record.key, position) {
                    return false;
                }
            }

            // * The record’s key is in range.
            range.is_in_range(record.key())
        };

    let prev_unique_requirements = |record: RecordInList<'_>, position: &Option<Rc<Key>>| -> bool {
        // * If key is defined:
        if let Some(key) = &key {
            // * The record’s key is less than or equal to key.
            if !Key::less_than_or_equal(record.key(), key) {
                return false;
            }
        }

        // * If position is defined:
        if let Some(position) = position {
            // * The record’s key is less than position.
            if !Key::less_than(record.key(), position) {
                return false;
            }
        }

        // * The record’s key is in range.
        range.is_in_range(record.key())
    };

    let records_of_source = |search: &mut dyn FnMut(
        &mut dyn DoubleEndedIterator<Item = RecordInList<'_>>,
    ) -> Option<FoundRecord>|
     -> Option<FoundRecord> {
        match source {
            RecordSource::ObjectStore(object_store) => {
                let records = object_store.records();
                search(&mut records.iter().map(RecordInList::ObjectStore))
            }
            RecordSource::Index(index) => {
                let records = index.records();
                search(&mut records.iter().map(RecordInList::Index))
            }
        }
    };

    // 9. While count is greater than 0:
    let mut found_record: Option<FoundRecord> = None;
    while count > 0 {
        // 1. Switch on direction:
        match direction {
            IDBCursorDirection::Next => {
                // Let found record be the first record in records which satisfy all of the following requirements:
                found_record = records_of_source(&mut |mut records| {
                    Iterator::find(&mut records, |record| {
                        next_requirements(*record, &position, &object_store_position)
                    })
                    .map(RecordInList::to_found_record)
                });
            }
            IDBCursorDirection::Nextunique => {
                // Let found record be the first record in records which satisfy all of the following requirements:
                found_record = records_of_source(&mut |mut records| {
                    Iterator::find(&mut records, |record| next_unique_requirements(*record, &position))
                        .map(RecordInList::to_found_record)
                });
            }
            IDBCursorDirection::Prev => {
                // Let found record be the last record in records which satisfy all of the following requirements:
                found_record = records_of_source(&mut |records| {
                    records
                        .rev()
                        .find(|record| prev_requirements(*record, &position, &object_store_position))
                        .map(RecordInList::to_found_record)
                });
            }
            IDBCursorDirection::Prevunique => {
                // Let temp record be the last record in records which satisfy all of the following requirements:
                let temp_record = records_of_source(&mut |records| {
                    records
                        .rev()
                        .find(|record| prev_unique_requirements(*record, &position))
                        .map(RecordInList::to_found_record)
                });

                // If temp record is defined, let found record be the first record in records whose key is equal to temp record’s key.
                found_record = None;
                if let Some(temp_record) = temp_record {
                    let temp_record_key = temp_record.key().clone();

                    found_record = records_of_source(&mut |mut records| {
                        Iterator::find(&mut records, |content_record| {
                            Key::equals(content_record.key(), &temp_record_key)
                        })
                        .map(RecordInList::to_found_record)
                    });
                }
            }
        }

        // 2. If found record is not defined, then:
        let Some(found) = &found_record else {
            // 1. Set cursor’s key to undefined.
            cursor.set_key(None);

            // 2. If source is an index, set cursor’s object store position to undefined.
            if source_is_index {
                cursor.set_object_store_position(None);
            }

            // 3. If cursor’s key only flag is false, set cursor’s value to undefined.
            if !cursor.key_only() {
                cursor.set_value(Some(Value::UNDEFINED));
            }

            // 4. Return null.
            return Ok(None);
        };

        // 3. Let position be found record’s key.
        position = Some(found.key().clone());

        // 4. If source is an index, let object store position be found record’s value.
        if let FoundRecord::Index(record) = found {
            object_store_position = Some(record.value.clone());
        }

        // 5. Decrease count by 1.
        count -= 1;
    }
    let found_record = found_record.expect("a record was found");

    // 10. Set cursor’s position to position.
    cursor.set_position(position);

    // 11. If source is an index, set cursor’s object store position to object store position.
    if source_is_index {
        cursor.set_object_store_position(object_store_position);
    }

    // 12. Set cursor’s key to found record’s key.
    cursor.set_key(Some(found_record.key().clone()));

    // 13. If cursor’s key only flag is false, then:
    if !cursor.key_only() {
        // 1. Let serialized be found record’s value if source is an object store, or found record’s referenced value otherwise.
        let serialized = match (&found_record, source) {
            (FoundRecord::ObjectStore(record), _) => record.value.clone(),
            (FoundRecord::Index(record), RecordSource::Index(index)) => index.referenced_value(record),
            (FoundRecord::Index(_), RecordSource::ObjectStore(_)) => {
                unreachable!("a record of an index is found in an index")
            }
        };

        // 2. Set cursor’s value to ! StructuredDeserialize(serialized, targetRealm)
        cursor.set_value(Some(deserialize_a_stored_record(cx, &serialized)?));
    }

    // 14. Set cursor’s got value flag to true.
    cursor.set_got_value(true);

    // 15. Return cursor.
    Ok(Some(cursor))
}

// https://w3c.github.io/IndexedDB/#clear-an-object-store
pub(crate) fn clear_an_object_store(vm: &Vm, store: ObjectStoreRef) -> Value {
    // 1. Remove all records from store.
    store.clear_records(vm);

    // 2. In all indexes which reference store, remove all records.
    for index in store.indexes(vm).values().iter().copied() {
        Index::clear_records(index, vm);
    }

    // 3. Return undefined.
    Value::UNDEFINED
}

// https://w3c.github.io/IndexedDB/#retrieve-a-key-from-an-object-store
pub(crate) fn retrieve_a_key_from_an_object_store(
    cx: &CallContext,
    store: ObjectStoreRef,
    range: &IDBKeyRange,
) -> Value {
    // 1. Let record be the first record in store’s list of records whose key is in range, if any.
    let record = store.first_in_range(range);

    // 2. If record was not found, return undefined.
    let Some(record) = record else {
        return Value::UNDEFINED;
    };

    // 3. Return the result of converting a key to a value with record’s key.
    convert_a_key_to_a_value(cx, &record.key)
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum RecordKind {
    Key,
    Value,
    Record,
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-object-store
pub(crate) fn retrieve_multiple_items_from_an_object_store(
    cx: &CallContext,
    store: ObjectStoreRef,
    range: &IDBKeyRange,
    kind: RecordKind,
    direction: IDBCursorDirection,
    mut count: Option<u32>,
) -> WebIdlResult<Gc<Array>> {
    let vm = cx.vm();

    // 1. If count is not given or is 0 (zero), let count be infinity.
    if count == Some(0) {
        count = None;
    }

    // 2. Let records be an empty list of records.
    let mut records = Vec::new();

    // 3. If direction is "next" or "nextunique", set records to the first count of store’s list of records whose key is in range.
    if direction == IDBCursorDirection::Next || direction == IDBCursorDirection::Nextunique {
        records.extend(store.first_n_in_range(range, count));
    }

    // 4. If direction is "prev" or "prevunique", set records to the last count of store’s list of records whose key is in range.
    if direction == IDBCursorDirection::Prev || direction == IDBCursorDirection::Prevunique {
        records.extend(store.last_n_in_range(range, count));
    }

    // 5. Let list be an empty list.
    let list = Array::create(vm, cx.realm(), records.len() as u64, None).must();

    // 6. For each record of records, switching on kind:
    for (i, record) in records.iter().enumerate() {
        let property_key = PropertyKey::from_number(i as u64);
        match kind {
            RecordKind::Key => {
                // 1. Let key be the result of converting a key to a value with record’s key.
                let key = convert_a_key_to_a_value(cx, &record.key);

                // 2. Append key to list.
                list.create_data_property_or_throw(vm, &property_key, key).must();
            }
            RecordKind::Value => {
                // 1. Let serialized be record’s value.
                let serialized = &record.value;

                // 2. Let value be ! StructuredDeserialize(serialized, targetRealm).
                let entry = deserialize_a_stored_record(cx, serialized)?;

                // 3. Append value to list.
                list.create_data_property_or_throw(vm, &property_key, entry).must();
            }
            RecordKind::Record => {
                // 1. Let key be the record’s key.
                let key = record.key.clone();

                // 2. Let serialized be record’s value.
                let serialized = &record.value;

                // 3. Let value be ! StructuredDeserialize(serialized, targetRealm).
                let value = deserialize_a_stored_record(cx, serialized)?;

                // 4. Let recordSnapshot be a new record snapshot with its key set to key, value set to value, and primary key set to key.
                let record_snapshot = IDBRecord::create(vm, key.clone(), value, key);

                // 5. Append recordSnapshot to list.
                list.create_data_property_or_throw(vm, &property_key, record_snapshot.to_js(cx)?)
                    .must();
            }
        }
    }

    // 7. Return list.
    Ok(list)
}

// https://w3c.github.io/IndexedDB/#retrieve-a-referenced-value-from-an-index
pub(crate) fn retrieve_a_referenced_value_from_an_index(
    cx: &CallContext,
    index: IndexRef,
    range: &IDBKeyRange,
) -> WebIdlResult<Value> {
    // 1. Let record be the first record in index’s list of records whose key is in range, if any.
    let record = index.first_in_range(range);

    // 2. If record was not found, return undefined.
    let Some(record) = record else {
        return Ok(Value::UNDEFINED);
    };

    // 3. Let serialized be record’s referenced value.
    let serialized = index.referenced_value(&record);

    // 4. Return ! StructuredDeserialize(serialized, targetRealm).
    deserialize_a_stored_record(cx, &serialized)
}

// https://w3c.github.io/IndexedDB/#retrieve-a-value-from-an-index
pub(crate) fn retrieve_a_value_from_an_index(cx: &CallContext, index: IndexRef, range: &IDBKeyRange) -> Value {
    // 1. Let record be the first record in index’s list of records whose key is in range, if any.
    let record = index.first_in_range(range);

    // 2. If record was not found, return undefined.
    let Some(record) = record else {
        return Value::UNDEFINED;
    };

    // 3. Return the result of converting a key to a value with record’s value.
    convert_a_key_to_a_value(cx, &record.value)
}

// https://w3c.github.io/IndexedDB/#queue-a-database-task
pub(crate) fn queue_a_database_task(vm: &Vm, steps: Function) {
    // To queue a database task, perform queue a task on the database access task source.
    crate::indexed_db::cpp::queue_a_task_on_the_database_access_task_source(vm, steps);
}

// https://w3c.github.io/IndexedDB/#cleanup-indexed-database-transactions
pub(crate) fn cleanup_indexed_database_transactions(vm: &Vm, event_loop: &CppCell) -> bool {
    let mut has_matching_event_loop = false;

    Database::for_each_database(vm, |database| {
        for connection in database.associated_connections(vm).values().iter() {
            for transaction in connection.transactions(vm).values().iter() {
                let transaction = *transaction;

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

                    // FIXME: We skip committing if the transaction is waiting for prior writes.
                    //        Update the steps here text if this becomes explicit in the spec:
                    //         https://github.com/w3c/IndexedDB/issues/489#issuecomment-3994928473
                    if !transaction.request_list().execution_is_blocked() && transaction.request_list().is_empty() {
                        commit_a_transaction(vm, transaction);
                    }

                    // 2. Clear transaction’s cleanup event loop.
                    transaction.set_cleanup_event_loop(vm, None);
                }
            }
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

// https://w3c.github.io/IndexedDB/#retrieve-multiple-items-from-an-index
pub(crate) fn retrieve_multiple_items_from_an_index(
    target_cx: &CallContext,
    index: IndexRef,
    range: &IDBKeyRange,
    kind: RecordKind,
    direction: IDBCursorDirection,
    mut count: Option<u32>,
) -> WebIdlResult<Gc<Array>> {
    let vm = target_cx.vm();

    // 1. If count is not given or is 0 (zero), let count be infinity.
    if count == Some(0) {
        count = None;
    }

    // 2. Let records be an empty list of records.
    let mut records: Vec<IndexRecord> = Vec::new();

    // 3. Switching on direction:
    match direction {
        // "next"
        IDBCursorDirection::Next => {
            // 1. Set records to the first count of index’s list of records whose key is in range.
            records.extend(index.first_n_in_range(range, count));
        }
        // "nextunique"
        IDBCursorDirection::Nextunique => {
            // 1. Let rangeRecords be a list containing the index’s list of records whose key is in range.
            let range_records = index.first_n_in_range(range, None);

            // 2. Let rangeRecordsLength be rangeRecords’s size.
            let range_records_length = range_records.len();

            // 3. Let i be 0.
            let mut i = 0;

            // x. Append |rangeRecords[0]| to records.
            // FIXME: https://github.com/w3c/IndexedDB/issues/480
            if range_records_length > 0 {
                records.push(range_records[0].clone());
            }

            // 4. While i is less than rangeRecordsLength, then:
            while i + 1 < range_records_length {
                // 1. Increase i by 1.
                i += 1;

                // 2. if record’s size is equal to count, then break.
                if count.is_some_and(|count| records.len() == count as usize) {
                    break;
                }

                // 3. If the result of comparing two keys using the keys from |rangeRecords[i]| and |rangeRecords[i-1]| is equal, then continue.
                if Key::equals(&range_records[i].key, &range_records[i - 1].key) {
                    continue;
                }

                // 4. Else append |rangeRecords[i]| to records.
                records.push(range_records[i].clone());
            }
        }
        // "prev"
        IDBCursorDirection::Prev => {
            // 1. Set records to the last count of index’s list of records whose key is in range.
            records.extend(index.last_n_in_range(range, count));
        }
        // "prevunique"
        IDBCursorDirection::Prevunique => {
            // 1. Let rangeRecords be a list containing the index’s list of records whose key is in range.
            let range_records = index.first_n_in_range(range, None);

            // 2. Let rangeRecordsLength be rangeRecords’s size.
            let range_records_length = range_records.len();

            // 3. Let i be 0.
            let mut i = 0;

            // x. Append |rangeRecords[0]| to records.
            // FIXME: https://github.com/w3c/IndexedDB/issues/480
            if range_records_length > 0 {
                records.push(range_records[0].clone());
            }

            // 4. While i is less than rangeRecordsLength, then:
            while i + 1 < range_records_length {
                // 1. Increase i by 1.
                i += 1;

                // 2. if record’s size is equal to count, then break.
                if count.is_some_and(|count| records.len() == count as usize) {
                    break;
                }

                // 3. If the result of comparing two keys using the keys from |rangeRecords[i]| and |rangeRecords[i-1]| is equal, then continue.
                if Key::equals(&range_records[i].key, &range_records[i - 1].key) {
                    continue;
                }

                // 4. Else prepend |rangeRecords[i]| to records.
                records.insert(0, range_records[i].clone());
            }
        }
    }

    // 4. Let list be an empty list.
    let list = Array::create(vm, target_cx.realm(), records.len() as u64, None).must();

    // 5. For each record of records, switching on kind:
    for (i, record) in records.iter().enumerate() {
        let property_key = PropertyKey::from_number(i as u64);
        match kind {
            // "key"
            RecordKind::Key => {
                // 1. Let key be the result of converting a key to a value with record’s value.
                let key = convert_a_key_to_a_value(target_cx, &record.value);

                // 2. Append key to list.
                list.create_data_property_or_throw(vm, &property_key, key).must();
            }
            // "value"
            RecordKind::Value => {
                // 1. Let serialized be record’s referenced value.
                let serialized = index.referenced_value(record);

                // 2. Let value be ! StructuredDeserialize(serialized, targetRealm).
                let value = deserialize_a_stored_record(target_cx, &serialized)?;

                // 3. Append value to list.
                list.create_data_property_or_throw(vm, &property_key, value).must();
            }

            // "record"
            RecordKind::Record => {
                // 1. Let index key be the record’s key.
                let index_key = record.key.clone();

                // 2. Let key be the record’s value.
                let key = record.value.clone();

                // 3. Let serialized be record’s referenced value.
                let serialized = index.referenced_value(record);

                // 4. Let value be ! StructuredDeserialize(serialized, targetRealm).
                let value = deserialize_a_stored_record(target_cx, &serialized)?;

                // 5. Let recordSnapshot be a new record snapshot with its key set to index key, value set to value, and primary key set to key.
                let record_snapshot = IDBRecord::create(vm, index_key, value, key);

                // 6. Append recordSnapshot to list.
                list.create_data_property_or_throw(vm, &property_key, record_snapshot.to_js(target_cx)?)
                    .must();
            }
        }
    }

    // 6. Return list.
    Ok(list)
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

fn retrieve_multiple_items_operation(
    vm: &Vm,
    realm: Gc<Realm>,
    source: RecordSource,
    range: Ref<IDBKeyRange>,
    kind: RecordKind,
    direction: IDBCursorDirection,
    count: Option<u32>,
) -> Operation {
    match source {
        // 11. If source is an index, set operation to retrieve multiple items from an index with targetRealm, source, range, kind, direction, and count if given.
        RecordSource::Index(index) => create_operation(
            vm,
            (realm, index, range, NoCells(kind), NoCells(direction), count),
            |vm, &(realm, index, range, ref kind, ref direction, count)| {
                let cx = CallContext::new(vm, realm);
                Ok(Value::from_object(retrieve_multiple_items_from_an_index(
                    &cx,
                    index,
                    &range,
                    kind.0,
                    direction.0,
                    count,
                )?))
            },
        ),
        // 12. Else set operation to retrieve multiple items from an object store with targetRealm, source, range, kind, direction, and count if given.
        RecordSource::ObjectStore(object_store) => create_operation(
            vm,
            (realm, object_store, range, NoCells(kind), NoCells(direction), count),
            |vm, &(realm, object_store, range, ref kind, ref direction, count)| {
                let cx = CallContext::new(vm, realm);
                Ok(Value::from_object(retrieve_multiple_items_from_an_object_store(
                    &cx,
                    object_store,
                    &range,
                    kind.0,
                    direction.0,
                    count,
                )?))
            },
        ),
    }
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
    let operation = retrieve_multiple_items_operation(vm, cx.realm(), source, range, kind, direction, count);

    // 13. Return the result (an IDBRequest) of running asynchronously execute a request with sourceHandle and operation.
    Ok(asynchronously_execute_a_request(vm, source_handle, operation, None))
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
    let operation =
        retrieve_multiple_items_operation(vm, cx.realm(), source, range, kind, options.direction, options.count);

    // 13. Return the result (an IDBRequest) of running asynchronously execute a request with sourceHandle and operation.
    Ok(asynchronously_execute_a_request(vm, source_handle, operation, None))
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
