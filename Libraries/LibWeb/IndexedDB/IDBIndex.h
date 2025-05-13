/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/Internal/Index.h>

namespace Web::IndexedDB {

// https://w3c.github.io/IndexedDB/#index-interface
class IDBIndex : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBIndex, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBIndex);

public:
    virtual ~IDBIndex() override;
    [[nodiscard]] static GC::Ref<IDBIndex> create(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBObjectStore>);

    WebIDL::ExceptionOr<void> set_name(String const& value);
    String name() const { return m_name; }
    GC::Ref<IDBObjectStore> object_store() { return m_object_store_handle; }
    JS::Value key_path() const;
    bool multi_entry() const { return m_index->multi_entry(); }
    bool unique() const { return m_index->unique(); }

    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> get(JS::Value);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> get_key(JS::Value);
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<IDBRequest>> open_cursor(JS::Value, Bindings::IDBCursorDirection = Bindings::IDBCursorDirection::Next);

    // The transaction of an index handle is the transaction of its associated object store handle.
    GC::Ref<IDBTransaction> transaction() { return m_object_store_handle->transaction(); }
    GC::Ref<Index> index() { return m_index; }

protected:
    explicit IDBIndex(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBObjectStore>);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // An index handle has an associated index and an associated object store handle.
    GC::Ref<Index> m_index;
    GC::Ref<IDBObjectStore> m_object_store_handle;

    // An index handle has a name, which is initialized to the name of the associated index when the index handle is created.
    String m_name;
};

}
