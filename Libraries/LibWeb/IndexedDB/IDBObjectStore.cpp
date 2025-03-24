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
}

GC::Ref<IDBObjectStore> IDBObjectStore::create(JS::Realm& realm, GC::Ref<ObjectStore> store, GC::Ref<IDBTransaction> transaction)
{
    return realm.create<IDBObjectStore>(realm, store, transaction);
}

void IDBObjectStore::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBObjectStore);
}

void IDBObjectStore::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_store);
    visitor.visit(m_transaction);
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
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active"_string);

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

}
