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

protected:
    explicit IDBObjectStore(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBTransaction>);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // An object store handle has an associated object store and an associated transaction.
    GC::Ref<ObjectStore> m_store;
    GC::Ref<IDBTransaction> m_transaction;
};

}
