/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBCursorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBCursor.h>
#include <LibWeb/IndexedDB/IDBCursorWithValue.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBCursor);

IDBCursor::~IDBCursor() = default;

IDBCursor::IDBCursor(JS::Realm& realm, CursorSourceHandle source_handle, GC::Ptr<Key> position, Bindings::IDBCursorDirection direction, GotValue got_value, GC::Ptr<Key> key, JS::Value value, GC::Ref<IDBKeyRange> range, KeyOnly key_only)
    : PlatformObject(realm)
    , m_value(value)
    , m_position(position)
    , m_direction(direction)
    , m_got_value(got_value == GotValue::Yes)
    , m_key(key)
    , m_source_handle(source_handle)
    , m_range(range)
    , m_key_only(key_only == KeyOnly::Yes)
{
}

GC::Ref<IDBCursor> IDBCursor::create(JS::Realm& realm, CursorSourceHandle source_handle, GC::Ptr<Key> position, Bindings::IDBCursorDirection direction, GotValue got_value, GC::Ptr<Key> key, JS::Value value, GC::Ref<IDBKeyRange> range, KeyOnly key_only)
{
    // A cursor that has its key only flag set to false implements the IDBCursorWithValue interface as well.
    if (key_only == KeyOnly::No)
        return realm.create<IDBCursorWithValue>(realm, source_handle, position, direction, got_value, key, value, range, key_only);

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

GC_DEFINE_ALLOCATOR(IDBCursorWithValue);

IDBCursorWithValue::~IDBCursorWithValue() = default;

IDBCursorWithValue::IDBCursorWithValue(JS::Realm& realm, CursorSourceHandle source_handle, GC::Ptr<Key> position, Bindings::IDBCursorDirection direction, GotValue got_value, GC::Ptr<Key> key, JS::Value value, GC::Ref<IDBKeyRange> range, KeyOnly key_only)
    : IDBCursor(realm, source_handle, position, direction, got_value, key, value, range, key_only)
{
}

void IDBCursorWithValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBCursorWithValue);
    Base::initialize(realm);
}

void IDBCursorWithValue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
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
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while continuing cursor"_string);

    // FIXME: 3. If this's source or effective object store has been deleted, throw an "InvalidStateError" DOMException

    // 4. If this's got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
    if (!m_got_value)
        return WebIDL::InvalidStateError::create(realm, "Cursor is active or EOL while continuing"_string);

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

// https://w3c.github.io/IndexedDB/#cursor-effective-key
[[nodiscard]] GC::Ref<Key> IDBCursor::effective_key() const
{
    return m_source_handle.visit(
        [&](GC::Ref<IDBObjectStore>) -> GC::Ref<Key> {
            //  If the source of a cursor is an object store, the effective key of the cursor is the cursor’s position
            return *m_position;
        },
        [&](GC::Ref<IDBIndex>) -> GC::Ref<Key> {
            // If the source of a cursor is an index, the effective key is the cursor’s object store position.
            return *m_object_store_position;
        });
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-primarykey
JS::Value IDBCursor::primary_key() const
{
    // The primaryKey getter steps are to return the result of converting a key to a value with the cursor’s current effective key.
    return convert_a_key_to_a_value(realm(), effective_key());
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-advance
WebIDL::ExceptionOr<void> IDBCursor::advance(WebIDL::UnsignedLong count)
{
    auto& realm = this->realm();

    // 1. If count is 0 (zero), throw a TypeError.
    if (count == 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Count must not be zero (0)"_string };

    // 2. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 3. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while advancing cursor"_string);

    // FIXME: 4. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.

    // 5. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
    if (!m_got_value)
        return WebIDL::InvalidStateError::create(realm, "Cursor is active or EOL while advancing"_string);

    // 6. Set this’s got value flag to false.
    m_got_value = false;

    // 7. Let request be this’s request.
    auto request = this->request();

    // 8. Set request’s processed flag to false.
    request->set_processed(false);

    // 9. Set request’s done flag to false.
    request->set_done(false);

    // 10. Let operation be an algorithm to run iterate a cursor with the current Realm record, this, and count.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [this, &realm, count] -> WebIDL::ExceptionOr<JS::Value> {
        return WebIDL::ExceptionOr<JS::Value>(iterate_a_cursor(realm, *this, nullptr, nullptr, count));
    });

    // 11. Run asynchronously execute a request with this’s source handle, operation, and request.
    asynchronously_execute_a_request(realm, source_handle(), operation, request);
    dbgln_if(IDB_DEBUG, "Executing request for cursor advance with uuid {}", request->uuid());

    return {};
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-continueprimarykey
WebIDL::ExceptionOr<void> IDBCursor::continue_primary_key(JS::Value key_param, JS::Value primary_key_param)
{
    auto& realm = this->realm();

    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while continuing cursor"_string);

    // FIXME: 3. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If this’s source is not an index throw an "InvalidAccessError" DOMException.
    if (!m_source_handle.has<GC::Ref<IDBIndex>>())
        return WebIDL::InvalidAccessError::create(realm, "Cursor source is not an index"_string);

    // 5. If this’s direction is not "next" or "prev", throw an "InvalidAccessError" DOMException.
    if (m_direction != Bindings::IDBCursorDirection::Next && m_direction != Bindings::IDBCursorDirection::Prev)
        return WebIDL::InvalidAccessError::create(realm, "Cursor direction is not next or prev"_string);

    // 6. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
    if (!m_got_value)
        return WebIDL::InvalidStateError::create(realm, "Cursor is active or EOL while continuing"_string);

    // 7. Let r be the result of converting a value to a key with key. Rethrow any exceptions.
    auto r = TRY(convert_a_value_to_a_key(realm, key_param));

    // 8. If r is invalid, throw a "DataError" DOMException.
    if (r->is_invalid())
        return WebIDL::DataError::create(realm, r->value_as_string());

    // 9. Let key be r.
    auto key = r;

    // 10. Let r be the result of converting a value to a key with primaryKey. Rethrow any exceptions.
    r = TRY(convert_a_value_to_a_key(realm, primary_key_param));

    // 11. If r is invalid, throw a "DataError" DOMException.
    if (r->is_invalid())
        return WebIDL::DataError::create(realm, r->value_as_string());

    // 12. Let primaryKey be r.
    auto primary_key = r;

    // 13. If key is less than this’s position and this’s direction is "next", throw a "DataError" DOMException.
    if (Key::less_than(*key, *this->position()) && m_direction == Bindings::IDBCursorDirection::Next)
        return WebIDL::DataError::create(realm, "Key is less than cursor's position"_string);

    // 14. If key is greater than this’s position and this’s direction is "prev", throw a "DataError" DOMException.
    if (Key::greater_than(*key, *this->position()) && m_direction == Bindings::IDBCursorDirection::Prev)
        return WebIDL::DataError::create(realm, "Key is greater than cursor's position"_string);

    // 15. If key is equal to this’s position and primaryKey is less than or equal to this’s object store position and this’s direction is "next", throw a "DataError" DOMException.
    if (Key::equals(*key, *this->position()) && (Key::less_than(*primary_key, *this->object_store_position()) || Key::equals(*primary_key, *this->object_store_position())) && m_direction == Bindings::IDBCursorDirection::Next)
        return WebIDL::DataError::create(realm, "Key is equal to cursor's position"_string);

    // 16. If key is equal to this’s position and primaryKey is greater than or equal to this’s object store position and this’s direction is "prev", throw a "DataError" DOMException.
    if (Key::equals(*key, *this->position()) && (Key::greater_than(*primary_key, *this->object_store_position()) || Key::equals(*primary_key, *this->object_store_position())) && m_direction == Bindings::IDBCursorDirection::Prev)
        return WebIDL::DataError::create(realm, "Key is equal to cursor's position"_string);

    // 17. Set this’s got value flag to false.
    m_got_value = false;

    // 18. Let request be this’s request.
    auto request = this->request();

    // 19. Set request’s processed flag to false.
    request->set_processed(false);

    // 20. Set request’s done flag to false.
    request->set_done(false);

    // 21. Let operation be an algorithm to run iterate a cursor with the current Realm record, this, key, and primaryKey.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [this, &realm, key, primary_key] -> WebIDL::ExceptionOr<JS::Value> {
        return WebIDL::ExceptionOr<JS::Value>(iterate_a_cursor(realm, *this, key, primary_key));
    });

    // 22. Run asynchronously execute a request with this’s source handle, operation, and request.
    asynchronously_execute_a_request(realm, source_handle(), operation, request);
    dbgln_if(IDB_DEBUG, "Executing request for cursor continue with primary key with uuid {}", request->uuid());

    return {};
}

// https://w3c.github.io/IndexedDB/#cursor-effective-object-store
GC::Ref<ObjectStore> IDBCursor::effective_object_store() const
{
    return m_source_handle.visit(
        [&](GC::Ref<IDBObjectStore> store) -> GC::Ref<ObjectStore> {
            // If the source of a cursor is an object store, the effective object store of the cursor is that object store.
            return store->store();
        },
        [&](GC::Ref<IDBIndex> index) -> GC::Ref<ObjectStore> {
            // If the source of a cursor is an index, the effective object store of the cursor is that index’s referenced object store.
            return index->object_store()->store();
        });
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-update
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBCursor::update(JS::Value value)
{
    auto& realm = this->realm();

    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while updating cursor"_string);

    // 3. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
    if (transaction->is_readonly())
        return WebIDL::ReadOnlyError::create(realm, "Transaction is read-only while updating cursor"_string);

    // FIXME:  4. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.

    // 5. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
    if (!m_got_value)
        return WebIDL::InvalidStateError::create(realm, "Cursor is active or EOL while updating"_string);

    // 6. If this’s key only flag is true, throw an "InvalidStateError" DOMException.
    if (m_key_only)
        return WebIDL::InvalidStateError::create(realm, "Cursor is key-only while updating"_string);

    // 7. Let targetRealm be a user-agent defined Realm.
    // NOTE: this is 'realm' above

    // 8. Let clone be a clone of value in targetRealm during transaction. Rethrow any exceptions.
    auto clone = TRY(clone_in_realm(realm, value, transaction));

    // 9. If this’s effective object store uses in-line keys, then:
    auto effective_object_store = this->effective_object_store();
    if (effective_object_store->uses_inline_keys()) {
        // 1. Let kpk be the result of extracting a key from a value using a key path with clone and the key path of this’s effective object store. Rethrow any exceptions.
        auto kpk = TRY(extract_a_key_from_a_value_using_a_key_path(realm, clone, *effective_object_store->key_path()));

        // 2. If kpk is failure, invalid, or not equal to this’s effective key, throw a "DataError" DOMException.
        if (kpk.is_error())
            return WebIDL::DataError::create(realm, "Key path is invalid"_string);

        auto kpk_value = kpk.release_value();
        if (kpk_value->is_invalid())
            return WebIDL::DataError::create(realm, "Key path is invalid"_string);

        if (!Key::equals(*kpk_value, *this->effective_key()))
            return WebIDL::DataError::create(realm, "Key path is not equal to effective key"_string);
    }

    // 10. Let operation be an algorithm to run store a record into an object store with this’s effective object store, clone, this’s effective key, and false.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [this, &realm, clone] -> WebIDL::ExceptionOr<JS::Value> {
        auto optional_key = TRY(store_a_record_into_an_object_store(realm, *this->effective_object_store(), clone, this->effective_key(), false));

        if (!optional_key || optional_key->is_invalid())
            return JS::js_undefined();

        return convert_a_key_to_a_value(realm, GC::Ref(*optional_key));
    });

    // 11. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
    auto request = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for cursor update with uuid {}", request->uuid());
    return request;
}

// https://w3c.github.io/IndexedDB/#dom-idbcursor-update
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBCursor::delete_()
{
    auto& realm = this->realm();

    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while deleting cursor"_string);

    // 3. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
    if (transaction->is_readonly())
        return WebIDL::ReadOnlyError::create(realm, "Transaction is read-only while deleting cursor"_string);

    // FIXME: 4. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.

    // 5. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
    if (!m_got_value)
        return WebIDL::InvalidStateError::create(realm, "Cursor is active or EOL while deleting"_string);

    // 6. If this’s key only flag is true, throw an "InvalidStateError" DOMException.
    if (m_key_only)
        return WebIDL::InvalidStateError::create(realm, "Cursor is key-only while deleting"_string);

    // 7. Let operation be an algorithm to run delete records from an object store with this’s effective object store and this’s effective key.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [this, &realm] -> WebIDL::ExceptionOr<JS::Value> {
        auto effective_key = this->effective_key();
        auto range = IDBKeyRange::create(realm, effective_key, effective_key, IDBKeyRange::LowerOpen::No, IDBKeyRange::UpperOpen::No);
        return delete_records_from_an_object_store(*this->effective_object_store(), range);
    });

    // 8. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
    auto request = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for cursor delete with uuid {}", request->uuid());
    return request;
}

}
