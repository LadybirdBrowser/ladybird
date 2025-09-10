/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/ConnectionQueueHandler.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/IndexedDB/Internal/IDBDatabaseObserver.h>
#include <LibWeb/IndexedDB/Internal/RequestList.h>

namespace Web::IndexedDB {

using IDBDatabaseMapping = HashMap<StorageAPI::StorageKey, HashMap<String, GC::Root<Database>>>;
static IDBDatabaseMapping m_databases;

void Database::for_each_database(AK::Function<void(GC::Root<Database> const&)> const& visitor)
{
    for (auto const& [key, mapping] : m_databases) {
        for (auto const& database_mapping : mapping) {
            visitor(database_mapping.value);
        }
    }
}

GC_DEFINE_ALLOCATOR(Database);
GC_DEFINE_ALLOCATOR(Database::ConnectionCloseState);

Database::~Database() = default;

GC::Ref<Database> Database::create(JS::Realm& realm, String const& name)
{
    return realm.create<Database>(realm, name);
}

void Database::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_connections);
    visitor.visit(m_pending_connection_close_queue);
    visitor.visit(m_upgrade_transaction);
    visitor.visit(m_object_stores);
}

GC::Ptr<ObjectStore> Database::object_store_with_name(String const& name) const
{
    for (auto const& object_store : m_object_stores) {
        if (object_store->name() == name)
            return object_store;
    }

    return nullptr;
}

Vector<GC::Root<Database>> Database::for_key(StorageAPI::StorageKey const& key)
{
    Vector<GC::Root<Database>> databases;
    for (auto const& database_mapping : m_databases.get(key).value_or({})) {
        databases.append(database_mapping.value);
    }

    return databases;
}

RequestList& ConnectionQueueHandler::for_key_and_name(StorageAPI::StorageKey const& key, String const& name)
{
    return ConnectionQueueHandler::the().m_open_requests.ensure(key, [] {
                                                            return HashMap<String, RequestList>();
                                                        })
        .ensure(name, [] {
            return RequestList();
        });
}

Optional<GC::Root<Database> const&> Database::for_key_and_name(StorageAPI::StorageKey const& key, String const& name)
{
    return m_databases.ensure(key, [] {
                          return HashMap<String, GC::Root<Database>>();
                      })
        .get(name);
}

ErrorOr<GC::Root<Database>> Database::create_for_key_and_name(JS::Realm& realm, StorageAPI::StorageKey const& key, String const& name)
{
    auto database_mapping = TRY(m_databases.try_ensure(key, [] {
        return HashMap<String, GC::Root<Database>>();
    }));

    auto value = Database::create(realm, name);

    database_mapping.set(name, value);
    m_databases.set(key, database_mapping);

    return value;
}

ErrorOr<void> Database::delete_for_key_and_name(StorageAPI::StorageKey const& key, String const& name)
{
    // FIXME: Is a missing entry a failure?
    auto maybe_database_mapping = m_databases.get(key);
    if (!maybe_database_mapping.has_value())
        return {};

    auto& database_mapping = maybe_database_mapping.value();
    auto maybe_database = database_mapping.get(name);
    if (!maybe_database.has_value())
        return {};

    auto did_remove = database_mapping.remove(name);
    if (!did_remove)
        return {};

    m_databases.set(key, database_mapping);

    return {};
}

Vector<GC::Root<IDBDatabase>> Database::associated_connections()
{
    Vector<GC::Root<IDBDatabase>> connections;
    connections.ensure_capacity(m_associated_connections.size());
    for (auto& associated_connection : m_associated_connections) {
        connections.unchecked_append(associated_connection);
    }
    return connections;
}

Vector<GC::Root<IDBDatabase>> Database::associated_connections_except(IDBDatabase& connection)
{
    Vector<GC::Root<IDBDatabase>> connections;
    for (auto& associated_connection : m_associated_connections) {
        if (associated_connection != &connection)
            connections.append(associated_connection);
    }
    return connections;
}

void Database::ConnectionCloseState::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(database_observers);
    visitor.visit(after_all);
}

void Database::ConnectionCloseState::add_connection_to_observe(GC::Ref<IDBDatabase> database)
{
    auto database_observer = heap().allocate<IDBDatabaseObserver>(database);
    database_observer->set_connection_state_changed_observer(GC::create_function(heap(), [this] {
        VERIFY(!database_observers.is_empty());
        database_observers.remove_all_matching([](GC::Ref<IDBDatabaseObserver> const& database_observer) {
            if (database_observer->database()->state() == ConnectionState::Closed) {
                database_observer->unobserve();
                return true;
            }

            return false;
        });

        if (database_observers.is_empty()) {
            queue_a_database_task(after_all.as_nonnull());
        }
    }));

    database_observers.append(database_observer);
}

void Database::wait_for_connections_to_close(ReadonlySpan<GC::Root<IDBDatabase>> connections, GC::Ref<GC::Function<void()>> after_all)
{
    GC::Ptr<ConnectionCloseState> connection_close_state;

    for (auto const& entry : connections) {
        if (entry->state() != ConnectionState::Closed) {
            if (!connection_close_state) {
                connection_close_state = heap().allocate<ConnectionCloseState>();
            }

            connection_close_state->add_connection_to_observe(*entry.cell());
        }
    }

    if (connection_close_state) {
        connection_close_state->after_all = GC::create_function(heap(), [this, connection_close_state, after_all] {
            bool was_removed = m_pending_connection_close_queue.remove_first_matching([connection_close_state](GC::Ref<ConnectionCloseState> pending_connection_close_state) {
                return pending_connection_close_state == connection_close_state;
            });
            VERIFY(was_removed);
            queue_a_database_task(after_all);
        });
        m_pending_connection_close_queue.append(connection_close_state.as_nonnull());
    } else {
        queue_a_database_task(after_all);
    }
}

}
