/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/IDBIndexPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBIndex.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBIndex);

IDBIndex::~IDBIndex() = default;

IDBIndex::IDBIndex(JS::Realm& realm, GC::Ref<Index> index, GC::Ref<IDBObjectStore> object_store)
    : PlatformObject(realm)
    , m_index(index)
    , m_object_store_handle(object_store)
    , m_name(index->name())
{
}

GC::Ref<IDBIndex> IDBIndex::create(JS::Realm& realm, GC::Ref<Index> index, GC::Ref<IDBObjectStore> object_store)
{
    return realm.create<IDBIndex>(realm, index, object_store);
}

void IDBIndex::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBIndex);
}

void IDBIndex::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_index);
    visitor.visit(m_object_store_handle);
}

// https://w3c.github.io/IndexedDB/#dom-idbindex-name
WebIDL::ExceptionOr<void> IDBIndex::set_name(String const& value)
{
    auto& realm = this->realm();

    // 1. Let name be the given value.
    auto const& name = value;

    // 2. Let transaction be this’s transaction.
    auto transaction = this->transaction();

    // 3. Let index be this’s index.
    auto index = this->index();

    // 4. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
    if (!transaction->is_upgrade_transaction())
        return WebIDL::InvalidStateError::create(realm, "Transaction is not an upgrade transaction"_string);

    // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (transaction->state() != IDBTransaction::TransactionState::Active)
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active"_string);

    // FIXME: 6. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.

    // 7. If index’s name is equal to name, terminate these steps.
    if (index->name() == name)
        return {};

    // 8. If an index named name already exists in index’s object store, throw a "ConstraintError" DOMException.
    for (auto const& existing_index : m_object_store_handle->index_set()) {
        if (existing_index->name() == name)
            return WebIDL::ConstraintError::create(realm, "An index with the given name already exists"_string);
    }

    // 9. Set index’s name to name.
    index->set_name(name);

    // 10. Set this’s name to name.
    m_name = name;

    return {};
}

// https://w3c.github.io/IndexedDB/#dom-idbindex-keypath
JS::Value IDBIndex::key_path() const
{
    return m_index->key_path().visit(
        [&](String const& value) -> JS::Value {
            return JS::PrimitiveString::create(realm().vm(), value);
        },
        [&](Vector<String> const& value) -> JS::Value {
            return JS::Array::create_from<String>(realm(), value.span(), [&](auto const& entry) -> JS::Value {
                return JS::PrimitiveString::create(realm().vm(), entry);
            });
        });
}

}
