/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/IDBCursorPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/Index.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

struct IDBIndexParameters {
    bool unique { false };
    bool multi_entry { false };
};

// https://w3c.github.io/IndexedDB/#object-store-interface
// https://w3c.github.io/IndexedDB/#object-store-handle-construct
class IDBObjectStore : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBObjectStore, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBObjectStore);

public:
    virtual ~IDBObjectStore() override;
    [[nodiscard]] static GC::Ref<IDBObjectStore> create(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBTransaction>);

    String name() const;
    WebIDL::ExceptionOr<void> set_name(String const& value);
    JS::Value key_path() const;
    [[nodiscard]] GC::Ref<HTML::DOMStringList> index_names();
    GC::Ref<IDBTransaction> transaction() const;
    bool auto_increment() const;

    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> put(JS::Value value, Optional<JS::Value> const& key);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> add(JS::Value value, Optional<JS::Value> const& key);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> delete_(JS::Value);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> clear();
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> get(JS::Value);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> get_key(JS::Value);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> get_all(Optional<JS::Value>, Optional<WebIDL::UnsignedLong>);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> get_all_keys(Optional<JS::Value>, Optional<WebIDL::UnsignedLong>);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> count(Optional<JS::Value>);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> open_cursor(JS::Value, Bindings::IDBCursorDirection = Bindings::IDBCursorDirection::Next);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> open_key_cursor(JS::Value, Bindings::IDBCursorDirection = Bindings::IDBCursorDirection::Next);

    WebIDL::ExceptionOr<GC::Ref<IDBIndex>> index(String const&);

    WebIDL::ExceptionOr<GC::Ref<IDBIndex>> create_index(String const&, KeyPath, IDBIndexParameters options);
    WebIDL::ExceptionOr<void> delete_index(String const&);

    AK::HashMap<String, GC::Ref<Index>>& index_set() { return m_indexes; }
    WebIDL::ExceptionOr<GC::Ref<IDBRequest>> add_or_put(GC::Ref<IDBObjectStore>, JS::Value, Optional<JS::Value> const&, bool);
    GC::Ref<ObjectStore> store() const { return m_store; }

protected:
    explicit IDBObjectStore(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBTransaction>);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // An object store handle has an associated object store and an associated transaction.
    GC::Ref<ObjectStore> m_store;
    GC::Ref<IDBTransaction> m_transaction;

    // An object store handle has a name, which is initialized to the name of the associated object store when the object store handle is created.
    String m_name;

    // An object store handle has an index set
    AK::HashMap<String, GC::Ref<Index>> m_indexes;
};

}
