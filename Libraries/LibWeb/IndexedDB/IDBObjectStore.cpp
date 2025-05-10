/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/IDBObjectStorePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBCursor.h>
#include <LibWeb/IndexedDB/IDBIndex.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBObjectStore);

IDBObjectStore::~IDBObjectStore() = default;

IDBObjectStore::IDBObjectStore(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBTransaction> transaction)
    : PlatformObject(realm)
    , m_store(store)
    , m_transaction(transaction)
    , m_name(store->name())
{
    transaction->add_to_scope(store);

    // An object store handle has an index set, which is initialized to the set of indexes that reference the associated object store when the object store handle is created.
    m_indexes = MUST(store->index_set().clone());
}

GC::Ref<IDBObjectStore> IDBObjectStore::create(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBTransaction> transaction)
{
    return realm.create<IDBObjectStore>(realm, store, transaction);
}

void IDBObjectStore::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBObjectStore);
    Base::initialize(realm);
}

void IDBObjectStore::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_store);
    visitor.visit(m_transaction);
    visitor.visit(m_indexes);
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-keypath
JS::Value IDBObjectStore::key_path() const
{
    if (!m_store->key_path().has_value())
        return JS::js_null();

    return m_store->key_path().value().visit(
        [&](String const& value) -> JS::Value {
            return JS::PrimitiveString::create(realm().vm(), value);
        },
        [&](Vector<String> const& value) -> JS::Value {
            return JS::Array::create_from<String>(realm(), value.span(), [&](auto const& entry) -> JS::Value {
                return JS::PrimitiveString::create(realm().vm(), entry);
            });
        });
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-name
WebIDL::ExceptionOr<void> IDBObjectStore::set_name(String const& value)
{
    auto& realm = this->realm();

    // 1. Let name be the given value.
    auto const& name = value;

    // 2. Let transaction be this’s transaction.
    auto& transaction = m_transaction;

    // 3. Let store be this’s object store.
    auto& store = m_store;

    // FIXME: 4. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 5. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
    if (transaction->mode() != Bindings::IDBTransactionMode::Versionchange)
        return WebIDL::InvalidStateError::create(realm, "Attempted to set name outside of version change"_string);

    // 6. If transaction’s state is not active, throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while updating object store name"_string);

    // 7. If store’s name is equal to name, terminate these steps.
    if (store->name() == name)
        return {};

    // 8. If an object store named name already exists in store’s database, throw a "ConstraintError" DOMException.
    if (store->database()->object_store_with_name(name))
        return WebIDL::ConstraintError::create(realm, "Object store with the given name already exists"_string);

    // 9. Set store’s name to name.
    store->set_name(name);

    // 10. Set this’s name to name.
    m_name = name;

    return {};
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
WebIDL::ExceptionOr<GC::Ref<IDBIndex>> IDBObjectStore::create_index(String const& name, KeyPath key_path, IDBIndexParameters options)
{
    auto& realm = this->realm();

    // 1. Let transaction be this's transaction.
    auto transaction = this->transaction();

    // 2. Let store be this's object store.
    auto store = this->store();

    // 3. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
    if (transaction->mode() != Bindings::IDBTransactionMode::Versionchange)
        return WebIDL::InvalidStateError::create(realm, "Transaction is not an upgrade transaction"_string);

    // FIXME: 4. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while creating index"_string);

    // 6. If an index named name already exists in store, throw a "ConstraintError" DOMException.
    if (store->index_set().contains(name))
        return WebIDL::ConstraintError::create(realm, "An index with the given name already exists"_string);

    // 7. If keyPath is not a valid key path, throw a "SyntaxError" DOMException.
    if (!is_valid_key_path(key_path))
        return WebIDL::SyntaxError::create(realm, "Key path is not valid"_string);

    // 8. Let unique be options’s unique member.
    auto unique = options.unique;

    // 9. Let multiEntry be options’s multiEntry member.
    auto multi_entry = options.multi_entry;

    // 10. If keyPath is a sequence and multiEntry is true, throw an "InvalidAccessError" DOMException.
    if (key_path.has<Vector<String>>() && multi_entry)
        return WebIDL::InvalidAccessError::create(realm, "Key path is a sequence and multiEntry is true"_string);

    // 11. Let index be a new index in store.
    //     Set index’s name to name, key path to keyPath, unique flag to unique, and multiEntry flag to multiEntry.
    auto index = Index::create(realm, store, name, key_path, unique, multi_entry);

    // 12. Add index to this's index set.
    this->index_set().set(name, index);

    // 13. Return a new index handle associated with index and this.
    return IDBIndex::create(realm, index, *this);
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-indexnames
GC::Ref<HTML::DOMStringList> IDBObjectStore::index_names()
{
    // 1. Let names be a list of the names of the indexes in this's index set.
    Vector<String> names;
    for (auto const& [name, index] : m_indexes)
        names.append(name);

    // 2. Return the result (a DOMStringList) of creating a sorted name list with names.
    return create_a_sorted_name_list(realm(), names);
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-index
WebIDL::ExceptionOr<GC::Ref<IDBIndex>> IDBObjectStore::index(String const& name)
{
    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. Let store be this’s object store.
    [[maybe_unused]] auto store = this->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is finished, then throw an "InvalidStateError" DOMException.
    if (transaction->state() == IDBTransaction::TransactionState::Finished)
        return WebIDL::InvalidStateError::create(realm(), "Transaction is finished"_string);

    // 5. Let index be the index named name in this’s index set if one exists, or throw a "NotFoundError" DOMException otherwise.
    auto index = m_indexes.get(name);
    if (!index.has_value())
        return WebIDL::NotFoundError::create(realm(), "Index not found in object store"_string);

    // 6. Return an index handle associated with index and this.
    return IDBIndex::create(realm(), *index, *this);
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-deleteindex
WebIDL::ExceptionOr<void> IDBObjectStore::delete_index(String const& name)
{
    auto& realm = this->realm();

    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. Let store be this’s object store.
    auto store = this->store();

    // 3. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
    if (transaction->mode() != Bindings::IDBTransactionMode::Versionchange)
        return WebIDL::InvalidStateError::create(realm, "Transaction is not an upgrade transaction"_string);

    // FIXME: 4. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while deleting index"_string);

    // 6. Let index be the index named name in store if one exists, or throw a "NotFoundError" DOMException otherwise.
    auto index = m_indexes.get(name);
    if (!index.has_value())
        return WebIDL::NotFoundError::create(realm, "Index not found while trying to delete it"_string);

    // 7. Remove index from this’s index set.
    m_indexes.remove(name);

    // 8. Destroy index.
    store->index_set().remove(name);

    return {};
}

// https://w3c.github.io/IndexedDB/#add-or-put
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::add_or_put(GC::Ref<IDBObjectStore> handle, JS::Value value, Optional<JS::Value> const& key, bool no_overwrite)
{
    auto& realm = this->realm();

    // 1. Let transaction be handle’s transaction.
    auto transaction = handle->transaction();

    // 2. Let store be handle’s object store.
    auto& store = *handle->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while running add/put"_string);

    // 5. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
    if (transaction->is_readonly())
        return WebIDL::ReadOnlyError::create(realm, "Transaction is read-only"_string);

    auto key_was_given = key.has_value() && key != JS::js_undefined();

    // 6. If store uses in-line keys and key was given, throw a "DataError" DOMException.
    if (store.uses_inline_keys() && key_was_given)
        return WebIDL::DataError::create(realm, "Store uses in-line keys and key was given"_string);

    // 7. If store uses out-of-line keys and has no key generator and key was not given, throw a "DataError" DOMException.
    if (store.uses_out_of_line_keys() && !store.uses_a_key_generator() && !key_was_given)
        return WebIDL::DataError::create(realm, "Store uses out-of-line keys and has no key generator and key was not given"_string);

    GC::Ptr<Key> key_value;
    // 8. If key was given, then:
    if (key_was_given) {
        // 1. Let r be the result of converting a value to a key with key. Rethrow any exceptions.
        auto r = TRY(convert_a_value_to_a_key(realm, key.value()));

        // 2. If r is invalid, throw a "DataError" DOMException.
        if (r->is_invalid())
            return WebIDL::DataError::create(realm, "Key is invalid"_string);

        // 3. Let key be r.
        key_value = r;
    }

    // 9. Let targetRealm be a user-agent defined Realm.
    auto& target_realm = realm;

    // 10. Let clone be a clone of value in targetRealm during transaction. Rethrow any exceptions.
    auto clone = TRY(clone_in_realm(target_realm, value, transaction));

    // 11. If store uses in-line keys, then:
    if (store.uses_inline_keys()) {
        // 1. Let kpk be the result of extracting a key from a value using a key path with clone and store’s key path. Rethrow any exceptions.
        auto maybe_kpk = TRY(extract_a_key_from_a_value_using_a_key_path(realm, clone, store.key_path().value()));

        // NOTE: Step 2 and 3 is reversed here, since we check for failure before validity.
        // 3. If kpk is not failure, let key be kpk.
        if (!maybe_kpk.is_error()) {
            key_value = maybe_kpk.release_value();

            // 2. If kpk is invalid, throw a "DataError" DOMException.
            if (key_value->is_invalid())
                return WebIDL::DataError::create(realm, key_value->value_as_string());
        }

        // 4. Otherwise (kpk is failure):
        else {
            // 1. If store does not have a key generator, throw a "DataError" DOMException.
            if (!store.uses_a_key_generator())
                return WebIDL::DataError::create(realm, "Store does not have a key generator"_string);

            // 2. Otherwise, if check that a key could be injected into a value with clone and store’s key path return false, throw a "DataError" DOMException.
            if (!check_that_a_key_could_be_injected_into_a_value(realm, clone, store.key_path().value()))
                return WebIDL::DataError::create(realm, "Key could not be injected into value"_string);
        }
    }

    // 12. Let operation be an algorithm to run store a record into an object store with store, clone, key, and no-overwrite flag.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [&realm, &store, clone, key_value, no_overwrite] -> WebIDL::ExceptionOr<JS::Value> {
        auto optional_key = TRY(store_a_record_into_an_object_store(realm, store, clone, key_value, no_overwrite));

        if (!optional_key || optional_key->is_invalid())
            return JS::js_undefined();

        return convert_a_key_to_a_value(realm, GC::Ref(*optional_key));
    });

    // 13. Return the result (an IDBRequest) of running asynchronously execute a request with handle and operation.
    auto result = asynchronously_execute_a_request(realm, handle, operation);
    dbgln_if(IDB_DEBUG, "Executing request for add/put with uuid {}", result->uuid());
    return result;
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-add
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::add(JS::Value value, Optional<JS::Value> const& key)
{
    // The add(value, key) method steps are to return the result of running add or put with this, value, key and the no-overwrite flag true.
    return add_or_put(*this, value, key, true);
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-put
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::put(JS::Value value, Optional<JS::Value> const& key)
{
    // The put(value, key) method steps are to return the result of running add or put with this, value, key and the no-overwrite flag false.
    return add_or_put(*this, value, key, false);
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-count
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::count(Optional<JS::Value> query)
{
    auto& realm = this->realm();

    // 1. Let transaction be this's transaction.
    auto transaction = this->transaction();

    // 2. Let store be this's object store.
    auto store = this->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while doing count"_string);

    // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
    auto range = TRY(convert_a_value_to_a_key_range(realm, move(query)));

    // 6. Let operation be an algorithm to run count the records in a range with store and range.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [store, range] -> WebIDL::ExceptionOr<JS::Value> {
        return count_the_records_in_a_range(store, range);
    });

    // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
    auto result = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for count with uuid {}", result->uuid());
    return result;
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-get
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::get(JS::Value query)
{
    auto& realm = this->realm();

    // 1. Let transaction be this's transaction.
    auto transaction = this->transaction();

    // 2. Let store be this's object store.
    auto store = this->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while getting"_string);

    // 5. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
    auto range = TRY(convert_a_value_to_a_key_range(realm, query, true));

    // 6. Let operation be an algorithm to run retrieve a value from an object store with the current Realm record, store, and range.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [&realm, store, range] -> WebIDL::ExceptionOr<JS::Value> {
        return retrieve_a_value_from_an_object_store(realm, store, range);
    });

    // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
    auto result = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for get with uuid {}", result->uuid());
    return result;
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-opencursor
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::open_cursor(JS::Value query, Bindings::IDBCursorDirection direction)
{
    auto& realm = this->realm();

    // 1. Let transaction be this's transaction.
    auto transaction = this->transaction();

    // 2. Let store be this's object store.
    [[maybe_unused]] auto store = this->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while opening cursor"_string);

    // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
    auto range = TRY(convert_a_value_to_a_key_range(realm, query, false));

    // 6. Let cursor be a new cursor with its source handle set to this, undefined position, direction set to direction,
    //    got value flag set to false, undefined key and value, range set to range, and key only flag set to false.
    auto cursor = IDBCursor::create(realm, GC::Ref(*this), {}, direction, false, {}, {}, range, false);

    // 7. Let operation be an algorithm to run iterate a cursor with the current Realm record and cursor.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [&realm, cursor] -> WebIDL::ExceptionOr<JS::Value> {
        return WebIDL::ExceptionOr<JS::Value>(iterate_a_cursor(realm, cursor));
    });

    // 8. Let request be the result of running asynchronously execute a request with this and operation.
    auto request = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for open cursor with uuid {}", request->uuid());

    // 9. Set cursor’s request to request.
    cursor->set_request(request);

    // 10. Return request.
    return request;
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-delete
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::delete_(JS::Value query)
{
    auto& realm = this->realm();

    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. Let store be this’s object store.
    auto store = this->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while deleting object store"_string);

    // 5. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
    if (transaction->is_readonly())
        return WebIDL::ReadOnlyError::create(realm, "Transaction is read-only while deleting object store"_string);

    // 6. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
    auto range = TRY(convert_a_value_to_a_key_range(realm, query, true));

    // 7. Let operation be an algorithm to run delete records from an object store with store and range.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [store, range] -> WebIDL::ExceptionOr<JS::Value> {
        return delete_records_from_an_object_store(store, range);
    });

    // 8. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
    auto result = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for delete with uuid {}", result->uuid());
    return result;
}

// https://w3c.github.io/IndexedDB/#dom-idbobjectstore-clear
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> IDBObjectStore::clear()
{
    auto& realm = this->realm();

    // 1. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 2. Let store be this’s object store.
    auto store = this->store();

    // FIXME: 3. If store has been deleted, throw an "InvalidStateError" DOMException.

    // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while clearing object store"_string);

    // 5. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
    if (transaction->is_readonly())
        return WebIDL::ReadOnlyError::create(realm, "Transaction is read-only while clearing object store"_string);

    // 6. Let operation be an algorithm to run clear an object store with store.
    auto operation = GC::Function<WebIDL::ExceptionOr<JS::Value>()>::create(realm.heap(), [store] -> WebIDL::ExceptionOr<JS::Value> {
        return clear_an_object_store(store);
    });

    // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
    auto result = asynchronously_execute_a_request(realm, GC::Ref(*this), operation);
    dbgln_if(IDB_DEBUG, "Executing request for clear with uuid {}", result->uuid());
    return result;
}

}
