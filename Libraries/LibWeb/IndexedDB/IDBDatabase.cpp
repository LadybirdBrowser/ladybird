/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBDatabasePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBDatabase);

IDBDatabase::IDBDatabase(JS::Realm& realm, Database& db)
    : EventTarget(realm)
    , m_name(db.name())
    , m_object_store_names(HTML::DOMStringList::create(realm, {}))
    , m_associated_database(db)
{
    db.associate(*this);
}

IDBDatabase::~IDBDatabase() = default;

JS::NonnullGCPtr<IDBDatabase> IDBDatabase::create(JS::Realm& realm, Database& db)
{
    return realm.create<IDBDatabase>(realm, db);
}

void IDBDatabase::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBDatabase);
}

void IDBDatabase::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_object_store_names);
    visitor.visit(m_associated_database);
}

void IDBDatabase::set_onabort(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::abort, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onabort()
{
    return event_handler_attribute(HTML::EventNames::abort);
}

void IDBDatabase::set_onerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

void IDBDatabase::set_onclose(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::close, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onclose()
{
    return event_handler_attribute(HTML::EventNames::close);
}

void IDBDatabase::set_onversionchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::versionchange, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onversionchange()
{
    return event_handler_attribute(HTML::EventNames::versionchange);
}

}
