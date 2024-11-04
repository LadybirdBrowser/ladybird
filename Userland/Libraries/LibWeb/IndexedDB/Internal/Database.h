/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

// https://www.w3.org/TR/IndexedDB/#database-construct
class Database : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Database, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Database);

public:
    void set_version(u64 version) { m_version = version; }
    u64 version() const { return m_version; }
    String name() const { return m_name; }

    void associate(JS::NonnullGCPtr<IDBDatabase> connection) { m_associated_connections.append(connection); }
    ReadonlySpan<JS::NonnullGCPtr<IDBDatabase>> associated_connections() { return m_associated_connections; }
    Vector<JS::Handle<IDBDatabase>> associated_connections_except(IDBDatabase& connection)
    {
        Vector<JS::Handle<IDBDatabase>> connections;
        for (auto& associated_connection : m_associated_connections) {
            if (associated_connection != &connection)
                connections.append(associated_connection);
        }
        return connections;
    }

    [[nodiscard]] static JS::NonnullGCPtr<Database> create(JS::Realm&, String const&);
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
    Vector<JS::NonnullGCPtr<IDBDatabase>> m_associated_connections;

    // FIXME: A database has zero or more object stores which hold the data stored in the database.

    // A database has a name which identifies it within a specific storage key.
    String m_name;

    // A database has a version. When a database is first created, its version is 0 (zero).
    u64 m_version { 0 };

    // FIXME: A database has at most one associated upgrade transaction, which is either null or an upgrade transaction, and is initially null.
};

}
