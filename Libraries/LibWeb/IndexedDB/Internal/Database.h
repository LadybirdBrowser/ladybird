/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

// https://www.w3.org/TR/IndexedDB/#database-construct
class Database : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Database, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Database);

public:
    void set_version(u64 version) { m_version = version; }
    u64 version() const { return m_version; }
    String name() const { return m_name; }

    void set_upgrade_transaction(GC::Ptr<IDBTransaction> transaction) { m_upgrade_transaction = transaction; }
    [[nodiscard]] GC::Ptr<IDBTransaction> upgrade_transaction() { return m_upgrade_transaction; }

    void associate(GC::Ref<IDBDatabase> connection) { m_associated_connections.append(connection); }
    ReadonlySpan<GC::Ref<IDBDatabase>> associated_connections() { return m_associated_connections; }
    Vector<GC::Root<IDBDatabase>> associated_connections_except(IDBDatabase& connection)
    {
        Vector<GC::Root<IDBDatabase>> connections;
        for (auto& associated_connection : m_associated_connections) {
            if (associated_connection != &connection)
                connections.append(associated_connection);
        }
        return connections;
    }

    ReadonlySpan<GC::Ref<ObjectStore>> object_stores() { return m_object_stores; }
    GC::Ptr<ObjectStore> object_store_with_name(String const& name) const;
    void add_object_store(GC::Ref<ObjectStore> object_store) { m_object_stores.append(object_store); }
    void remove_object_store(GC::Ref<ObjectStore> object_store)
    {
        m_object_stores.remove_first_matching([&](auto& entry) { return entry == object_store; });
    }

    [[nodiscard]] static Vector<GC::Root<Database>> for_key(StorageAPI::StorageKey const&);
    [[nodiscard]] static Optional<GC::Root<Database> const&> for_key_and_name(StorageAPI::StorageKey&, String&);
    [[nodiscard]] static ErrorOr<GC::Root<Database>> create_for_key_and_name(JS::Realm&, StorageAPI::StorageKey&, String&);
    [[nodiscard]] static ErrorOr<void> delete_for_key_and_name(StorageAPI::StorageKey&, String&);

    [[nodiscard]] static GC::Ref<Database> create(JS::Realm&, String const&);
    virtual ~Database();

protected:
    explicit Database(IDBDatabase& database);

    explicit Database(JS::Realm& realm, String name)
        : PlatformObject(realm)
        , m_name(move(name))
    {
    }

    virtual void visit_edges(Visitor&) override;

private:
    Vector<GC::Ref<IDBDatabase>> m_associated_connections;

    // A database has a name which identifies it within a specific storage key.
    String m_name;

    // A database has a version. When a database is first created, its version is 0 (zero).
    u64 m_version { 0 };

    // A database has at most one associated upgrade transaction, which is either null or an upgrade transaction, and is initially null.
    GC::Ptr<IDBTransaction> m_upgrade_transaction;

    // A database has zero or more object stores which hold the data stored in the database.
    Vector<GC::Ref<ObjectStore>> m_object_stores;
};

}
