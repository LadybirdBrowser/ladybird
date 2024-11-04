/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/ConnectionQueueHandler.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#open-a-database-connection
WebIDL::ExceptionOr<JS::NonnullGCPtr<IDBDatabase>> open_a_database_connection(JS::Realm& realm, StorageAPI::StorageKey storage_key, String name, Optional<u64> maybe_version, JS::NonnullGCPtr<IDBRequest> request)
{
    // 1. Let queue be the connection queue for storageKey and name.
    auto& queue = ConnectionQueueHandler::for_key_and_name(storage_key, name);

    // 2. Add request to queue.
    queue.append(request);

    // 3. Wait until all previous requests in queue have been processed.
    HTML::main_thread_event_loop().spin_until(JS::create_heap_function(realm.vm().heap(), [queue, request]() {
        return queue.all_previous_requests_processed(request);
    }));

    // 4. Let db be the database named name in storageKey, or null otherwise.
    auto maybe_db = Database::for_key_and_name(storage_key, name);
    JS::GCPtr<Database> db;

    // 5. If version is undefined, let version be 1 if db is null, or db’s version otherwise.
    auto version = maybe_version.value_or(maybe_db.has_value() ? maybe_db.value()->version() : 1);

    // 6. If db is null, let db be a new database with name name, version 0 (zero), and with no object stores.
    // If this fails for any reason, return an appropriate error (e.g. a "QuotaExceededError" or "UnknownError" DOMException).
    if (!maybe_db.has_value()) {
        auto maybe_database = Database::create_for_key_and_name(realm, storage_key, name);

        if (maybe_database.is_error()) {
            return WebIDL::OperationError::create(realm, "Unable to create a new database"_string);
        }

        db = maybe_database.release_value();
    }

    // 7. If db’s version is greater than version, return a newly created "VersionError" DOMException and abort these steps.
    if (db->version() > version) {
        return WebIDL::VersionError::create(realm, "Database version is greater than the requested version"_string);
    }

    // 8. Let connection be a new connection to db.
    auto connection = IDBDatabase::create(realm, *db);

    // 9. Set connection’s version to version.
    connection->set_version(version);

    // 10. If db’s version is less than version, then:
    if (db->version() < version) {
        // 1. Let openConnections be the set of all connections, except connection, associated with db.
        auto open_connections = db->associated_connections_except(connection);

        // FIXME: 2. For each entry of openConnections that does not have its close pending flag set to true,
        //    queue a task to fire a version change event named versionchange at entry with db’s version and version.
        for (auto& entry : open_connections) {
            if (!entry->close_pending()) {
            }
        }

        // FIXME: 3. Wait for all of the events to be fired.

        // FIXME: 4. If any of the connections in openConnections are still not closed,
        //    queue a task to fire a version change event named blocked at request with db’s version and version.
        for (auto& entry : open_connections) {
            if (entry->state() != IDBDatabase::ConnectionState::Closed) {
            }
        }

        // 5. Wait until all connections in openConnections are closed.
        HTML::main_thread_event_loop().spin_until(JS::create_heap_function(realm.vm().heap(), [open_connections]() {
            for (auto const& entry : open_connections) {
                if (entry->state() != IDBDatabase::ConnectionState::Closed) {
                    return false;
                }
            }

            return true;
        }));

        // FIXME: 6. Run upgrade a database using connection, version and request.
        // NOTE: upgrade a database sets this flag, so we set it manually temporarily.
        request->set_processed(true);

        // 7. If connection was closed, return a newly created "AbortError" DOMException and abort these steps.
        if (connection->state() == IDBDatabase::ConnectionState::Closed) {
            return WebIDL::AbortError::create(realm, "Connection was closed"_string);
        }

        // FIXME: 8. If the upgrade transaction was aborted, run the steps to close a database connection with connection,
        // return a newly created "AbortError" DOMException and abort these steps.
    }

    // 11. Return connection.
    return connection;
}

}
