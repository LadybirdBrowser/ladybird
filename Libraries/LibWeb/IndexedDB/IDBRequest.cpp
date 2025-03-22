/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBRequest.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBRequest);

IDBRequest::~IDBRequest() = default;

IDBRequest::IDBRequest(JS::Realm& realm, IDBRequestSource source)
    : EventTarget(realm)
    , m_source(source)
{
}

void IDBRequest::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBRequest);
}

GC::Ref<IDBRequest> IDBRequest::create(JS::Realm& realm, IDBRequestSource source)
{
    return realm.create<IDBRequest>(realm, source);
}

void IDBRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error);
    visitor.visit(m_result);
    visitor.visit(m_transaction);
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

// https://w3c.github.io/IndexedDB/#dom-idbrequest-readystate
[[nodiscard]] Bindings::IDBRequestReadyState IDBRequest::ready_state() const
{
    // The readyState getter steps are to return "pending" if this's done flag is false, and "done" otherwise.
    return m_done ? Bindings::IDBRequestReadyState::Done : Bindings::IDBRequestReadyState::Pending;
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-error
[[nodiscard]] WebIDL::ExceptionOr<GC::Ptr<WebIDL::DOMException>> IDBRequest::error() const
{
    // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
    if (!m_done)
        return WebIDL::InvalidStateError::create(realm(), "The request is not done"_string);

    // 2. Otherwise, return this's error, or null if no error occurred.
    return m_error;
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-result
[[nodiscard]] WebIDL::ExceptionOr<JS::Value> IDBRequest::result() const
{
    // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
    if (!m_done)
        return WebIDL::InvalidStateError::create(realm(), "The request is not done"_string);

    // 2. Otherwise, return this's result, or undefined if the request resulted in an error.
    return m_result;
}

}
