/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#object-store-interface
// https://w3c.github.io/IndexedDB/#object-store-handle-construct
class IDBObjectStore : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBObjectStore, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBObjectStore);

public:
    virtual ~IDBObjectStore() override;
    [[nodiscard]] static GC::Ref<IDBObjectStore> create(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBTransaction>);

    JS::Value key_path() const;
    GC::Ref<IDBTransaction> transaction() const { return m_transaction; }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-autoincrement
    // The autoIncrement getter steps are to return true if this’s object store has a key generator, and false otherwise.
    bool auto_increment() const { return m_store->key_generator().has_value(); }
    String name() const { return m_name; }
    WebIDL::ExceptionOr<void> set_name(String const& value);

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
};

}
