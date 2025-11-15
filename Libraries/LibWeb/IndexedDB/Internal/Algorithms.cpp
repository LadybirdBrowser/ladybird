/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/IndexedDB/IDBCursor.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBIndex.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/IDBRecord.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/IDBVersionChangeEvent.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/ConnectionQueueHandler.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/IndexedDB/Internal/Index.h>
#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::IndexedDB {

#if defined(AK_COMPILER_CLANG)
#    define MAX_KEY_GENERATOR_VALUE AK::exp2(53.)
#else
constexpr double const MAX_KEY_GENERATOR_VALUE { __builtin_exp2(53) };
#endif

struct TaskCounterState final : public GC::Cell {
    GC_CELL(TaskCounterState, GC::Cell);
    GC_DECLARE_ALLOCATOR(TaskCounterState);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(after_all);
    }

    void decrement_remaining_tasks()
    {
        VERIFY(remaining_tasks > 0);
        --remaining_tasks;
        if (remaining_tasks > 0)
            return;

        queue_a_database_task(after_all.as_nonnull());
    }

    size_t remaining_tasks { 0 };
    GC::Ptr<GC::Function<void()>> after_all;
};

GC_DEFINE_ALLOCATOR(TaskCounterState);

// https://w3c.github.io/IndexedDB/#open-a-database-connection
void open_a_database_connection(JS::Realm& realm, StorageAPI::StorageKey storage_key, String name, Optional<u64> maybe_version, GC::Ref<IDBRequest> request, GC::Ref<GC::Function<void(WebIDL::ExceptionOr<GC::Ref<IDBDatabase>>)>> on_complete)
{
    // 1. Let queue be the connection queue for storageKey and name.
    auto& queue = ConnectionQueueHandler::for_key_and_name(storage_key, name);

    // 2. Add request to queue.
    queue.append(request);
    dbgln_if(IDB_DEBUG, "open_a_database_connection: added request {} to queue", request->uuid());

    // 3. Wait until all previous requests in queue have been processed.
    if constexpr (IDB_DEBUG) {
        dbgln("open_a_database_connection: waiting for step 3");
        dbgln("requests in queue:");
        for (auto const& item : queue) {
            dbgln("[{}] - {} = {}", item == request ? "x"sv : " "sv, item->uuid(), item->processed() ? "processed"sv : "not processed"sv);
        }
    }

    queue.all_previous_requests_processed(realm.heap(), request, GC::create_function(realm.heap(), [&realm, storage_key = move(storage_key), name = move(name), maybe_version = move(maybe_version), request, on_complete] -> void {
        // 4. Let db be the database named name in storageKey, or null otherwise.
        GC::Ptr<Database> db;
        auto maybe_db = Database::for_key_and_name(storage_key, name);
        if (maybe_db.has_value()) {
            db = maybe_db.value();
        }

        // 5. If version is undefined, let version be 1 if db is null, or db’s version otherwise.
        auto version = maybe_version.value_or(maybe_db.has_value() ? maybe_db.value()->version() : 1);

        // 6. If db is null, let db be a new database with name name, version 0 (zero), and with no object stores.
        // If this fails for any reason, return an appropriate error (e.g. a "QuotaExceededError" or "UnknownError" DOMException).
        if (!maybe_db.has_value()) {
            auto maybe_database = Database::create_for_key_and_name(realm, storage_key, name);

            if (maybe_database.is_error()) {
                on_complete->function()(WebIDL::OperationError::create(realm, "Unable to create a new database"_utf16));
                return;
            }

            db = maybe_database.release_value();
        }

        // 7. If db’s version is greater than version, return a newly created "VersionError" DOMException and abort these steps.
        if (db->version() > version) {
            on_complete->function()(WebIDL::VersionError::create(realm, "Database version is greater than the requested version"_utf16));
            return;
        }

        // 8. Let connection be a new connection to db.
        auto connection = IDBDatabase::create(realm, *db);
        dbgln_if(IDB_DEBUG, "Created new connection with UUID: {}", connection->uuid());

        // 9. Set connection’s version to version.
        connection->set_version(version);

        // 10. If db’s version is less than version, then:
        if (db->version() < version) {
            dbgln_if(IDB_DEBUG, "open_a_database_connection: Upgrading database from version {} to {}", db->version(), version);

            // 1. Let openConnections be the set of all connections, except connection, associated with db.
            auto open_connections = db->associated_connections_except(connection);

            // 2. For each entry of openConnections that does not have its close pending flag set to true,
            //    queue a database task to fire a version change event named versionchange at entry with db’s version and version.
            GC::Ptr<TaskCounterState> task_counter_state;
            for (auto const& entry : open_connections) {
                if (!entry->close_pending()) {
                    if (!task_counter_state) {
                        task_counter_state = realm.heap().allocate<TaskCounterState>();
                    }

                    task_counter_state->remaining_tasks++;
                    queue_a_database_task(GC::create_function(realm.vm().heap(), [&realm, entry, db, version, task_counter_state] {
                        fire_a_version_change_event(realm, HTML::EventNames::versionchange, *entry, db->version(), version);
                        task_counter_state->decrement_remaining_tasks();
                    }));
                }
            }

            // 3. Wait for all of the events to be fired.
            if constexpr (IDB_DEBUG) {
                dbgln("open_a_database_connection: waiting for step 10.3");
                dbgln("remaining tasks: {}", task_counter_state ? task_counter_state->remaining_tasks : 0);
            }

            auto after_all = GC::create_function(realm.heap(), [&realm, open_connections = move(open_connections), db, version, connection, request, on_complete] {
                // 4. If any of the connections in openConnections are still not closed,
                //    queue a database task to fire a version change event named blocked at request with db’s version and version.
                for (auto const& entry : open_connections) {
                    if (entry->state() != ConnectionState::Closed) {
                        queue_a_database_task(GC::create_function(realm.vm().heap(), [&realm, entry, db, version]() {
                            fire_a_version_change_event(realm, HTML::EventNames::blocked, *entry, db->version(), version);
                        }));
                    }
                }

                // 5. Wait until all connections in openConnections are closed.
                if constexpr (IDB_DEBUG) {
                    dbgln("open_a_database_connection: waiting for step 10.5");
                    dbgln("open connections: {}", open_connections.size());
                    for (auto const& open_connection : open_connections) {
                        dbgln("  - {}", open_connection->uuid());
                    }
                }

                db->wait_for_connections_to_close(open_connections, GC::create_function(realm.heap(), [&realm, connection, version, request, on_complete] {
                    dbgln_if(IDB_DEBUG, "open_a_database_connection: finished waiting for step 10.5");

                    // 6. Run upgrade a database using connection, version and request.
                    dbgln_if(IDB_DEBUG, "open_a_database_connection: waiting for step 10.6");
                    upgrade_a_database(realm, connection, version, request, GC::create_function(realm.heap(), [&realm, connection, request, on_complete] {
                        dbgln_if(IDB_DEBUG, "open_a_database_connection: finished waiting for step 10.6");

                        // 7. If connection was closed, return a newly created "AbortError" DOMException and abort these steps.
                        if (connection->state() == ConnectionState::Closed) {
                            dbgln_if(IDB_DEBUG, "open_a_database_connection: step 10.7: connection was closed, aborting");
                            on_complete->function()(WebIDL::AbortError::create(realm, "Connection was closed"_utf16));
                            return;
                        }

                        // 8. If request's error is set, run the steps to close a database connection with connection,
                        //    return a newly created "AbortError" DOMException and abort these steps.
                        if (request->has_error()) {
                            dbgln_if(IDB_DEBUG, "open_a_database_connection: step 10.8: request errored, waiting to close connection");
                            close_a_database_connection(*connection, GC::create_function(realm.heap(), [&realm, on_complete] {
                                dbgln_if(IDB_DEBUG, "open_a_database_connection: step 10.8: connection closed, aborting");
                                on_complete->function()(WebIDL::AbortError::create(realm, "Upgrade transaction was aborted"_utf16));
                            }));
                            return;
                        }

                        // 11. Return connection.
                        dbgln_if(IDB_DEBUG, "open_a_database_connection: step 11: successfully upgraded database, completing with new connection");
                        on_complete->function()(connection);
                    }));
                }));
            });

            if (task_counter_state) {
                task_counter_state->after_all = after_all;
            } else {
                queue_a_database_task(after_all);
            }

            // NOTE: Because of the async nature of this function, we return here and call the on_complete function
            //       with the connection when necessary.
            return;
        }

        // 11. Return connection.
        dbgln_if(IDB_DEBUG, "open_a_database_connection: step 11: no upgrade required, completing with new connection");
        on_complete->function()(connection);
    }));
}

bool fire_a_version_change_event(JS::Realm& realm, FlyString const& event_name, GC::Ref<DOM::EventTarget> target, u64 old_version, Optional<u64> new_version)
{
    IDBVersionChangeEventInit event_init = {};
    // 4. Set event’s oldVersion attribute to oldVersion.
    event_init.old_version = old_version;
    // 5. Set event’s newVersion attribute to newVersion.
    event_init.new_version = new_version;

    // 1. Let event be the result of creating an event using IDBVersionChangeEvent.
    // 2. Set event’s type attribute to e.
    auto event = IDBVersionChangeEvent::create(realm, event_name, event_init);

    // 3. Set event’s bubbles and cancelable attributes to false.
    event->set_bubbles(false);
    event->set_cancelable(false);

    // 6. Let legacyOutputDidListenersThrowFlag be false.
    auto legacy_output_did_listeners_throw_flag = false;

    // 7. Dispatch event at target with legacyOutputDidListenersThrowFlag.
    DOM::EventDispatcher::dispatch(target, *event, false, legacy_output_did_listeners_throw_flag);

    // 8. Return legacyOutputDidListenersThrowFlag.
    return legacy_output_did_listeners_throw_flag;
}

// https://w3c.github.io/IndexedDB/#convert-value-to-key
WebIDL::ExceptionOr<GC::Ref<Key>> convert_a_value_to_a_key(JS::Realm& realm, JS::Value input, Vector<JS::Value> seen)
{
    // 1. If seen was not given, then let seen be a new empty set.
    // NOTE: This is handled by the caller.

    // 2. If seen contains input, then return invalid.
    if (seen.contains_slow(input))
        return Key::create_invalid(realm, "Already seen key"_string);

    // 3. Jump to the appropriate step below:

    // - If Type(input) is Number
    if (input.is_number()) {

        // 1. If input is NaN then return invalid.
        if (input.is_nan())
            return Key::create_invalid(realm, "NaN key"_string);

        // 2. Otherwise, return a new key with type number and value input.
        return Key::create_number(realm, input.as_double());
    }

    // - If input is a Date (has a [[DateValue]] internal slot)
    if (input.is_object() && is<JS::Date>(input.as_object())) {

        // 1. Let ms be the value of input’s [[DateValue]] internal slot.
        auto& date = static_cast<JS::Date&>(input.as_object());
        auto ms = date.date_value();

        // 2. If ms is NaN then return invalid.
        if (isnan(ms))
            return Key::create_invalid(realm, "NaN key"_string);

        // 3. Otherwise, return a new key with type date and value ms.
        return Key::create_date(realm, ms);
    }

    // - If Type(input) is String
    if (input.is_string()) {

        // 1. Return a new key with type string and value input.
        return Key::create_string(realm, input.as_string().utf8_string());
    }

    // - If input is a buffer source type
    if (input.is_object() && (is<JS::TypedArrayBase>(input.as_object()) || is<JS::ArrayBuffer>(input.as_object()) || is<JS::DataView>(input.as_object()))) {

        // 1. If input is detached then return invalid.
        if (WebIDL::is_buffer_source_detached(input))
            return Key::create_invalid(realm, "Detached buffer is not supported as key"_string);

        // 2. Let bytes be the result of getting a copy of the bytes held by the buffer source input.
        auto data_buffer = MUST(WebIDL::get_buffer_source_copy(input.as_object()));

        // 3. Return a new key with type binary and value bytes.
        return Key::create_binary(realm, data_buffer);
    }

    // - If input is an Array exotic object
    if (input.is_object() && is<JS::Array>(input.as_object())) {

        // 1. Let len be ? ToLength( ? Get(input, "length")).
        auto length = TRY(length_of_array_like(realm.vm(), input.as_object()));

        // 2. Append input to seen.
        seen.append(input);

        // 3. Let keys be a new empty list.
        Vector<GC::Root<Key>> keys;

        // 4. Let index be 0.
        u64 index = 0;

        // 5. While index is less than len:
        while (index < length) {
            // 1. Let hop be ? HasOwnProperty(input, index).
            auto hop = TRY(input.as_object().has_own_property(index));

            // 2. If hop is false, return invalid.
            if (!hop)
                return Key::create_invalid(realm, "Array-like object has no property"_string);

            // 3. Let entry be ? Get(input, index).
            auto entry = TRY(input.as_object().get(index));

            // 4. Let key be the result of converting a value to a key with arguments entry and seen.
            // 5. ReturnIfAbrupt(key).
            auto key = TRY(convert_a_value_to_a_key(realm, entry, seen));

            // 6. If key is invalid abort these steps and return invalid.
            if (key->is_invalid())
                return key;

            // 7. Append key to keys.
            keys.append(key);

            // 8. Increase index by 1.
            index++;
        }

        // 6. Return a new array key with value keys.
        return Key::create_array(realm, keys);
    }

    // - Otherwise
    // Return invalid.
    return Key::create_invalid(realm, "Unable to convert value to key. Its not of a known type"_string);
}

// https://w3c.github.io/IndexedDB/#close-a-database-connection
void close_a_database_connection(GC::Ref<IDBDatabase> connection, GC::Ptr<GC::Function<void()>> on_complete, bool forced)
{
    auto& realm = connection->realm();

    // 1. Set connection’s close pending flag to true.
    connection->set_close_pending(true);

    // 2. If the forced flag is true, then for each transaction created using connection run abort a transaction with transaction and newly created "AbortError" DOMException.
    if (forced) {
        for (auto const& transaction : connection->transactions()) {
            abort_a_transaction(*transaction, WebIDL::AbortError::create(realm, "Connection was closed"_utf16));
        }
    }

    // 3. Wait for all transactions created using connection to complete. Once they are complete, connection is closed.
    if constexpr (IDB_DEBUG) {
        dbgln("close_a_database_connection: waiting for step 3");
        dbgln("transactions created using connection:");
        for (auto const& transaction : connection->transactions()) {
            dbgln("  - {} - {}", transaction->uuid(), (u8)transaction->state());
        }
    }

    connection->wait_for_transactions_to_finish(connection->transactions(), GC::create_function(realm.heap(), [&realm, connection, forced, on_complete] {
        dbgln_if(IDB_DEBUG, "close_a_database_connection: finished waiting for step 3, closing database connection");
        connection->set_state(ConnectionState::Closed);

        // 4. If the forced flag is true, then fire an event named close at connection.
        if (forced)
            connection->dispatch_event(DOM::Event::create(realm, HTML::EventNames::close));

        if (on_complete)
            queue_a_database_task(on_complete.as_nonnull());
    }));
}

// https://w3c.github.io/IndexedDB/#upgrade-a-database
void upgrade_a_database(JS::Realm& realm, GC::Ref<IDBDatabase> connection, u64 version, GC::Ref<IDBRequest> request, GC::Ref<GC::Function<void()>> on_complete)
{
    // 1. Let db be connection’s database.
    auto db = connection->associated_database();

    // 2. Let transaction be a new upgrade transaction with connection used as connection.
    // 3. Set transaction’s scope to connection’s object store set.
    auto transaction = IDBTransaction::create(realm, connection, Bindings::IDBTransactionMode::Versionchange, Bindings::IDBTransactionDurability::Default, Vector<GC::Ref<ObjectStore>> { connection->object_store_set() });
    dbgln_if(IDB_DEBUG, "Created new upgrade transaction with UUID: {}", transaction->uuid());

    // 4. Set db’s upgrade transaction to transaction.
    db->set_upgrade_transaction(transaction);

    // 5. Set transaction’s state to inactive.
    transaction->set_state(IDBTransaction::TransactionState::Inactive);

    // FIXME: 6. Start transaction.

    // 7. Let old version be db’s version.
    auto old_version = db->version();

    // 8. Set db’s version to version. This change is considered part of the transaction, and so if the transaction is aborted, this change is reverted.
    db->set_version(version);

    // 9. Set request’s processed flag to true.
    request->set_processed(true);

    // 10. Queue a database task to run these steps:
    queue_a_database_task(GC::create_function(realm.vm().heap(), [&realm, request, connection, transaction, old_version, version]() {
        // 1. Set request’s result to connection.
        request->set_result(connection);

        // 2. Set request’s transaction to transaction.
        // NOTE: We need to do a two-way binding here.
        request->set_transaction(transaction);
        transaction->set_associated_request(request);

        // 3. Set request’s done flag to true.
        request->set_done(true);

        // 4. Set transaction’s state to active.
        transaction->set_state(IDBTransaction::TransactionState::Active);

        // 5. Let didThrow be the result of firing a version change event named upgradeneeded at request with old version and version.
        auto did_throw = fire_a_version_change_event(realm, HTML::EventNames::upgradeneeded, request, old_version, version);

        // 6. If transaction’s state is active, then:
        if (transaction->state() == IDBTransaction::TransactionState::Active) {

            // 1. Set transaction’s state to inactive.
            transaction->set_state(IDBTransaction::TransactionState::Inactive);

            // 2. If didThrow is true, run abort a transaction with transaction and a newly created "AbortError" DOMException.
            if (did_throw)
                abort_a_transaction(transaction, WebIDL::AbortError::create(realm, "Version change event threw an exception"_utf16));

            // AD-HOC:
            // The implementation must attempt to commit a transaction when all requests placed against the transaction have completed
            // and their returned results handled,
            // no new requests have been placed against the transaction,
            // and the transaction has not been aborted.
            if (transaction->state() == IDBTransaction::TransactionState::Inactive && transaction->request_list().is_empty() && !transaction->aborted())
                commit_a_transaction(realm, transaction);
        }
    }));

    // 11. Wait for transaction to finish.
    dbgln_if(IDB_DEBUG, "upgrade_a_database: waiting for step 11");
    connection->wait_for_transactions_to_finish({ &transaction, 1 }, GC::create_function(realm.heap(), [on_complete] {
        dbgln_if(IDB_DEBUG, "upgrade_a_database: finished waiting for step 11, queuing completion task");
        queue_a_database_task(on_complete);
    }));
}

// https://w3c.github.io/IndexedDB/#deleting-a-database
void delete_a_database(JS::Realm& realm, StorageAPI::StorageKey storage_key, String name, GC::Ref<IDBRequest> request, GC::Ref<GC::Function<void(WebIDL::ExceptionOr<u64>)>> on_complete)
{
    // 1. Let queue be the connection queue for storageKey and name.
    auto& queue = ConnectionQueueHandler::for_key_and_name(storage_key, name);

    // 2. Add request to queue.
    queue.append(request);
    dbgln_if(IDB_DEBUG, "delete_a_database: added request {} to queue", request->uuid());

    // 3. Wait until all previous requests in queue have been processed.
    if constexpr (IDB_DEBUG) {
        dbgln("delete_a_database: waiting for step 3");
        dbgln("requests in queue:");
        for (auto const& item : queue) {
            dbgln("[{}] - {} = {}", item == request ? "x"sv : " "sv, item->uuid(), item->processed() ? "processed"sv : "not processed"sv);
        }
    }

    queue.all_previous_requests_processed(realm.heap(), request, GC::create_function(realm.heap(), [&realm, storage_key = move(storage_key), name = move(name), on_complete] -> void {
        // 4. Let db be the database named name in storageKey, if one exists. Otherwise, return 0 (zero).
        auto maybe_db = Database::for_key_and_name(storage_key, name);
        if (!maybe_db.has_value()) {
            on_complete->function()(0);
            return;
        }

        auto db = maybe_db.value();

        // 5. Let openConnections be the set of all connections associated with db.
        auto open_connections = db->associated_connections();

        // 6. For each entry of openConnections that does not have its close pending flag set to true,
        //    queue a database task to fire a version change event named versionchange at entry with db’s version and null.
        GC::Ptr<TaskCounterState> task_counter_state;
        for (auto const& entry : open_connections) {
            if (!entry->close_pending()) {
                if (!task_counter_state) {
                    task_counter_state = realm.heap().allocate<TaskCounterState>();
                }

                task_counter_state->remaining_tasks++;
                queue_a_database_task(GC::create_function(realm.vm().heap(), [&realm, entry, db, task_counter_state] {
                    fire_a_version_change_event(realm, HTML::EventNames::versionchange, *entry, db->version(), {});
                    task_counter_state->decrement_remaining_tasks();
                }));
            }
        }

        // 7. Wait for all of the events to be fired.
        if constexpr (IDB_DEBUG) {
            dbgln("delete_a_database: waiting for step 7");
            dbgln("remaining tasks: {}", task_counter_state ? task_counter_state->remaining_tasks : 0);
        }

        auto after_all = GC::create_function(realm.heap(), [&realm, open_connections, db, storage_key = move(storage_key), name = move(name), on_complete] {
            // 8. If any of the connections in openConnections are still not closed, queue a database task to fire a version change event named blocked at request with db’s version and null.
            for (auto const& entry : open_connections) {
                if (entry->state() != ConnectionState::Closed) {
                    queue_a_database_task(GC::create_function(realm.vm().heap(), [&realm, entry, db]() {
                        fire_a_version_change_event(realm, HTML::EventNames::blocked, *entry, db->version(), {});
                    }));
                }
            }

            // 9. Wait until all connections in openConnections are closed.
            if constexpr (IDB_DEBUG) {
                dbgln("delete_a_database: waiting for step 9");
                dbgln("open connections: {}", open_connections.size());
                for (auto const& connection : open_connections) {
                    dbgln("  - {}", connection->uuid());
                }
            }

            db->wait_for_connections_to_close(open_connections, GC::create_function(realm.heap(), [&realm, db, storage_key = move(storage_key), name = move(name), on_complete] {
                // 10. Let version be db’s version.
                auto version = db->version();

                // 11. Delete db. If this fails for any reason, return an appropriate error (e.g. "QuotaExceededError" or "UnknownError" DOMException).
                auto maybe_deleted = Database::delete_for_key_and_name(storage_key, name);
                if (maybe_deleted.is_error()) {
                    on_complete->function()(WebIDL::OperationError::create(realm, "Unable to delete database"_utf16));
                    return;
                }

                // 12. Return version.
                on_complete->function()(version);
            }));
        });

        if (task_counter_state) {
            task_counter_state->after_all = after_all;
        } else {
            queue_a_database_task(after_all);
        }
    }));
}

// https://w3c.github.io/IndexedDB/#abort-a-transaction
void abort_a_transaction(GC::Ref<IDBTransaction> transaction, GC::Ptr<WebIDL::DOMException> error)
{
    // NOTE: This is not spec'ed anywhere, but we need to know IF the transaction was aborted.
    transaction->set_aborted(true);
    dbgln_if(IDB_DEBUG, "abort_a_transaction: transaction {} is aborting", transaction->uuid());

    // 1. If transaction is finished, abort these steps.
    if (transaction->is_finished())
        return;

    // FIXME: 2. All the changes made to the database by the transaction are reverted.
    // For upgrade transactions this includes changes to the set of object stores and indexes, as well as the change to the version.
    // Any object stores and indexes which were created during the transaction are now considered deleted for the purposes of other algorithms.

    // FIXME: 3. If transaction is an upgrade transaction, run the steps to abort an upgrade transaction with transaction.
    // if (transaction.is_upgrade_transaction())
    //     abort_an_upgrade_transaction(transaction);

    // 4. Set transaction’s state to finished.
    transaction->set_state(IDBTransaction::TransactionState::Finished);

    // 5. Set transaction’s error to error.
    transaction->set_error(error);

    // FIXME: https://github.com/w3c/IndexedDB/issues/473
    // x. If transaction is an upgrade transaction:
    if (transaction->is_upgrade_transaction()) {
        // 1. Set transaction's associated request's error to a newly created "AbortError" DOMException.
        transaction->associated_request()->set_error(WebIDL::AbortError::create(transaction->realm(), "Upgrade transaction was aborted"_utf16));
    }

    // 6. For each request of transaction’s request list,
    for (auto const& request : transaction->request_list()) {
        // FIXME: abort the steps to asynchronously execute a request for request,

        // set request’s processed flag to true
        request->set_processed(true);

        // and queue a database task to run these steps:
        queue_a_database_task(GC::create_function(transaction->realm().vm().heap(), [request]() {
            // 1. Set request’s done flag to true.
            request->set_done(true);

            // 2. Set request’s result to undefined.
            request->set_result(JS::js_undefined());

            // 3. Set request’s error to a newly created "AbortError" DOMException.
            request->set_error(WebIDL::AbortError::create(request->realm(), "Transaction was aborted"_utf16));

            // 4. Fire an event named error at request with its bubbles and cancelable attributes initialized to true.
            request->dispatch_event(DOM::Event::create(request->realm(), HTML::EventNames::error, { .bubbles = true, .cancelable = true }));
        }));
    }

    // 7. Queue a database task to run these steps:
    queue_a_database_task(GC::create_function(transaction->realm().vm().heap(), [transaction]() {
        // 1. If transaction is an upgrade transaction, then set transaction’s connection's associated database's upgrade transaction to null.
        if (transaction->is_upgrade_transaction())
            transaction->connection()->associated_database()->set_upgrade_transaction(nullptr);

        // 2. Fire an event named abort at transaction with its bubbles attribute initialized to true.
        transaction->dispatch_event(DOM::Event::create(transaction->realm(), HTML::EventNames::abort, { .bubbles = true }));

        // 3. If transaction is an upgrade transaction, then:
        if (transaction->is_upgrade_transaction()) {
            // 1. Let request be the open request associated with transaction.
            auto request = transaction->associated_request();

            // 2. Set request’s transaction to null.
            // NOTE: Clear the two-way binding.
            request->set_transaction(nullptr);
            transaction->set_associated_request(nullptr);

            // 3. Set request’s result to undefined.
            request->set_result(JS::js_undefined());

            // 4. Set request’s processed flag to false.
            // FIXME: request->set_processed(false);

            // 5. Set request’s done flag to false.
            request->set_done(false);
        }
    }));
}

// https://w3c.github.io/IndexedDB/#convert-a-key-to-a-value
JS::Value convert_a_key_to_a_value(JS::Realm& realm, GC::Ref<Key> key)
{
    // 1. Let type be key’s type.
    auto type = key->type();

    // 2. Let value be key’s value.
    auto value = key->value();

    // 3. Switch on type:
    switch (type) {
    case Key::KeyType::Number: {
        // Return an ECMAScript Number value equal to value
        return JS::Value(key->value_as_double());
    }

    case Key::KeyType::String: {
        // Return an ECMAScript String value equal to value
        return JS::PrimitiveString::create(realm.vm(), key->value_as_string());
    }

    case Key::KeyType::Date: {
        // 1. Let date be the result of executing the ECMAScript Date constructor with the single argument value.
        auto date = JS::Date::create(realm, key->value_as_double());

        // 2. Assert: date is not an abrupt completion.
        // NOTE: This is not possible in our implementation.

        // 3. Return date.
        return date;
    }

    case Key::KeyType::Binary: {
        auto buffer = key->value_as_byte_buffer();

        // 1. Let len be value’s length.
        auto len = buffer.size();

        // 2. Let buffer be the result of executing the ECMAScript ArrayBuffer constructor with len.
        // 3. Assert: buffer is not an abrupt completion.
        auto array_buffer = MUST(JS::ArrayBuffer::create(realm, len));

        // 4. Set the entries in buffer’s [[ArrayBufferData]] internal slot to the entries in value.
        buffer.span().copy_to(array_buffer->buffer());

        // 5. Return buffer.
        return array_buffer;
    }

    case Key::KeyType::Array: {
        auto data = key->value_as_vector();

        // 1. Let array be the result of executing the ECMAScript Array constructor with no arguments.
        // 2. Assert: array is not an abrupt completion.
        auto array = MUST(JS::Array::create(realm, 0));

        // 3. Let len be value’s size.
        auto len = data.size();

        // 4. Let index be 0.
        u64 index = 0;

        // 5. While index is less than len:
        while (index < len) {
            // 1. Let entry be the result of converting a key to a value with value[index].
            auto entry = convert_a_key_to_a_value(realm, *data[index]);

            // 2. Let status be CreateDataProperty(array, index, entry).
            auto status = MUST(array->create_data_property(index, entry));

            // 3. Assert: status is true.
            VERIFY(status);

            // 4. Increase index by 1.
            index++;
        }

        // 6. Return array.
        return array;
    }
    case Key::KeyType::Invalid:
        VERIFY_NOT_REACHED();
    }

    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/IndexedDB/#valid-key-path
bool is_valid_key_path(KeyPath const& path)
{
    // A valid key path is one of:
    return path.visit(
        [](String const& value) -> bool {
            // * An empty string.
            if (value.is_empty())
                return true;

            // FIXME: * An identifier, which is a string matching the IdentifierName production from the ECMAScript Language Specification [ECMA-262].
            return true;

            // FIXME: * A string consisting of two or more identifiers separated by periods (U+002E FULL STOP).
            return true;

            return false;
        },
        [](Vector<String> const& values) -> bool {
            // * A non-empty list containing only strings conforming to the above requirements.
            if (values.is_empty())
                return false;

            for (auto const& value : values) {
                if (!is_valid_key_path(value))
                    return false;
            }

            return true;
        });
}

// https://w3c.github.io/IndexedDB/#create-a-sorted-name-list
GC::Ref<HTML::DOMStringList> create_a_sorted_name_list(JS::Realm& realm, Vector<String> names)
{
    // 1. Let sorted be names sorted in ascending order with the code unit less than algorithm.
    quick_sort(names, [](auto const& a, auto const& b) {
        return Infra::code_unit_less_than(a, b);
    });

    // 2. Return a new DOMStringList associated with sorted.
    return HTML::DOMStringList::create(realm, names);
}

// https://w3c.github.io/IndexedDB/#commit-a-transaction
void commit_a_transaction(JS::Realm& realm, GC::Ref<IDBTransaction> transaction)
{
    // 1. Set transaction’s state to committing.
    transaction->set_state(IDBTransaction::TransactionState::Committing);

    dbgln_if(IDB_DEBUG, "commit_a_transaction: transaction {} is committing", transaction->uuid());

    // 2. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, transaction]() {
        // 1. Wait until every item in transaction’s request list is processed.
        if constexpr (IDB_DEBUG) {
            dbgln("commit_a_transaction: waiting for step 1");
            dbgln("requests in queue:");
            for (auto const& request : transaction->request_list()) {
                dbgln("  - {} = {}", request->uuid(), request->processed() ? "processed"sv : "not processed"sv);
            }
        }

        transaction->request_list().all_requests_processed(realm.heap(), GC::create_function(realm.heap(), [transaction] {
            // 2. If transaction’s state is no longer committing, then terminate these steps.
            if (transaction->state() != IDBTransaction::TransactionState::Committing)
                return;

            // FIXME: 3. Attempt to write any outstanding changes made by transaction to the database, considering transaction’s durability hint.
            // FIXME: 4. If an error occurs while writing the changes to the database, then run abort a transaction with transaction and an appropriate type for the error, for example "QuotaExceededError" or "UnknownError" DOMException, and terminate these steps.

            // 5. Queue a database task to run these steps:
            queue_a_database_task(GC::create_function(transaction->realm().vm().heap(), [transaction]() {
                // 1. If transaction is an upgrade transaction, then set transaction’s connection's associated database's upgrade transaction to null.
                if (transaction->is_upgrade_transaction())
                    transaction->connection()->associated_database()->set_upgrade_transaction(nullptr);

                // 2. Set transaction’s state to finished.
                transaction->set_state(IDBTransaction::TransactionState::Finished);

                // 3. Fire an event named complete at transaction.
                transaction->dispatch_event(DOM::Event::create(transaction->realm(), HTML::EventNames::complete));

                // 4. If transaction is an upgrade transaction, then let request be the request associated with transaction and set request’s transaction to null.
                if (transaction->is_upgrade_transaction()) {
                    auto request = transaction->associated_request();
                    request->set_transaction(nullptr);

                    // Ad-hoc: Clear the two-way binding.
                    transaction->set_associated_request(nullptr);
                }
            }));
        }));
    }));
}

// https://w3c.github.io/IndexedDB/#clone
WebIDL::ExceptionOr<JS::Value> clone_in_realm(JS::Realm& target_realm, JS::Value value, GC::Ref<IDBTransaction> transaction)
{
    auto& vm = target_realm.vm();

    // 1. Assert: transaction’s state is active.
    VERIFY(transaction->state() == IDBTransaction::TransactionState::Active);

    // 2. Set transaction’s state to inactive.
    transaction->set_state(IDBTransaction::TransactionState::Inactive);

    // 3. Let serialized be ? StructuredSerializeForStorage(value).
    auto serialized = TRY(HTML::structured_serialize_for_storage(vm, value));

    // 4. Let clone be ? StructuredDeserialize(serialized, targetRealm).
    auto clone = TRY(HTML::structured_deserialize(vm, serialized, target_realm));

    // 5. Set transaction’s state to active.
    transaction->set_state(IDBTransaction::TransactionState::Active);

    // 6. Return clone.
    return clone;
}

// https://w3c.github.io/IndexedDB/#convert-a-value-to-a-multientry-key
WebIDL::ExceptionOr<GC::Ref<Key>> convert_a_value_to_a_multi_entry_key(JS::Realm& realm, JS::Value value)
{
    // 1. If input is an Array exotic object, then:
    if (value.is_object() && is<JS::Array>(value.as_object())) {

        // 1. Let len be ? ToLength( ? Get(input, "length")).
        auto len = TRY(length_of_array_like(realm.vm(), value.as_object()));

        // 2. Let seen be a new set containing only input.
        Vector<JS::Value> seen { value };

        // 3. Let keys be a new empty list.
        Vector<GC::Root<Key>> keys;

        // 4. Let index be 0.
        u64 index = 0;

        // 5. While index is less than len:
        while (index < len) {

            // 1. Let entry be Get(input, index).
            auto maybe_entry = value.as_object().get(index);

            // 2. If entry is not an abrupt completion, then:
            if (!maybe_entry.is_error()) {

                // 1. Let key be the result of converting a value to a key with arguments entry and seen.
                auto completion_key = convert_a_value_to_a_key(realm, maybe_entry.release_value(), seen);

                // 2. If key is not invalid or an abrupt completion, and there is no item in keys equal to key, then append key to keys.
                if (!completion_key.is_error()) {
                    auto key = completion_key.release_value();

                    if (!key->is_invalid() && !keys.contains_slow(key))
                        keys.append(key);
                }
            }

            // 3. Increase index by 1.
            index++;
        }

        // 6. Return a new array key with value set to keys.
        return Key::create_array(realm, keys);
    }

    // 2. Otherwise, return the result of converting a value to a key with argument input. Rethrow any exceptions.
    return convert_a_value_to_a_key(realm, value);
}

// https://w3c.github.io/IndexedDB/#evaluate-a-key-path-on-a-value
WebIDL::ExceptionOr<ErrorOr<JS::Value>> evaluate_key_path_on_a_value(JS::Realm& realm, JS::Value value, KeyPath const& key_path)
{
    // 1. If keyPath is a list of strings, then:
    if (key_path.has<Vector<String>>()) {
        auto const& key_path_list = key_path.get<Vector<String>>();

        // 1. Let result be a new Array object created as if by the expression [].
        auto result = MUST(JS::Array::create(realm, 0));

        // 2. Let i be 0.
        u64 i = 0;

        // 3. For each item of keyPath:
        for (auto const& item : key_path_list) {
            // 1. Let key be the result of recursively evaluating a key path on a value with item and value.
            auto completion_key = evaluate_key_path_on_a_value(realm, value, item);

            // 2. Assert: key is not an abrupt completion.
            VERIFY(!completion_key.is_error());

            // 3. If key is failure, abort the overall algorithm and return failure.
            auto key = TRY(TRY(completion_key));

            // 4. Let p be ! ToString(i).
            auto p = JS::PropertyKey { i };

            // 5. Let status be CreateDataProperty(result, p, key).
            auto status = MUST(result->create_data_property(p, key));

            // 6. Assert: status is true.
            VERIFY(status);

            // 7. Increase i by 1.
            i++;
        }

        // 4. Return result.
        return result;
    }

    auto const& key_path_string = key_path.get<String>();

    // 2. If keyPath is the empty string, return value and skip the remaining steps.
    if (key_path_string.is_empty())
        return value;

    // 3. Let identifiers be the result of strictly splitting keyPath on U+002E FULL STOP characters (.).
    // 4. For each identifier of identifiers, jump to the appropriate step below:
    TRY(key_path_string.bytes_as_string_view().for_each_split_view('.', SplitBehavior::KeepEmpty, [&](auto const& identifier) -> ErrorOr<void> {
        // If Type(value) is String, and identifier is "length"
        if (value.is_string() && identifier == "length") {
            // Let value be a Number equal to the number of elements in value.
            value = JS::Value(value.as_string().length_in_utf16_code_units());
        }

        // If value is an Array and identifier is "length"
        else if (value.is_object() && is<JS::Array>(value.as_object()) && identifier == "length") {
            // Let value be ! ToLength(! Get(value, "length")).
            value = JS::Value(MUST(length_of_array_like(realm.vm(), value.as_object())));
        }

        // If value is a Blob and identifier is "size"
        else if (value.is_object() && is<FileAPI::Blob>(value.as_object()) && identifier == "size") {
            // Let value be value’s size.
            value = JS::Value(static_cast<FileAPI::Blob&>(value.as_object()).size());
        }

        // If value is a Blob and identifier is "type"
        else if (value.is_object() && is<FileAPI::Blob>(value.as_object()) && identifier == "type") {
            // Let value be a String equal to value’s type.
            value = JS::PrimitiveString::create(realm.vm(), static_cast<FileAPI::Blob&>(value.as_object()).type());
        }

        // If value is a File and identifier is "name"
        else if (value.is_object() && is<FileAPI::File>(value.as_object()) && identifier == "name") {
            // Let value be a String equal to value’s name.
            value = JS::PrimitiveString::create(realm.vm(), static_cast<FileAPI::File&>(value.as_object()).name());
        }

        // If value is a File and identifier is "lastModified"
        else if (value.is_object() && is<FileAPI::File>(value.as_object()) && identifier == "lastModified") {
            // Let value be a Number equal to value’s lastModified.
            value = JS::Value(static_cast<double>(static_cast<FileAPI::File&>(value.as_object()).last_modified()));
        }

        // Otherwise
        else {
            // 1. If Type(value) is not Object, return failure.
            if (!value.is_object())
                return Error::from_string_literal("Value is not an object during key path evaluation");

            auto identifier_property = Utf16String::from_utf8_without_validation(identifier.bytes());

            // 2. Let hop be ! HasOwnProperty(value, identifier).
            auto hop = MUST(value.as_object().has_own_property(identifier_property));

            // 3. If hop is false, return failure.
            if (!hop)
                return Error::from_string_literal("Failed to find property on object during key path evaluation");

            // 4. Let value be ! Get(value, identifier).
            value = MUST(value.as_object().get(identifier_property));

            // 5. If value is undefined, return failure.
            if (value.is_undefined())
                return Error::from_string_literal("undefined value on object during key path evaluation");
        }

        return {};
    }));

    // 5. Assert: value is not an abrupt completion.
    // NOTE: Step 4 above makes this assertion via MUST

    // 6. Return value.
    return value;
}

// https://w3c.github.io/IndexedDB/#extract-a-key-from-a-value-using-a-key-path
WebIDL::ExceptionOr<ErrorOr<GC::Ref<Key>>> extract_a_key_from_a_value_using_a_key_path(JS::Realm& realm, JS::Value value, KeyPath const& key_path, bool multi_entry)
{
    // 1. Let r be the result of evaluating a key path on a value with value and keyPath. Rethrow any exceptions.
    // 2. If r is failure, return failure.
    auto r = TRY(TRY(evaluate_key_path_on_a_value(realm, value, key_path)));

    // 3. Let key be the result of converting a value to a key with r if the multiEntry flag is false,
    //    and the result of converting a value to a multiEntry key with r otherwise. Rethrow any exceptions.
    // 4. If key is invalid, return invalid.
    // 5. Return key.
    return multi_entry ? TRY(convert_a_value_to_a_multi_entry_key(realm, r)) : TRY(convert_a_value_to_a_key(realm, r));
}

// https://w3c.github.io/IndexedDB/#check-that-a-key-could-be-injected-into-a-value
bool check_that_a_key_could_be_injected_into_a_value(JS::Realm& realm, JS::Value value, KeyPath const& key_path)
{
    // NOTE: The key paths used in this section are always strings and never sequences

    // 1. Let identifiers be the result of strictly splitting keyPath on U+002E FULL STOP characters (.).
    auto identifiers = MUST(key_path.get<String>().split('.'));

    // 2. Assert: identifiers is not empty.
    VERIFY(!identifiers.is_empty());

    // 3. Remove the last item of identifiers.
    identifiers.take_last();

    // 4. For each remaining identifier of identifiers, if any:
    for (auto const& identifier : identifiers) {
        auto identifier_utf16 = Utf16FlyString::from_utf8(identifier);

        // 1. If value is not an Object or an Array, return false.
        if (!(value.is_object() || MUST(value.is_array(realm.vm()))))
            return false;

        // 2. Let hop be ! HasOwnProperty(value, identifier).
        auto hop = MUST(value.as_object().has_own_property(identifier_utf16));

        // 3. If hop is false, return true.
        if (!hop)
            return true;

        // 4. Let value be ! Get(value, identifier).
        value = MUST(value.as_object().get(identifier_utf16));
    }

    // 5. Return true if value is an Object or an Array, or false otherwise.
    return value.is_object() || MUST(value.is_array(realm.vm()));
}

// https://w3c.github.io/IndexedDB/#fire-an-error-event
void fire_an_error_event(JS::Realm& realm, GC::Ref<IDBRequest> request)
{
    // 1. Let event be the result of creating an event using Event.
    // 2. Set event’s type attribute to "error".
    // 3. Set event’s bubbles and cancelable attributes to true.
    auto event = DOM::Event::create(realm, HTML::EventNames::error, { .bubbles = true, .cancelable = true });

    // 4. Let transaction be request’s transaction.
    auto transaction = request->transaction();

    // 5. Let legacyOutputDidListenersThrowFlag be initially false.
    bool legacy_output_did_listeners_throw_flag = false;

    // 6. If transaction’s state is inactive, then set transaction’s state to active.
    if (transaction->state() == IDBTransaction::TransactionState::Inactive)
        transaction->set_state(IDBTransaction::TransactionState::Active);

    // 7. Dispatch event at request with legacyOutputDidListenersThrowFlag.
    DOM::EventDispatcher::dispatch(request, *event, false, legacy_output_did_listeners_throw_flag);

    // 8. If transaction’s state is active, then:
    if (transaction->state() == IDBTransaction::TransactionState::Active) {
        // 1. Set transaction’s state to inactive.
        transaction->set_state(IDBTransaction::TransactionState::Inactive);

        // 2. If legacyOutputDidListenersThrowFlag is true, then run abort a transaction with transaction and a newly created "AbortError" DOMException and terminate these steps.
        //    This is done even if event’s canceled flag is false.
        if (legacy_output_did_listeners_throw_flag) {
            abort_a_transaction(*transaction, WebIDL::AbortError::create(realm, "Error event interrupted by exception"_utf16));
            return;
        }

        // 3. If event’s canceled flag is false, then run abort a transaction using transaction and request's error, and terminate these steps.
        if (!event->cancelled()) {
            abort_a_transaction(*transaction, request->error());
            return;
        }

        // 4. If transaction’s request list is empty, then run commit a transaction with transaction.
        if (transaction->request_list().is_empty())
            commit_a_transaction(realm, *transaction);
    }
}

// https://w3c.github.io/IndexedDB/#fire-a-success-event
void fire_a_success_event(JS::Realm& realm, GC::Ref<IDBRequest> request)
{
    // 1. Let event be the result of creating an event using Event.
    // 2. Set event’s type attribute to "success".
    // 3. Set event’s bubbles and cancelable attributes to false.
    auto event = DOM::Event::create(realm, HTML::EventNames::success, { .bubbles = false, .cancelable = false });

    // 4. Let transaction be request’s transaction.
    auto transaction = request->transaction();

    // 5. Let legacyOutputDidListenersThrowFlag be initially false.
    bool legacy_output_did_listeners_throw_flag = false;

    // 6. If transaction’s state is inactive, then set transaction’s state to active.
    if (transaction->state() == IDBTransaction::TransactionState::Inactive)
        transaction->set_state(IDBTransaction::TransactionState::Active);

    // 7. Dispatch event at request with legacyOutputDidListenersThrowFlag.
    DOM::EventDispatcher::dispatch(request, *event, false, legacy_output_did_listeners_throw_flag);

    // 8. If transaction’s state is active, then:
    if (transaction->state() == IDBTransaction::TransactionState::Active) {
        // 1. Set transaction’s state to inactive.
        transaction->set_state(IDBTransaction::TransactionState::Inactive);

        // 2. If legacyOutputDidListenersThrowFlag is true, then run abort a transaction with transaction and a newly created "AbortError" DOMException.
        if (legacy_output_did_listeners_throw_flag) {
            abort_a_transaction(*transaction, WebIDL::AbortError::create(realm, "An error occurred"_utf16));
            return;
        }

        // 3. If transaction’s request list is empty, then run commit a transaction with transaction.
        if (transaction->request_list().is_empty())
            commit_a_transaction(realm, *transaction);
    }
}

// https://w3c.github.io/IndexedDB/#asynchronously-execute-a-request
GC::Ref<IDBRequest> asynchronously_execute_a_request(JS::Realm& realm, IDBRequestSource source, GC::Ref<GC::Function<WebIDL::ExceptionOr<JS::Value>()>> operation, GC::Ptr<IDBRequest> request_input)
{
    // 1. Let transaction be the transaction associated with source.
    auto transaction = source.visit(
        [](Empty) -> GC::Ptr<IDBTransaction> {
            VERIFY_NOT_REACHED();
        },
        [](GC::Ref<IDBObjectStore> object_store) -> GC::Ptr<IDBTransaction> {
            return object_store->transaction();
        },
        [](GC::Ref<IDBIndex> index) -> GC::Ptr<IDBTransaction> {
            return index->transaction();
        },
        [](GC::Ref<IDBCursor> cursor) -> GC::Ptr<IDBTransaction> {
            return cursor->transaction();
        });

    // 2. Assert: transaction’s state is active.
    VERIFY(transaction->state() == IDBTransaction::TransactionState::Active);

    // 3. If request was not given, let request be a new request with source as source.
    GC::Ref<IDBRequest> request = request_input ? GC::Ref(*request_input) : IDBRequest::create(realm, source);

    // 4. Add request to the end of transaction’s request list.
    transaction->request_list().append(request);

    // Set the two-way binding. (Missing spec step)
    // FIXME: https://github.com/w3c/IndexedDB/issues/433
    request->set_transaction(transaction);

    // 5. Run these steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, transaction, operation, request]() {
        // 1. Wait until request is the first item in transaction’s request list that is not processed.
        if constexpr (IDB_DEBUG) {
            dbgln("asynchronously_execute_a_request: waiting for step 5.1");
            dbgln("requests in queue:");
            for (auto const& item : transaction->request_list()) {
                dbgln("[{}] - {} = {}", item == request ? "x"sv : " "sv, item->uuid(), item->processed() ? "processed"sv : "not processed"sv);
            }
        }

        transaction->request_list().all_previous_requests_processed(realm.heap(), request, GC::create_function(realm.heap(), [&realm, transaction, operation, request] {
            dbgln_if(IDB_DEBUG, "asynchronously_execute_a_request: finished waiting for step 5.1, executing request");

            // 2. Let result be the result of performing operation.
            auto result = operation->function()();

            // 3. If result is an error and transaction’s state is committing, then run abort a transaction with transaction and result, and terminate these steps.
            if (result.is_error() && transaction->state() == IDBTransaction::TransactionState::Committing) {
                dbgln_if(IDB_DEBUG, "asynchronously_execute_a_request: step 5.3: request errored, aborting transaction");
                abort_a_transaction(*transaction, result.exception().get<GC::Ref<WebIDL::DOMException>>());
                return;
            }

            // FIXME: 4. If result is an error, then revert all changes made by operation.

            // 5. Set request’s processed flag to true.
            request->set_processed(true);

            // 6. Queue a database task to run these steps:
            dbgln_if(IDB_DEBUG, "asynchronously_execute_a_request: step 5.6: request finished without error, queuing task to finish up");
            queue_a_database_task(GC::create_function(realm.vm().heap(), [&realm, request, result, transaction]() mutable {
                dbgln_if(IDB_DEBUG, "asynchronously_execute_a_request: step 5.6: finish up task executing");

                // 1. Remove request from transaction’s request list.
                transaction->request_list().remove_first_matching([&request](auto& entry) { return entry.ptr() == request.ptr(); });

                // 2. Set request’s done flag to true.
                request->set_done(true);

                // 3. If result is an error, then:
                if (result.is_error()) {
                    // 1. Set request’s result to undefined.
                    request->set_result(JS::js_undefined());

                    // 2. Set request’s error to result.
                    request->set_error(result.exception().get<GC::Ref<WebIDL::DOMException>>());

                    // 3. Fire an error event at request.
                    fire_an_error_event(realm, request);
                } else {
                    // 1. Set request’s result to result.
                    request->set_result(result.release_value());

                    // 2. Set request’s error to undefined.
                    request->set_error(Optional<GC::Ptr<WebIDL::DOMException>> {});

                    // 3. Fire a success event at request.
                    fire_a_success_event(realm, request);
                }
            }));
        }));
    }));

    // 6. Return request.
    return request;
}

// https://w3c.github.io/IndexedDB/#generate-a-key
ErrorOr<u64> generate_a_key(GC::Ref<ObjectStore> store)
{
    // 1. Let generator be store’s key generator.
    auto& generator = store->key_generator();

    // 2. Let key be generator’s current number.
    auto key = generator.current_number();

    // 3. If key is greater than 2^53 (9007199254740992), then return failure.
    if (key > static_cast<u64>(MAX_KEY_GENERATOR_VALUE))
        return Error::from_string_literal("Key is greater than 2^53 while trying to generate a key");

    // 4. Increase generator’s current number by 1.
    generator.increment(1);

    // 5. Return key.
    return key;
}

// https://w3c.github.io/IndexedDB/#possibly-update-the-key-generator
void possibly_update_the_key_generator(GC::Ref<ObjectStore> store, GC::Ref<Key> key)
{
    // 1. If the type of key is not number, abort these steps.
    if (key->type() != Key::KeyType::Number)
        return;

    // 2. Let value be the value of key.
    auto value = key->value_as_double();

    // 3. Set value to the minimum of value and 2^53 (9007199254740992).
    value = min(value, MAX_KEY_GENERATOR_VALUE);

    // 4. Set value to the largest integer not greater than value.
    value = floor(value);

    // 5. Let generator be store’s key generator.
    auto& generator = store->key_generator();

    // 6. If value is greater than or equal to generator’s current number, then set generator’s current number to value + 1.
    if (value >= static_cast<double>(generator.current_number())) {
        generator.set(static_cast<u64>(value + 1));
    }
}

// https://w3c.github.io/IndexedDB/#inject-a-key-into-a-value-using-a-key-path
void inject_a_key_into_a_value_using_a_key_path(JS::Realm& realm, JS::Value value, GC::Ref<Key> key, KeyPath const& key_path)
{
    // 1. Let identifiers be the result of strictly splitting keyPath on U+002E FULL STOP characters (.).
    auto identifiers = MUST(key_path.get<String>().split('.'));

    // 2. Assert: identifiers is not empty.
    VERIFY(!identifiers.is_empty());

    // 3. Let last be the last item of identifiers and remove it from the list.
    auto last = identifiers.take_last();

    // 4. For each remaining identifier of identifiers:
    for (auto const& identifier : identifiers) {
        auto identifier_utf16 = Utf16FlyString::from_utf8(identifier);

        // 1. Assert: value is an Object or an Array.
        VERIFY(value.is_object() || MUST(value.is_array(realm.vm())));

        // 2. Let hop be ! HasOwnProperty(value, identifier).
        auto hop = MUST(value.as_object().has_own_property(identifier_utf16));

        // 3. If hop is false, then:
        if (!hop) {
            // 1. Let o be a new Object created as if by the expression ({}).
            auto o = JS::Object::create(realm, realm.intrinsics().object_prototype());

            // 2. Let status be CreateDataProperty(value, identifier, o).
            auto status = MUST(value.as_object().create_data_property(identifier_utf16, o));

            // 3. Assert: status is true.
            VERIFY(status);
        }

        // 4. Let value be ! Get(value, identifier).
        value = MUST(value.as_object().get(identifier_utf16));
    }

    // 5. Assert: value is an Object or an Array.
    VERIFY(value.is_object() || MUST(value.is_array(realm.vm())));

    // 6. Let keyValue be the result of converting a key to a value with key.
    auto key_value = convert_a_key_to_a_value(realm, key);

    // 7. Let status be CreateDataProperty(value, last, keyValue).
    auto status = MUST(value.as_object().create_data_property(Utf16FlyString::from_utf8(last), key_value));

    // 8. Assert: status is true.
    VERIFY(status);
}

// https://w3c.github.io/IndexedDB/#delete-records-from-an-object-store
JS::Value delete_records_from_an_object_store(GC::Ref<ObjectStore> store, GC::Ref<IDBKeyRange> range)
{
    // 1. Remove all records, if any, from store’s list of records with key in range.
    store->remove_records_in_range(range);

    // 2. For each index which references store, remove every record from index’s list of records whose value is in range, if any such records exist.
    for (auto const& [name, index] : store->index_set()) {
        index->remove_records_with_value_in_range(range);
    }

    // 3. Return undefined.
    return JS::js_undefined();
}

// https://w3c.github.io/IndexedDB/#store-a-record-into-an-object-store
WebIDL::ExceptionOr<GC::Ptr<Key>> store_a_record_into_an_object_store(JS::Realm& realm, GC::Ref<ObjectStore> store, JS::Value value, GC::Ptr<Key> key, bool no_overwrite)
{
    // 1. If store uses a key generator, then:
    if (store->uses_a_key_generator()) {
        // 1. If key is undefined, then:
        if (key == nullptr) {
            // 1. Let key be the result of generating a key for store.
            auto maybe_key = generate_a_key(store);

            // 2. If key is failure, then this operation failed with a "ConstraintError" DOMException. Abort this algorithm without taking any further steps.
            if (maybe_key.is_error())
                return WebIDL::ConstraintError::create(realm, Utf16String::from_utf8_without_validation(maybe_key.error().string_literal()));

            key = Key::create_number(realm, static_cast<double>(maybe_key.value()));

            // 3. If store also uses in-line keys, then run inject a key into a value using a key path with value, key and store’s key path.
            if (store->uses_inline_keys())
                inject_a_key_into_a_value_using_a_key_path(realm, value, GC::Ref(*key), store->key_path().value());
        }

        // 2. Otherwise, run possibly update the key generator for store with key.
        else {
            possibly_update_the_key_generator(store, GC::Ref(*key));
        }
    }

    // 2. If the no-overwrite flag was given to these steps and is true, and a record already exists in store with its key equal to key,
    //    then this operation failed with a "ConstraintError" DOMException. Abort this algorithm without taking any further steps.
    auto has_record = store->has_record_with_key(*key);
    if (no_overwrite && has_record)
        return WebIDL::ConstraintError::create(realm, "Record already exists"_utf16);

    // 3. If a record already exists in store with its key equal to key, then remove the record from store using delete records from an object store.
    if (has_record) {
        auto key_range = IDBKeyRange::create(realm, key, key, IDBKeyRange::LowerOpen::No, IDBKeyRange::UpperOpen::No);
        delete_records_from_an_object_store(store, key_range);
    }

    // 4. Store a record in store containing key as its key and ! StructuredSerializeForStorage(value) as its value.
    //    The record is stored in the object store’s list of records such that the list is sorted according to the key of the records in ascending order.
    ObjectStoreRecord record = {
        .key = *key,
        .value = MUST(HTML::structured_serialize_for_storage(realm.vm(), value)),
    };
    store->store_a_record(record);

    // 5. For each index which references store:
    for (auto const& [name, index] : store->index_set()) {
        // 1. Let index key be the result of extracting a key from a value using a key path with value, index’s key path, and index’s multiEntry flag.
        auto completion_index_key = extract_a_key_from_a_value_using_a_key_path(realm, value, index->key_path(), index->multi_entry());

        // 2. If index key is an exception, or invalid, or failure, take no further actions for index, and continue these steps for the next index.
        if (completion_index_key.is_error())
            continue;

        auto failure_index_key = completion_index_key.release_value();
        if (failure_index_key.is_error())
            continue;

        auto index_key = failure_index_key.release_value();
        if (index_key->is_invalid())
            continue;

        auto index_multi_entry = index->multi_entry();
        auto index_key_is_array = index_key->type() == Key::KeyType::Array;
        auto index_is_unique = index->unique();

        // 3. If index’s multiEntry flag is false, or if index key is not an array key,
        //    and if index already contains a record with key equal to index key,
        //    and index’s unique flag is true,
        //    then this operation failed with a "ConstraintError" DOMException.
        //    Abort this algorithm without taking any further steps.
        if ((!index_multi_entry || !index_key_is_array) && index_is_unique && index->has_record_with_key(index_key))
            return WebIDL::ConstraintError::create(realm, "Record already exists in index"_utf16);

        // 4. If index’s multiEntry flag is true and index key is an array key,
        //    and if index already contains a record with key equal to any of the subkeys of index key,
        //    and index’s unique flag is true,
        //    then this operation failed with a "ConstraintError" DOMException.
        //    Abort this algorithm without taking any further steps.
        if (index_multi_entry && index_key_is_array && index_is_unique) {
            for (auto const& subkey : index_key->subkeys()) {
                if (index->has_record_with_key(*subkey))
                    return WebIDL::ConstraintError::create(realm, "Record already exists in index"_utf16);
            }
        }

        // 5. If index’s multiEntry flag is false, or if index key is not an array key
        //    then store a record in index containing index key as its key and key as its value.
        //    The record is stored in index’s list of records such that the list is sorted primarily on the records keys,
        //    and secondarily on the records values, in ascending order.
        if (!index_multi_entry || !index_key_is_array) {
            IndexRecord index_record = {
                .key = *index_key,
                .value = *key,
            };
            index->store_a_record(index_record);
        }

        // 6. If index’s multiEntry flag is true and index key is an array key,
        //    then for each subkey of the subkeys of index key store a record in index containing subkey as its key and key as its value.
        if (index_multi_entry && index_key_is_array) {
            for (auto const& subkey : index_key->subkeys()) {
                IndexRecord index_record = {
                    .key = *subkey,
                    .value = *key,
                };
                index->store_a_record(index_record);
            }
        }
    }

    // 6. Return key.
    return key;
}

// https://w3c.github.io/IndexedDB/#convert-a-value-to-a-key-range
WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> convert_a_value_to_a_key_range(JS::Realm& realm, Optional<JS::Value> value, bool null_disallowed)
{
    // 1. If value is a key range, return value.
    if (value.has_value() && value->is_object() && is<IDBKeyRange>(value->as_object())) {
        return GC::Ref(static_cast<IDBKeyRange&>(value->as_object()));
    }

    // 2. If value is undefined or is null, then throw a "DataError" DOMException if null disallowed flag is true, or return an unbounded key range otherwise.
    if (!value.has_value() || (value.has_value() && (value->is_undefined() || value->is_null()))) {
        if (null_disallowed)
            return WebIDL::DataError::create(realm, "Value is undefined or null"_utf16);

        return IDBKeyRange::create(realm, {}, {}, IDBKeyRange::LowerOpen::No, IDBKeyRange::UpperOpen::No);
    }

    // 3. Let key be the result of converting a value to a key with value. Rethrow any exceptions.
    auto key = TRY(convert_a_value_to_a_key(realm, *value));

    // 4. If key is invalid, throw a "DataError" DOMException.
    if (key->is_invalid())
        return WebIDL::DataError::create(realm, "Value is invalid"_utf16);

    // 5. Return a key range containing only key.
    return IDBKeyRange::create(realm, key, key, IDBKeyRange::LowerOpen::No, IDBKeyRange::UpperOpen::No);
}

// https://w3c.github.io/IndexedDB/#count-the-records-in-a-range
JS::Value count_the_records_in_a_range(RecordSource source, GC::Ref<IDBKeyRange> range)
{
    // 1. Let count be the number of records, if any, in source’s list of records with key in range.
    auto count = source.visit(
        [range](GC::Ref<ObjectStore> object_store) {
            return object_store->count_records_in_range(range);
        },
        [range](GC::Ref<Index> index) {
            return index->count_records_in_range(range);
        });

    // 2. Return count.
    return JS::Value(count);
}

// https://w3c.github.io/IndexedDB/#retrieve-a-value-from-an-object-store
WebIDL::ExceptionOr<JS::Value> retrieve_a_value_from_an_object_store(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBKeyRange> range)
{
    // 1. Let record be the first record in store’s list of records whose key is in range, if any.
    auto record = store->first_in_range(range);

    // 2. If record was not found, return undefined.
    if (!record.has_value())
        return JS::js_undefined();

    // 3. Let serialized be record’s value. If an error occurs while reading the value from the underlying storage, return a newly created "NotReadableError" DOMException.
    auto serialized = record->value;

    // 4. Return ! StructuredDeserialize(serialized, targetRealm).
    return MUST(HTML::structured_deserialize(realm.vm(), serialized, realm));
}

// https://w3c.github.io/IndexedDB/#iterate-a-cursor
GC::Ptr<IDBCursor> iterate_a_cursor(JS::Realm& realm, GC::Ref<IDBCursor> cursor, GC::Ptr<Key> key, GC::Ptr<Key> primary_key, u64 count)
{
    // 1. Let source be cursor’s source.
    auto source = cursor->internal_source();

    // 2. Let direction be cursor’s direction.
    auto direction = cursor->direction();

    // 3. Assert: if primaryKey is given, source is an index and direction is "next" or "prev".
    auto direction_is_next_or_prev = direction == Bindings::IDBCursorDirection::Next || direction == Bindings::IDBCursorDirection::Prev;
    if (primary_key)
        VERIFY(source.has<GC::Ref<Index>>() && direction_is_next_or_prev);

    // 4. Let records be the list of records in source.
    Variant<ReadonlySpan<ObjectStoreRecord>, ReadonlySpan<IndexRecord>> records = source.visit(
        [](GC::Ref<ObjectStore> object_store) -> Variant<ReadonlySpan<ObjectStoreRecord>, ReadonlySpan<IndexRecord>> {
            return object_store->records();
        },
        [](GC::Ref<Index> index) -> Variant<ReadonlySpan<ObjectStoreRecord>, ReadonlySpan<IndexRecord>> {
            return index->records();
        });

    // 5. Let range be cursor’s range.
    auto range = cursor->range();

    // 6. Let position be cursor’s position.
    auto position = cursor->position();

    // 7. Let object store position be cursor’s object store position.
    auto object_store_position = cursor->object_store_position();

    // 8. If count is not given, let count be 1.
    // NOTE: This is handled by the default parameter

    auto next_requirements = [&](Variant<ObjectStoreRecord, IndexRecord> const& record) -> bool {
        // * If key is defined:
        if (key) {
            // * The record’s key is greater than or equal to key.
            if (!record.visit([&](auto const& inner_record) {
                    return Key::greater_than_or_equal(inner_record.key, *key);
                }))
                return false;
        }

        // * If primaryKey is defined:
        if (primary_key) {
            auto const& inner_record = record.get<IndexRecord>();

            // * If the record’s key is equal to key:
            if (Key::equals(inner_record.key, *key)) {
                // * The record’s value is greater than or equal to primaryKey
                if (!Key::greater_than_or_equal(inner_record.value, *primary_key))
                    return false;
            }
            // * Else:
            else {
                // * The record’s key is greater than key.
                if (!Key::greater_than(inner_record.key, *key))
                    return false;
            }
        }

        // * If position is defined and source is an object store:
        if (position && source.has<GC::Ref<ObjectStore>>()) {
            auto const& inner_record = record.get<ObjectStoreRecord>();

            // * The record’s key is greater than position.
            if (!Key::greater_than(inner_record.key, *position))
                return false;
        }

        // * If position is defined and source is an index:
        if (position && source.has<GC::Ref<Index>>()) {
            auto const& inner_record = record.get<IndexRecord>();

            // * If the record’s key is equal to position:
            if (Key::equals(inner_record.key, *position)) {
                // * The record’s value is greater than object store position
                if (!Key::greater_than(inner_record.value, *object_store_position))
                    return false;
            }
            // * Else:
            else {
                // * The record’s key is greater than position.
                if (!Key::greater_than(inner_record.key, *position))
                    return false;
            }
        }

        // * The record’s key is in range.
        return record.visit(
            [&](auto const& inner_record) {
                return range->is_in_range(inner_record.key);
            });
    };

    auto next_unique_requirements = [&](Variant<ObjectStoreRecord, IndexRecord> const& record) -> bool {
        // * If key is defined:
        if (key) {
            // * The record’s key is greater than or equal to key.
            if (!record.visit([&](auto const& inner_record) {
                    return Key::greater_than_or_equal(inner_record.key, *key);
                }))
                return false;
        }

        // * If position is defined:
        if (position) {
            // * The record’s key is greater than position.
            if (!record.visit([&](auto const& inner_record) {
                    return Key::greater_than(inner_record.key, *position);
                }))
                return false;
        }

        // * The record’s key is in range.
        return record.visit(
            [&](auto const& inner_record) {
                return range->is_in_range(inner_record.key);
            });
    };

    auto prev_requirements = [&](Variant<ObjectStoreRecord, IndexRecord> const& record) -> bool {
        // * If key is defined:
        if (key) {
            // * The record’s key is less than or equal to key.
            if (!record.visit([&](auto const& inner_record) {
                    return Key::less_than_or_equal(inner_record.key, *key);
                }))
                return false;
        }

        // * If primaryKey is defined:
        if (primary_key) {
            auto const& inner_record = record.get<IndexRecord>();

            // * If the record’s key is equal to key:
            if (Key::equals(inner_record.key, *key)) {
                // * The record’s value is less than or equal to primaryKey
                if (!Key::less_than_or_equal(inner_record.value, *primary_key))
                    return false;
            }
            // * Else:
            else {
                // * The record’s key is less than key.
                if (!Key::less_than(inner_record.key, *key))
                    return false;
            }
        }

        // * If position is defined and source is an object store:
        if (position && source.has<GC::Ref<ObjectStore>>()) {
            auto const& inner_record = record.get<ObjectStoreRecord>();

            // * The record’s key is less than position.
            if (!Key::less_than(inner_record.key, *position))
                return false;
        }

        // * If position is defined and source is an index:
        if (position && source.has<GC::Ref<Index>>()) {
            auto const& inner_record = record.get<IndexRecord>();

            // * If the record’s key is equal to position:
            if (Key::equals(inner_record.key, *position)) {
                // * The record’s value is less than object store position
                if (!Key::less_than(inner_record.value, *object_store_position))
                    return false;
            }
            // Else:
            else {
                // * The record’s key is less than position.
                if (!Key::less_than(inner_record.key, *position))
                    return false;
            }
        }

        // * The record’s key is in range.
        return record.visit(
            [&](auto const& inner_record) {
                return range->is_in_range(inner_record.key);
            });
    };

    auto prev_unique_requirements = [&](Variant<ObjectStoreRecord, IndexRecord> const& record) -> bool {
        // * If key is defined:
        if (key) {
            // * The record’s key is less than or equal to key.
            if (!record.visit([&](auto const& inner_record) {
                    return Key::less_than_or_equal(inner_record.key, *key);
                }))
                return false;
        }

        //* If position is defined:
        if (position) {
            // * The record’s key is less than position.
            if (!record.visit([&](auto const& inner_record) {
                    return Key::less_than(inner_record.key, *position);
                }))
                return false;
        }

        // * The record’s key is in range.
        return record.visit(
            [&](auto const& inner_record) {
                return range->is_in_range(inner_record.key);
            });
    };

    // 9. While count is greater than 0:
    Variant<Empty, ObjectStoreRecord, IndexRecord> found_record;
    while (count > 0) {
        // 1. Switch on direction:
        switch (direction) {
        case Bindings::IDBCursorDirection::Next: {
            // Let found record be the first record in records which satisfy all of the following requirements:
            found_record = records.visit([&](auto content) -> Variant<Empty, ObjectStoreRecord, IndexRecord> {
                auto value = content.first_matching(next_requirements);
                if (value.has_value())
                    return *value;

                return Empty {};
            });
            break;
        }
        case Bindings::IDBCursorDirection::Nextunique: {
            // Let found record be the first record in records which satisfy all of the following requirements:
            found_record = records.visit([&](auto content) -> Variant<Empty, ObjectStoreRecord, IndexRecord> {
                auto value = content.first_matching(next_unique_requirements);
                if (value.has_value())
                    return *value;

                return Empty {};
            });
            break;
        }
        case Bindings::IDBCursorDirection::Prev: {
            // Let found record be the last record in records which satisfy all of the following requirements:
            found_record = records.visit([&](auto content) -> Variant<Empty, ObjectStoreRecord, IndexRecord> {
                auto value = content.last_matching(prev_requirements);
                if (value.has_value())
                    return *value;

                return Empty {};
            });
            break;
        }

        case Bindings::IDBCursorDirection::Prevunique: {
            // Let temp record be the last record in records which satisfy all of the following requirements:
            auto temp_record = records.visit([&](auto content) -> Variant<Empty, ObjectStoreRecord, IndexRecord> {
                auto value = content.last_matching(prev_unique_requirements);
                if (value.has_value())
                    return *value;

                return Empty {};
            });

            // If temp record is defined, let found record be the first record in records whose key is equal to temp record’s key.
            if (!temp_record.has<Empty>()) {
                auto temp_record_key = temp_record.visit(
                    [](Empty) -> GC::Ref<Key> { VERIFY_NOT_REACHED(); },
                    [](auto const& record) { return record.key; });

                found_record = records.visit([&](auto content) -> Variant<Empty, ObjectStoreRecord, IndexRecord> {
                    auto value = content.first_matching([&](auto const& content_record) {
                        return Key::equals(content_record.key, temp_record_key);
                    });
                    if (value.has_value())
                        return *value;

                    return Empty {};
                });
            }

            break;
        }
        }

        // 2. If found record is not defined, then:
        if (found_record.has<Empty>()) {
            // 1. Set cursor’s key to undefined.
            cursor->set_key(nullptr);

            // 2. If source is an index, set cursor’s object store position to undefined.
            if (source.has<GC::Ref<Index>>())
                cursor->set_object_store_position(nullptr);

            // 3. If cursor’s key only flag is false, set cursor’s value to undefined.
            if (!cursor->key_only())
                cursor->set_value(JS::js_undefined());

            // 4. Return null.
            return nullptr;
        }

        // 3. Let position be found record’s key.
        position = found_record.visit(
            [](Empty) -> GC::Ref<Key> { VERIFY_NOT_REACHED(); },
            [](auto val) { return val.key; });

        // 4. If source is an index, let object store position be found record’s value.
        if (source.has<GC::Ref<Index>>())
            object_store_position = found_record.get<IndexRecord>().value;

        // 5. Decrease count by 1.
        count--;
    }

    // 10. Set cursor’s position to position.
    cursor->set_position(position);

    // 11. If source is an index, set cursor’s object store position to object store position.
    if (source.has<GC::Ref<Index>>())
        cursor->set_object_store_position(object_store_position);

    // 12. Set cursor’s key to found record’s key.
    cursor->set_key(found_record.visit(
        [](Empty) -> GC::Ref<Key> { VERIFY_NOT_REACHED(); },
        [](auto val) { return val.key; }));

    // 13. If cursor’s key only flag is false, then:
    if (!cursor->key_only()) {

        // 1. Let serialized be found record’s value if source is an object store, or found record’s referenced value otherwise.
        auto serialized = source.visit(
            [&](GC::Ref<ObjectStore>) {
                return found_record.get<ObjectStoreRecord>().value;
            },
            [&](GC::Ref<Index> index) {
                return index->referenced_value(found_record.get<IndexRecord>());
            });

        // 2. Set cursor’s value to ! StructuredDeserialize(serialized, targetRealm)
        cursor->set_value(MUST(HTML::structured_deserialize(realm.vm(), serialized, realm)));
    }

    // 14. Set cursor’s got value flag to true.
    cursor->set_got_value(true);

    // 15. Return cursor.
    return cursor;
}

// https://w3c.github.io/IndexedDB/#clear-an-object-store
JS::Value clear_an_object_store(GC::Ref<ObjectStore> store)
{
    // 1. Remove all records from store.
    store->clear_records();

    // 2. In all indexes which reference store, remove all records.
    for (auto const& [name, index] : store->index_set()) {
        index->clear_records();
    }

    // 3. Return undefined.
    return JS::js_undefined();
}

// https://w3c.github.io/IndexedDB/#retrieve-a-key-from-an-object-store
JS::Value retrieve_a_key_from_an_object_store(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBKeyRange> range)
{
    // 1. Let record be the first record in store’s list of records whose key is in range, if any.
    auto record = store->first_in_range(range);

    // 2. If record was not found, return undefined.
    if (!record.has_value())
        return JS::js_undefined();

    // 3. Return the result of converting a key to a value with record’s key.
    return convert_a_key_to_a_value(realm, record.value().key);
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-values-from-an-object-store
GC::Ref<JS::Array> retrieve_multiple_values_from_an_object_store(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    // 1. If count is not given or is 0 (zero), let count be infinity.
    if (count.has_value() && *count == 0)
        count = OptionalNone();

    // 2. Let records be a list containing the first count records in store’s list of records whose key is in range.
    auto records = store->first_n_in_range(range, count);

    // 3. Let list be an empty list.
    auto list = MUST(JS::Array::create(realm, records.size()));

    // 4. For each record of records:
    for (u32 i = 0; i < records.size(); ++i) {
        auto& record = records[i];

        // 1. Let serialized be record’s value. If an error occurs while reading the value from the underlying storage, return a newly created "NotReadableError" DOMException.
        auto serialized = record.value;

        // 2. Let entry be ! StructuredDeserialize(serialized, targetRealm).
        auto entry = MUST(HTML::structured_deserialize(realm.vm(), serialized, realm));

        // 3. Append entry to list.
        MUST(list->create_data_property_or_throw(i, entry));
    }

    // 5. Return list converted to a sequence<any>.
    return list;
}

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#retrieve-multiple-items-from-an-object-store
GC::Ref<JS::Array> retrieve_multiple_items_from_an_object_store(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBKeyRange> range, RecordKind kind, Bindings::IDBCursorDirection direction, Optional<WebIDL::UnsignedLong> count)
{
    // 1. If count is not given or is 0 (zero), let count be infinity.
    if (count.has_value() && *count == 0)
        count = OptionalNone();

    // 2. Let records an empty list.
    GC::ConservativeVector<ObjectStoreRecord> records(realm.heap());

    // 3. If direction is "next" or "nextunique", set records to the first count of store’s list of records whose key is in range.
    if (direction == Bindings::IDBCursorDirection::Next || direction == Bindings::IDBCursorDirection::Nextunique) {
        records.extend(store->first_n_in_range(range, count));
    }

    // 4. If direction is "prev" or "prevunique", set records to the last count of store’s list of records whose key is in range.
    if (direction == Bindings::IDBCursorDirection::Prev || direction == Bindings::IDBCursorDirection::Prevunique) {
        records.extend(store->last_n_in_range(range, count));
    }

    // 5. Let list be an empty list.
    auto list = MUST(JS::Array::create(realm, records.size()));

    // 6. For each record of records, switching on kind:
    for (u32 i = 0; i < records.size(); ++i) {
        auto& record = records[i];

        switch (kind) {
        case RecordKind::Key: {
            // 1. Let key be the result of converting a key to a value with record’s key.
            auto key = convert_a_key_to_a_value(realm, record.key);

            // 2. Append key to list.
            MUST(list->create_data_property_or_throw(i, key));
            break;
        }
        case RecordKind::Value: {
            // 1. Let serialized be record’s value.
            auto serialized = record.value;

            // 2. Let value be ! StructuredDeserialize(serialized, targetRealm).
            auto entry = MUST(HTML::structured_deserialize(realm.vm(), serialized, realm));

            // 3. Append entry to list.
            MUST(list->create_data_property_or_throw(i, entry));
            break;
        }
        case RecordKind::Record: {
            // 1. Let key be the record’s key.
            auto key = record.key;

            // 2. Let serialized be record’s value.
            auto serialized = record.value;

            // 3. Let value be ! StructuredDeserialize(serialized, targetRealm).
            auto value = MUST(HTML::structured_deserialize(realm.vm(), serialized, realm));

            // 4. Let record snapshot be a new record snapshot with its key set to key, value set to value, and primary key set to key.
            auto record_snapshot = IDBRecord::create(realm, key, value, key);

            // 5. Append record snapshot to list.
            MUST(list->create_data_property_or_throw(i, record_snapshot));
            break;
        }
        }
    }

    // 5. Return list.
    return list;
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-keys-from-an-object-store
GC::Ref<JS::Array> retrieve_multiple_keys_from_an_object_store(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    // 1. If count is not given or is 0 (zero), let count be infinity.
    if (count.has_value() && *count == 0)
        count = OptionalNone();

    // 2. Let records be a list containing the first count records in store’s list of records whose key is in range.
    auto records = store->first_n_in_range(range, count);

    // 3. Let list be an empty list.
    auto list = MUST(JS::Array::create(realm, records.size()));

    // 4. For each record of records:
    for (u32 i = 0; i < records.size(); ++i) {
        auto& record = records[i];

        // 1. Let entry be the result of converting a key to a value with record’s key.
        auto entry = convert_a_key_to_a_value(realm, record.key);

        // 2. Append entry to list.
        MUST(list->create_data_property_or_throw(i, entry));
    }

    // 5. Return list converted to a sequence<any>.
    return list;
}

// https://w3c.github.io/IndexedDB/#retrieve-a-referenced-value-from-an-index
JS::Value retrieve_a_referenced_value_from_an_index(JS::Realm& realm, GC::Ref<Index> index, GC::Ref<IDBKeyRange> range)
{
    // 1. Let record be the first record in index’s list of records whose key is in range, if any.
    auto record = index->first_in_range(range);

    // 2. If record was not found, return undefined.
    if (!record.has_value())
        return JS::js_undefined();

    // 3. Let serialized be record’s referenced value.
    auto serialized = index->referenced_value(*record);

    // 4. Return ! StructuredDeserialize(serialized, targetRealm).
    return MUST(HTML::structured_deserialize(realm.vm(), serialized, realm));
}

// https://w3c.github.io/IndexedDB/#retrieve-a-value-from-an-index
JS::Value retrieve_a_value_from_an_index(JS::Realm& realm, GC::Ref<Index> index, GC::Ref<IDBKeyRange> range)
{
    // 1. Let record be the first record in index’s list of records whose key is in range, if any.
    auto record = index->first_in_range(range);

    // 2. If record was not found, return undefined.
    if (!record.has_value())
        return JS::js_undefined();

    // 3. Return the result of converting a key to a value with record’s value.
    return convert_a_key_to_a_value(realm, record->value);
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-referenced-values-from-an-index
GC::Ref<JS::Array> retrieve_multiple_referenced_values_from_an_index(JS::Realm& realm, GC::Ref<Index> index, GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    // 1. If count is not given or is 0 (zero), let count be infinity.
    if (count.has_value() && *count == 0)
        count = OptionalNone();

    // 2. Let records be a list containing the first count records in index’s list of records whose key is in range.
    auto records = index->first_n_in_range(range, count);

    // 3. Let list be an empty list.
    auto list = MUST(JS::Array::create(realm, records.size()));

    // 4. For each record of records:
    for (u32 i = 0; i < records.size(); ++i) {
        auto& record = records[i];

        // 1. Let serialized be record’s referenced value.
        auto serialized = index->referenced_value(record);

        // 2. Let entry be ! StructuredDeserialize(serialized, targetRealm).
        auto entry = MUST(HTML::structured_deserialize(realm.vm(), serialized, realm));

        // 3. Append entry to list.
        MUST(list->create_data_property_or_throw(i, entry));
    }

    // 5. Return list converted to a sequence<any>.
    return list;
}

// https://w3c.github.io/IndexedDB/#retrieve-multiple-values-from-an-index
GC::Ref<JS::Array> retrieve_multiple_values_from_an_index(JS::Realm& realm, GC::Ref<Index> index, GC::Ref<IDBKeyRange> range, Optional<WebIDL::UnsignedLong> count)
{
    // 1. If count is not given or is 0 (zero), let count be infinity.
    if (count.has_value() && *count == 0)
        count = OptionalNone();

    // 2. Let records be a list containing the first count records in index’s list of records whose key is in range.
    auto records = index->first_n_in_range(range, count);

    // 3. Let list be an empty list.
    auto list = MUST(JS::Array::create(realm, records.size()));

    // 4. For each record of records:
    for (u32 i = 0; i < records.size(); ++i) {
        auto& record = records[i];

        // 1. Let entry be the result of converting a key to a value with record’s value.
        auto entry = convert_a_key_to_a_value(realm, record.value);

        // 2. Append entry to list.
        MUST(list->create_data_property_or_throw(i, entry));
    }

    // 7. Return list converted to a sequence<any>.
    return list;
}

// https://w3c.github.io/IndexedDB/#queue-a-database-task
void queue_a_database_task(GC::Ref<GC::Function<void()>> steps)
{
    // To queue a database task, perform queue a task on the database access task source.
    HTML::queue_a_task(HTML::Task::Source::DatabaseAccess, nullptr, nullptr, steps);
}

// https://w3c.github.io/IndexedDB/#cleanup-indexed-database-transactions
bool cleanup_indexed_database_transactions(GC::Ref<HTML::EventLoop> event_loop)
{
    bool has_matching_event_loop = false;

    Database::for_each_database([&has_matching_event_loop, event_loop](GC::Root<Database> const& database) {
        for (auto const& connection : database->associated_connections()) {
            for (auto const& transaction : connection->transactions()) {

                // 2. For each transaction transaction with cleanup event loop matching the current event loop:
                if (transaction->cleanup_event_loop() == event_loop) {
                    has_matching_event_loop = true;

                    // 1. Set transaction’s state to inactive.
                    transaction->set_state(IDBTransaction::TransactionState::Inactive);

                    // 2. Clear transaction’s cleanup event loop.
                    transaction->set_cleanup_event_loop(nullptr);
                }
            }
        }
    });

    // 1. If there are no transactions with cleanup event loop matching the current event loop, return false.
    // 3. Return true.
    return has_matching_event_loop;
}

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#potentially-valid-key-range
bool is_a_potentially_valid_key_range(JS::Realm& realm, JS::Value value)
{
    // 1. If value is a key range, return true.
    if (value.is_object() && is<IDBKeyRange>(value.as_object()))
        return true;

    // 2. Else if Type(value) is Number, return true.
    if (value.is_number())
        return true;

    // 3. Else if Type(value) is String, return true.
    if (value.is_string())
        return true;

    // 4. Else if value is a Date (has a [[DateValue]] internal slot), return true.
    if (value.is_object() && value.as_object().is_date())
        return true;

    // 5. Else if value is a buffer source type, return true.
    if (value.is_object() && (is<JS::TypedArrayBase>(value.as_object()) || is<JS::ArrayBuffer>(value.as_object()) || is<JS::DataView>(value.as_object())))
        return true;

    // 6. Else if value is an Array exotic object, return true.
    if (value.is_object() && MUST(value.is_array(realm.vm())))
        return true;

    // 7. Else return false.
    return false;
}

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#retrieve-multiple-items-from-an-index
GC::Ref<JS::Array> retrieve_multiple_items_from_an_index(JS::Realm& target_realm, GC::Ref<Index> index, GC::Ref<IDBKeyRange> range, RecordKind kind, Bindings::IDBCursorDirection direction, Optional<WebIDL::UnsignedLong> count)
{
    // 1. If count is not given or is 0 (zero), let count be infinity.
    if (count.has_value() && *count == 0)
        count = OptionalNone();

    // 2. Let records be a an empty list.
    GC::ConservativeVector<IndexRecord> records(target_realm.heap());

    // 3. Switching on direction:
    switch (direction) {
    // "next"
    case Bindings::IDBCursorDirection::Next: {
        // 1. Set records to the first count of index’s list of records whose key is in range.
        records.extend(index->first_n_in_range(range, count));
        break;
    }
    // "nextunique"
    case Bindings::IDBCursorDirection::Nextunique: {
        // 1. Let range records be a list containing the index’s list of records whose key is in range.
        auto range_records = index->first_n_in_range(range, OptionalNone());

        // 2. Let range records length be range records’s size.
        auto range_records_length = range_records.size();

        // 3. Let i be 0.
        size_t i = 0;

        // x. Append |range records[0]| to records.
        // FIXME: https://github.com/w3c/IndexedDB/issues/480
        if (range_records_length > 0)
            records.append(range_records[0]);

        // 4. While i is less than range records length, then:
        while (i < range_records_length - 1) {
            // 1. Increase i by 1.
            i++;

            // 2. if record’s size is equal to count, then break.
            if (records.size() == count)
                break;

            // 3. If the result of comparing two keys using the keys from |range records[i]| and |range records[i-1]| is equal, then continue.
            if (Key::equals(range_records[i].key, range_records[i - 1].key))
                continue;

            // 4. Else append |range records[i]| to records.
            records.append(range_records[i]);
        }

        break;
    }
    // "prev"
    case Bindings::IDBCursorDirection::Prev: {
        // 1. Set records to the last count of index’s list of records whose key is in range.
        records.extend(index->last_n_in_range(range, count));
        break;
    }
    // "prevunique"
    case Bindings::IDBCursorDirection::Prevunique: {
        // 1. Let range records be a list containing the index’s list of records whose key is in range.
        auto range_records = index->first_n_in_range(range, OptionalNone());

        // 2. Let range records length be range records’s size.
        auto range_records_length = range_records.size();

        // 3. Let i be 0.
        size_t i = 0;

        // x. Append |range records[0]| to records.
        // FIXME: https://github.com/w3c/IndexedDB/issues/480
        if (range_records_length > 0)
            records.append(range_records[0]);

        // 4. While i is less than range records length, then:
        while (i < range_records_length - 1) {
            // 1. Increase i by 1.
            i++;

            // 2. if record’s size is equal to count, then break.
            if (records.size() == count)
                break;

            // 3. If the result of comparing two keys using the keys from |range records[i]| and |range records[i-1]| is equal, then continue.
            if (Key::equals(range_records[i].key, range_records[i - 1].key))
                continue;

            // 4. Else prepend |range records[i]| to records.
            records.prepend(range_records[i]);
        }

        break;
    }
    }

    // 4. Let list be an empty list.
    auto list = MUST(JS::Array::create(target_realm, records.size()));

    // 5. For each record of records, switching on kind:
    for (u32 i = 0; i < records.size(); ++i) {
        auto& record = records[i];

        switch (kind) {
        // "key"
        case RecordKind::Key: {
            // 1. Let key be the result of converting a key to a value with record’s value.
            auto key = convert_a_key_to_a_value(target_realm, record.value);

            // 2. Append key to list.
            MUST(list->create_data_property_or_throw(i, key));
            break;
        }
        // "value"
        case RecordKind::Value: {
            // 1. Let serialized be record’s referenced value.
            auto serialized = index->referenced_value(record);

            // 2. Let value be ! StructuredDeserialize(serialized, targetRealm).
            auto value = MUST(HTML::structured_deserialize(target_realm.vm(), serialized, target_realm));

            // 3. Append value to list.
            MUST(list->create_data_property_or_throw(i, value));
            break;
        }

        // "record"
        case RecordKind::Record: {
            // 1. Let index key be the record’s key.
            auto index_key = record.key;

            // 2. Let key be the record’s value.
            auto key = record.value;

            // 3. Let serialized be record’s referenced value.
            auto serialized = index->referenced_value(record);

            // 4. Let value be ! StructuredDeserialize(serialized, targetRealm).
            auto value = MUST(HTML::structured_deserialize(target_realm.vm(), serialized, target_realm));

            // 5. Let record snapshot be a new record snapshot with its key set to index key, value set to value, and primary key set to key.
            auto record_snapshot = IDBRecord::create(target_realm, index_key, value, key);

            // 6. Append record snapshot to list.
            MUST(list->create_data_property_or_throw(i, record_snapshot));
            break;
        }
        }
    }

    // 6. Return list.
    return list;
}

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#create-a-request-to-retrieve-multiple-items
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> create_a_request_to_retrieve_multiple_items(JS::Realm& realm, IDBRequestSource source_handle, RecordKind kind, JS::Value query_or_options, Optional<WebIDL::UnsignedLong> count)
{
    auto& vm = realm.vm();

    // 1. Let source be an index or an object store from sourceHandle.
    // If sourceHandle is an index handle, then source is the index handle’s associated index.
    // Otherwise, source is the object store handle’s associated object store.
    auto source = source_handle.visit(
        [](Empty) -> Variant<GC::Ref<ObjectStore>, GC::Ref<Index>> { VERIFY_NOT_REACHED(); },
        [](GC::Ref<IDBCursor>) -> Variant<GC::Ref<ObjectStore>, GC::Ref<Index>> { VERIFY_NOT_REACHED(); },
        [](GC::Ref<IDBIndex> index) -> Variant<GC::Ref<ObjectStore>, GC::Ref<Index>> { return index->index(); },
        [](GC::Ref<IDBObjectStore> object_store) -> Variant<GC::Ref<ObjectStore>, GC::Ref<Index>> { return object_store->store(); });

    // FIXME: 2. If source has been deleted, throw an "InvalidStateError" DOMException.
    // FIXME: 3. If source is an index and source’s object store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. Let transaction be sourceHandle’s transaction.
    auto transaction = source_handle.visit(
        [](Empty) -> GC::Ref<IDBTransaction> { VERIFY_NOT_REACHED(); },
        [](GC::Ref<IDBCursor>) -> GC::Ref<IDBTransaction> { VERIFY_NOT_REACHED(); },
        [](GC::Ref<IDBIndex> index) -> GC::Ref<IDBTransaction> { return index->transaction(); },
        [](GC::Ref<IDBObjectStore> object_store) -> GC::Ref<IDBTransaction> { return object_store->transaction(); });

    // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while creating retrieve multiple items request"_utf16);

    // 6. Let range be a key range.
    GC::Ptr<IDBKeyRange> range;

    // 7. Let direction be "next".
    // FIXME: Spec bug: https://github.com/w3c/IndexedDB/pull/478
    Bindings::IDBCursorDirection direction = Bindings::IDBCursorDirection::Next;

    // 8. If running is a potentially valid key range with queryOrOptions is true, then:
    // AD-HOC: Check if query_or_options is null following https://github.com/w3c/IndexedDB/issues/475
    if (query_or_options.is_nullish() || is_a_potentially_valid_key_range(realm, query_or_options)) {
        // 1. Set range to the result of converting a value to a key range with queryOrOptions. Rethrow any exceptions.
        range = TRY(convert_a_value_to_a_key_range(realm, query_or_options));
    }

    // 9. Else:
    else {
        // 1. Set range to the result of converting a value to a key range with queryOrOptions["query"]. Rethrow any exceptions.
        range = TRY(convert_a_value_to_a_key_range(realm, TRY(query_or_options.get(vm, "query"_utf16))));

        // 2. Set count to query_or_options["count"].
        count = TRY(TRY(query_or_options.get(vm, "count"_utf16)).to_u32(vm));

        // 3. Set direction to query_or_options["direction"].
        auto direction_value = TRY(TRY(query_or_options.get(vm, "direction"_utf16)).to_string(vm));
        if (direction_value == "next")
            direction = Bindings::IDBCursorDirection::Next;
        else if (direction_value == "nextunique")
            direction = Bindings::IDBCursorDirection::Nextunique;
        else if (direction_value == "prev")
            direction = Bindings::IDBCursorDirection::Prev;
        else if (direction_value == "prevunique")
            direction = Bindings::IDBCursorDirection::Prevunique;
    }

    // 10. Let operation be an algorithm to run.
    auto operation = source.visit(
        // 11. If source is an index, set operation to retrieve multiple items from an index with targetRealm, source, range, kind, direction, and count if given.
        [&](GC::Ref<Index> index) {
            return GC::create_function(realm.heap(),
                [&realm, index, range, kind, direction, count]() -> WebIDL::ExceptionOr<JS::Value> {
                    return retrieve_multiple_items_from_an_index(realm, index, GC::Ref(*range), kind, direction, count);
                });
        },
        // 12. Else set operation to retrieve multiple items from an object store with targetRealm, source, range, kind, direction, and count if given.
        [&](GC::Ref<ObjectStore> object_store) {
            return GC::create_function(realm.heap(),
                [&realm, object_store, range, kind, direction, count]() -> WebIDL::ExceptionOr<JS::Value> {
                    return retrieve_multiple_items_from_an_object_store(realm, object_store, GC::Ref(*range), kind, direction, count);
                });
        });

    // 13. Return the result (an IDBRequest) of running asynchronously execute a request with sourceHandle and operation.
    auto result = asynchronously_execute_a_request(realm, source_handle, operation);
    dbgln_if(IDB_DEBUG, "Executing request for creating retrieve multiple items request with uuid {}", result->uuid());
    return result;
}

}
