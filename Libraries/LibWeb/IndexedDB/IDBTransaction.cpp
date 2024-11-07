/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBTransaction);

IDBTransaction::~IDBTransaction() = default;

IDBTransaction::IDBTransaction(JS::Realm& realm, GC::Ref<IDBDatabase> database)
    : EventTarget(realm)
    , m_connection(database)
{
}

GC::Ref<IDBTransaction> IDBTransaction::create(JS::Realm& realm, GC::Ref<IDBDatabase> database)
{
    return realm.create<IDBTransaction>(realm, database);
}

void IDBTransaction::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBTransaction);
}

void IDBTransaction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_connection);
    visitor.visit(m_error);
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

}
