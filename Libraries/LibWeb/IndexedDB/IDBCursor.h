/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/IDBCursorPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/IndexedDB/IDBIndex.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/Key.h>

namespace Web::IndexedDB {

using CursorSourceHandle = Variant<GC::Ref<IDBObjectStore>, GC::Ref<IDBIndex>>;
using CursorSource = Variant<GC::Ref<ObjectStore>, GC::Ref<Index>>;

// https://w3c.github.io/IndexedDB/#cursor-interface
class IDBCursor : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBCursor, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBCursor);

    enum class GotValue {
        No,
        Yes,
    };

    enum class KeyOnly {
        No,
        Yes,
    };

public:
    virtual ~IDBCursor() override;
    [[nodiscard]] static GC::Ref<IDBCursor> create(JS::Realm&, CursorSourceHandle, GC::Ptr<Key>, Bindings::IDBCursorDirection, GotValue, GC::Ptr<Key>, JS::Value, GC::Ref<IDBKeyRange>, KeyOnly);

    [[nodiscard]] CursorSourceHandle source_handle() { return m_source_handle; }
    [[nodiscard]] Bindings::IDBCursorDirection direction() { return m_direction; }
    [[nodiscard]] JS::Value key();
    [[nodiscard]] JS::Value primary_key() const;
    [[nodiscard]] GC::Ptr<IDBRequest> request() { return m_request; }

    WebIDL::ExceptionOr<void> advance(WebIDL::UnsignedLong);
    WebIDL::ExceptionOr<void> continue_(JS::Value);
    WebIDL::ExceptionOr<void> continue_primary_key(JS::Value, JS::Value);

    [[nodiscard]] JS::Value value() { return m_value.value_or(JS::js_undefined()); }
    [[nodiscard]] GC::Ref<IDBKeyRange> range() { return m_range; }
    [[nodiscard]] GC::Ptr<Key> position() { return m_position; }
    [[nodiscard]] GC::Ptr<Key> object_store_position() { return m_object_store_position; }
    [[nodiscard]] bool key_only() const { return m_key_only; }
    [[nodiscard]] bool got_value() const { return m_got_value; }
    [[nodiscard]] GC::Ref<IDBTransaction> transaction();
    [[nodiscard]] CursorSource internal_source();
    [[nodiscard]] GC::Ref<Key> effective_key() const;

    void set_request(GC::Ptr<IDBRequest> request) { m_request = request; }
    void set_position(GC::Ptr<Key> position) { m_position = position; }
    void set_got_value(bool got_value) { m_got_value = got_value; }
    void set_key(GC::Ptr<Key> key) { m_key = key; }
    void set_value(JS::Value value) { m_value = value; }
    void set_object_store_position(GC::Ptr<Key> object_store_position) { m_object_store_position = object_store_position; }

protected:
    explicit IDBCursor(JS::Realm&, CursorSourceHandle, GC::Ptr<Key>, Bindings::IDBCursorDirection, GotValue, GC::Ptr<Key>, JS::Value, GC::Ref<IDBKeyRange>, KeyOnly);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // A cursor has a position within its range.
    GC::Ptr<Key> m_position;

    // When iterating indexes the cursor also has an object store position
    GC::Ptr<Key> m_object_store_position;

    // A cursor has a direction that determines whether it moves in monotonically increasing or decreasing order of the record keys when iterated, and if it skips duplicated values when iterating indexes.
    Bindings::IDBCursorDirection m_direction;

    // A cursor has a got value flag.
    bool m_got_value { false };

    // A cursor has a key and a value which represent the key and the value of the last iterated record.
    GC::Ptr<Key> m_key;
    Optional<JS::Value> m_value;

    // A cursor has a source handle, which is the index handle or the object store handle that opened the cursor.
    CursorSourceHandle m_source_handle;

    // A cursor has a range of records in either an index or an object store.
    GC::Ref<IDBKeyRange> m_range;

    // A cursor has a request, which is the request used to open the cursor.
    GC::Ptr<IDBRequest> m_request;

    // A cursor also has a key only flag, that indicates whether the cursor’s value is exposed via the API.
    bool m_key_only { false };
};
}
