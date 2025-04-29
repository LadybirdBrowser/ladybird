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

using CursorSource = Variant<GC::Ref<IDBObjectStore>, GC::Ref<IDBIndex>>;

// https://w3c.github.io/IndexedDB/#cursor-interface
class IDBCursor : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(IDBCursor, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(IDBCursor);

public:
    virtual ~IDBCursor() override;
    [[nodiscard]] static GC::Ref<IDBCursor> create(JS::Realm&, GC::Ref<IDBTransaction>, GC::Ptr<Key>, Bindings::IDBCursorDirection, bool, GC::Ptr<Key>, JS::Value, CursorSource, GC::Ref<IDBKeyRange>, bool);

    [[nodiscard]] CursorSource source() { return m_source; }
    [[nodiscard]] Bindings::IDBCursorDirection direction() { return m_direction; }
    [[nodiscard]] JS::Value key();
    [[nodiscard]] JS::Value value() { return m_value.value_or(JS::js_undefined()); }
    [[nodiscard]] GC::Ptr<IDBRequest> request() { return m_request; }
    [[nodiscard]] GC::Ref<IDBTransaction> transaction() { return m_transaction; }
    [[nodiscard]] GC::Ref<IDBKeyRange> range() { return m_range; }
    [[nodiscard]] GC::Ptr<Key> position() { return m_position; }
    [[nodiscard]] GC::Ptr<Key> object_store_position() { return m_object_store_position; }
    [[nodiscard]] bool key_only() const { return m_key_only; }
    [[nodiscard]] bool got_value() const { return m_got_value; }

    void set_request(GC::Ptr<IDBRequest> request) { m_request = request; }
    void set_position(GC::Ptr<Key> position) { m_position = position; }
    void set_got_value(bool got_value) { m_got_value = got_value; }
    void set_key(GC::Ptr<Key> key) { m_key = key; }
    void set_value(JS::Value value) { m_value = value; }
    void set_object_store_position(GC::Ptr<Key> object_store_position) { m_object_store_position = object_store_position; }

protected:
    explicit IDBCursor(JS::Realm&, GC::Ref<IDBTransaction>, GC::Ptr<Key>, Bindings::IDBCursorDirection, bool, GC::Ptr<Key>, JS::Value, CursorSource, GC::Ref<IDBKeyRange>, bool);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor& visitor) override;

private:
    // A cursor has a transaction, the transaction that was active when the cursor was created.
    GC::Ref<IDBTransaction> m_transaction;

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

    // A cursor has a source that indicates which index or an object store is associated with the records over which the cursor is iterating.
    CursorSource m_source;

    // A cursor has a range of records in either an index or an object store.
    GC::Ref<IDBKeyRange> m_range;

    // A cursor has a request, which is the request used to open the cursor.
    GC::Ptr<IDBRequest> m_request;

    // A cursor also has a key only flag, that indicates whether the cursor’s value is exposed via the API.
    bool m_key_only { false };
};
}
