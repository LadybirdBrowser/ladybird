/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBRequestPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBRequest.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBRequest);

IDBRequest::~IDBRequest() = default;

IDBRequest::IDBRequest(JS::Realm& realm)
    : EventTarget(realm)
{
}

void IDBRequest::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBRequest);
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-onsuccess
void IDBRequest::set_onsuccess(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::success, event_handler);
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-onsuccess
WebIDL::CallbackType* IDBRequest::onsuccess()
{
    return event_handler_attribute(HTML::EventNames::success);
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-onerror
void IDBRequest::set_onerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-onerror
WebIDL::CallbackType* IDBRequest::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

}
