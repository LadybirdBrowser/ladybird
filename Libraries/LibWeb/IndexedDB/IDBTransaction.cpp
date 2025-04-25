/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBDatabasePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBTransaction);

IDBTransaction::~IDBTransaction() = default;

IDBTransaction::IDBTransaction(JS::Realm& realm, GC::Ref<IDBDatabase> connection, Bindings::IDBTransactionMode mode, Bindings::IDBTransactionDurability durability, Vector<GC::Ref<ObjectStore>> scopes)
    : EventTarget(realm)
    , m_connection(connection)
    , m_mode(mode)
    , m_durability(durability)
    , m_scope(move(scopes))
{
    m_uuid = MUST(Crypto::generate_random_uuid());
    connection->add_transaction(*this);
}

GC::Ref<IDBTransaction> IDBTransaction::create(JS::Realm& realm, GC::Ref<IDBDatabase> connection, Bindings::IDBTransactionMode mode, Bindings::IDBTransactionDurability durability = Bindings::IDBTransactionDurability::Default, Vector<GC::Ref<ObjectStore>> scopes = {})
{
    return realm.create<IDBTransaction>(realm, connection, mode, durability, move(scopes));
}

void IDBTransaction::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBTransaction);
    Base::initialize(realm);
}

void IDBTransaction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_connection);
    visitor.visit(m_error);
    visitor.visit(m_associated_request);
    visitor.visit(m_scope);
    visitor.visit(m_cleanup_event_loop);
}

void IDBTransaction::set_onabort(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::abort, event_handler);
}

WebIDL::CallbackType* IDBTransaction::onabort()
{
    return event_handler_attribute(HTML::EventNames::abort);
}

void IDBTransaction::set_oncomplete(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::complete, event_handler);
}

WebIDL::CallbackType* IDBTransaction::oncomplete()
{
    return event_handler_attribute(HTML::EventNames::complete);
}

void IDBTransaction::set_onerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

WebIDL::CallbackType* IDBTransaction::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

// https://w3c.github.io/IndexedDB/#dom-idbtransaction-abort
WebIDL::ExceptionOr<void> IDBTransaction::abort()
{
    // 1. If this's state is committing or finished, then throw an "InvalidStateError" DOMException.
    if (m_state == TransactionState::Committing || m_state == TransactionState::Finished)
        return WebIDL::InvalidStateError::create(realm(), "Transaction is ending"_string);

    // 2. Set this's state to inactive and run abort a transaction with this and null.
    m_state = TransactionState::Inactive;
    abort_a_transaction(*this, nullptr);
    return {};
}

// https://w3c.github.io/IndexedDB/#dom-idbtransaction-objectstorenames
GC::Ref<HTML::DOMStringList> IDBTransaction::object_store_names()
{
    // 1. Let names be a list of the names of the object stores in this's scope.
    Vector<String> names;
    for (auto const& object_store : this->scope())
        names.append(object_store->name());

    // 2. Return the result (a DOMStringList) of creating a sorted name list with names.
    return create_a_sorted_name_list(realm(), names);
}

// https://w3c.github.io/IndexedDB/#dom-idbtransaction-commit
WebIDL::ExceptionOr<void> IDBTransaction::commit()
{
    auto& realm = this->realm();

    // 1. If this's state is not active, then throw an "InvalidStateError" DOMException.
    if (m_state != TransactionState::Active)
        return WebIDL::InvalidStateError::create(realm, "Transaction is not active while commiting"_string);

    // 2. Run commit a transaction with this.
    commit_a_transaction(realm, *this);

    return {};
}

GC::Ptr<ObjectStore> IDBTransaction::object_store_named(String const& name) const
{
    for (auto const& store : m_scope) {
        if (store->name() == name)
            return store;
    }

    return nullptr;
}

// https://w3c.github.io/IndexedDB/#dom-idbtransaction-objectstore
WebIDL::ExceptionOr<GC::Ref<IDBObjectStore>> IDBTransaction::object_store(String const& name)
{
    auto& realm = this->realm();

    // 1. If this's state is finished, then throw an "InvalidStateError" DOMException.
    if (m_state == TransactionState::Finished)
        return WebIDL::InvalidStateError::create(realm, "Transaction is finished"_string);

    // 2. Let store be the object store named name in this's scope, or throw a "NotFoundError" DOMException if none.
    auto store = object_store_named(name);
    if (!store)
        return WebIDL::NotFoundError::create(realm, "Object store not found in transactions scope"_string);

    // 3. Return an object store handle associated with store and this.
    return IDBObjectStore::create(realm, *store, *this);
}

}
