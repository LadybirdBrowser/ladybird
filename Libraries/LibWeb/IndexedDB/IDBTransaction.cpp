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
#include <LibWeb/IndexedDB/Internal/IDBTransactionObserver.h>

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
    visitor.visit(m_transaction_observers_being_notified);
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
        return WebIDL::InvalidStateError::create(realm(), "Transaction is ending"_utf16);

    // 2. Run abort a transaction with this and null.
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
        return WebIDL::InvalidStateError::create(realm, "Transaction is not active while committing"_utf16);

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
        return WebIDL::InvalidStateError::create(realm, "Transaction is finished"_utf16);

    // 2. Let store be the object store named name in this's scope, or throw a "NotFoundError" DOMException if none.
    auto store = object_store_named(name);
    if (!store)
        return WebIDL::NotFoundError::create(realm, "Object store not found in transactions scope"_utf16);

    // 3. Return an object store handle associated with store and this.
    return IDBObjectStore::create(realm, *store, *this);
}

void IDBTransaction::register_transaction_observer(Badge<IDBTransactionObserver>, IDBTransactionObserver& database_observer)
{
    auto result = m_transaction_observers.set(database_observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void IDBTransaction::unregister_transaction_observer(Badge<IDBTransactionObserver>, IDBTransactionObserver& database_observer)
{
    bool was_removed = m_transaction_observers.remove(database_observer);
    VERIFY(was_removed);
}

void IDBTransaction::set_state(TransactionState state)
{
    m_state = state;

    if (m_state == TransactionState::Finished) {
        notify_each_transaction_observer([](IDBTransactionObserver const& transaction_observer) {
            return transaction_observer.transaction_finished_observer();
        });
    }
}

}
