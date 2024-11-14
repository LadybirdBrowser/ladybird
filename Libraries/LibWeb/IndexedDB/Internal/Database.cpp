/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/ConnectionQueueHandler.h>
#include <LibWeb/IndexedDB/Internal/Database.h>

namespace Web::IndexedDB {

using IDBDatabaseMapping = HashMap<StorageAPI::StorageKey, HashMap<String, GC::Root<Database>>>;
static IDBDatabaseMapping m_databases;

GC_DEFINE_ALLOCATOR(Database);

Database::~Database() = default;

GC::Ref<Database> Database::create(JS::Realm& realm, String const& name)
{
    return realm.create<Database>(realm, name);
}

void Database::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_connections);
}

ConnectionQueue& ConnectionQueueHandler::for_key_and_name(StorageAPI::StorageKey& key, String& name)
{
    return ConnectionQueueHandler::the().m_open_requests.ensure(key, [] {
                                                            return HashMap<String, ConnectionQueue>();
                                                        })
        .ensure(name, [] {
            return ConnectionQueue();
        });
}

Optional<GC::Root<Database>> Database::for_key_and_name(StorageAPI::StorageKey& key, String& name)
{
    return m_databases.ensure(key, [] {
                          return HashMap<String, GC::Root<Database>>();
                      })
        .get(name);
}

ErrorOr<GC::Root<Database>> Database::create_for_key_and_name(JS::Realm& realm, StorageAPI::StorageKey& key, String& name)
{
    auto database_mapping = TRY(m_databases.try_ensure(key, [] {
        return HashMap<String, GC::Root<Database>>();
    }));

    return database_mapping.try_ensure(name, [&] {
        return Database::create(realm, name);
    });
}

ErrorOr<void> Database::delete_for_key_and_name(StorageAPI::StorageKey& key, String& name)
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

    return {};
}

}
