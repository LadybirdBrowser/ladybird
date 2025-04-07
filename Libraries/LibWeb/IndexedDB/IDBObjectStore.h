/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
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

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-autoincrement
    // The autoIncrement getter steps are to return true if this’s object store has a key generator, and false otherwise.
    bool auto_increment() const { return m_store->key_generator().has_value(); }
    JS::Value key_path() const;
    String name() const { return m_name; }
    WebIDL::ExceptionOr<void> set_name(String const& value);
    GC::Ref<IDBTransaction> transaction() const { return m_transaction; }
    GC::Ref<ObjectStore> store() const { return m_store; }
    AK::HashMap<String, GC::Ref<Index>>& index_set() { return m_indexes; }

    WebIDL::ExceptionOr<GC::Ref<IDBIndex>> create_index(String const&, KeyPath, IDBIndexParameters options);
    [[nodiscard]] GC::Ref<HTML::DOMStringList> index_names();
    WebIDL::ExceptionOr<GC::Ref<IDBIndex>> index(String const&);

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
