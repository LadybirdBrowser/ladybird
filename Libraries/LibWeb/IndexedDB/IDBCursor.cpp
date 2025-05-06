/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBCursorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBCursor.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBCursor);

IDBCursor::~IDBCursor() = default;

IDBCursor::IDBCursor(JS::Realm& realm, CursorSourceHandle source_handle, GC::Ptr<Key> position, Bindings::IDBCursorDirection direction, bool got_value, GC::Ptr<Key> key, JS::Value value, GC::Ref<IDBKeyRange> range, bool key_only)
    : PlatformObject(realm)
    , m_position(position)
    , m_direction(direction)
    , m_got_value(got_value)
    , m_key(key)
    , m_value(value)
    , m_source_handle(source_handle)
    , m_range(range)
    , m_key_only(key_only)
{
}

GC::Ref<IDBCursor> IDBCursor::create(JS::Realm& realm, CursorSourceHandle source_handle, GC::Ptr<Key> position, Bindings::IDBCursorDirection direction, bool got_value, GC::Ptr<Key> key, JS::Value value, GC::Ref<IDBKeyRange> range, bool key_only)
{
    return realm.create<IDBCursor>(realm, source_handle, position, direction, got_value, key, value, range, key_only);
}

void IDBCursor::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBCursor);
    Base::initialize(realm);
}

void IDBCursor::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_position);
    visitor.visit(m_object_store_position);
    visitor.visit(m_key);
    visitor.visit(m_range);
    visitor.visit(m_request);

    m_source_handle.visit([&](auto& source) {
        visitor.visit(source);
    });
}

// https://w3c.github.io/IndexedDB/#cursor-transaction
GC::Ref<IDBTransaction> IDBCursor::transaction()
{
    // A cursor has a transaction, which is the transaction from the cursor’s source handle.
    return m_source_handle.visit(
        [](GC::Ref<IDBObjectStore> object_store) { return object_store->transaction(); },
        [](GC::Ref<IDBIndex> index) { return index->transaction(); });
}

// https://w3c.github.io/IndexedDB/#cursor-source
CursorSource IDBCursor::internal_source()
{
    // A cursor has a source, which is an index or an object store from the cursor’s source handle.
    return m_source_handle.visit(
        [](GC::Ref<IDBObjectStore> object_store) -> CursorSource { return object_store->store(); },
        [](GC::Ref<IDBIndex> index) -> CursorSource { return index->index(); });
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-key
JS::Value IDBCursor::key()
{
    // The key getter steps are to return the result of converting a key to a value with the cursor’s current key.
    if (!m_key)
        return JS::js_undefined();

    return convert_a_key_to_a_value(realm(), *m_key);
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-continue
WebIDL::ExceptionOr<void> IDBCursor::continue_(JS::Value key)
{
    auto& realm = this->realm();

    // 1. Let transaction be this's transaction.
    auto transaction = this->transaction();

    // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while continuing cursor"_string);

    // FIXME: 3. If this's source or effective object store has been deleted, throw an "InvalidStateError" DOMException

    // 4. If this's got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
    if (!m_got_value)
        return WebIDL::InvalidStateError::create(realm, "Cursor is active or EOL"_string);

    // 5. If key is given, then:
    GC::Ptr<Key> key_value;
    if (!key.is_undefined()) {
        // 1. Let r be the result of converting a value to a key with key. Rethrow any exceptions.
        auto r = TRY(convert_a_value_to_a_key(realm, key));

        // 2. If r is invalid, throw a "DataError" DOMException.
        if (r->is_invalid())
            return WebIDL::DataError::create(realm, r->value_as_string());

        // 3. Let key be r.
        key_value = r;

        // 4. If key is less than or equal to this's position and this's direction is "next" or "nextunique", then throw a "DataError" DOMException.
        auto is_less_than_or_equal_to = Key::less_than(*key_value, *this->position()) || Key::equals(*key_value, *this->position());
        if (is_less_than_or_equal_to && (m_direction == Bindings::IDBCursorDirection::Next || m_direction == Bindings::IDBCursorDirection::Nextunique))
            return WebIDL::DataError::create(realm, "Key is less than or equal to cursor's position"_string);

        // 5. If key is greater than or equal to this's position and this's direction is "prev" or "prevunique", then throw a "DataError" DOMException.
        auto is_greater_than_or_equal_to = Key::greater_than(*key_value, *this->position()) || Key::equals(*key_value, *this->position());
        if (is_greater_than_or_equal_to && (m_direction == Bindings::IDBCursorDirection::Prev || m_direction == Bindings::IDBCursorDirection::Prevunique))
            return WebIDL::DataError::create(realm, "Key is greater than or equal to cursor's position"_string);
    }

    // 6. Set this's got value flag to false.
    m_got_value = false;

    // 7. Let request be this's request.
    auto request = this->request();

    // 8. Set request’s processed flag to false.
    request->set_processed(false);

    // 9. Set request’s done flag to false.
    request->set_done(false);

    // 10. Let operation be an algorithm to run iterate a cursor with the current Realm record, this, and key (if given).
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [this, &realm, key_value] -> WebIDL::ExceptionOr<JS::Value> {
        return WebIDL::ExceptionOr<JS::Value>(iterate_a_cursor(realm, *this, key_value));
    });

    // 11. Run asynchronously execute a request with this’s source handle, operation, and request.
    asynchronously_execute_a_request(realm, source_handle(), operation, request);
    dbgln_if(IDB_DEBUG, "Executing request for cursor continue with uuid {}", request->uuid());

    return {};
}

}
