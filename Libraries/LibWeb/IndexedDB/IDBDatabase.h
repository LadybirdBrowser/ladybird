/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

using KeyPath = Variant<String, Vector<String>>;

// https://w3c.github.io/IndexedDB/#dictdef-idbobjectstoreparameters
struct IDBObjectStoreParameters {
    Optional<KeyPath> key_path;
    bool auto_increment { false };
};

// FIXME: I'm not sure if this object should do double duty as both the connection and the interface
//        but the spec treats it as such...?
// https://w3c.github.io/IndexedDB/#IDBDatabase-interface
// https://www.w3.org/TR/IndexedDB/#database-connection
class IDBDatabase : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(IDBDatabase, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(IDBDatabase);

    enum ConnectionState {
        Open,
        Closed,
    };

public:
    virtual ~IDBDatabase() override;

    [[nodiscard]] static GC::Ref<IDBDatabase> create(JS::Realm&, Database&);

    void set_version(u64 version) { m_version = version; }
    void set_close_pending(bool close_pending) { m_close_pending = close_pending; }
    void set_state(ConnectionState state) { m_state = state; }

    [[nodiscard]] GC::Ref<HTML::DOMStringList> object_store_names() { return m_object_store_names; }
    [[nodiscard]] String name() const { return m_name; }
    [[nodiscard]] u64 version() const { return m_version; }
    [[nodiscard]] bool close_pending() const { return m_close_pending; }
    [[nodiscard]] ConnectionState state() const { return m_state; }
    [[nodiscard]] GC::Ref<Database> associated_database() { return m_associated_database; }

    WebIDL::ExceptionOr<GC::Ref<IDBObjectStore>> create_object_store(String const&, IDBObjectStoreParameters const&);

    void close();

    void set_onabort(WebIDL::CallbackType*);
    WebIDL::CallbackType* onabort();
    void set_onclose(WebIDL::CallbackType*);
    WebIDL::CallbackType* onclose();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();
    void set_onversionchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onversionchange();

protected:
    explicit IDBDatabase(JS::Realm&, Database&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    u64 m_version { 0 };
    String m_name;
    GC::Ref<HTML::DOMStringList> m_object_store_names;

    // Each connection has a close pending flag which is initially false.
    bool m_close_pending { false };
    // When a connection is initially created it is in an opened state.
    ConnectionState m_state { ConnectionState::Open };

    // NOTE: There is an associated database in the spec, but there is no mention where it is assigned, nor where its from
    //       So we stash the one we have when opening a connection.
    GC::Ref<Database> m_associated_database;
};

}
