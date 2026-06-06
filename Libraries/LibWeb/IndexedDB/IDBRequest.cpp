/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/IndexedDB/IDBCursor.h>
#include <LibWeb/IndexedDB/IDBIndex.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBRequest);

IDBRequest::~IDBRequest() = default;

IDBRequest::IDBRequest(GC::Ref<DOM::EventTarget> relevant_global_object, IDBRequestSource source)
    : EventTarget()
    , m_source(source)
    , m_global_object(relevant_global_object)
    , m_uuid(Crypto::generate_random_uuid())
{
}

GC::Ref<IDBRequest> IDBRequest::create(GC::Ref<DOM::EventTarget> relevant_global_object, IDBRequestSource source)
{
    return GC::Heap::the().allocate<IDBRequest>(relevant_global_object, source);
}

void IDBRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_result);
    visitor.visit(m_transaction);
    visitor.visit(m_source);
    visitor.visit(m_global_object);
    visitor.visit(m_error);
}

JS::Object& IDBRequest::relevant_global_object() const
{
    return HTML::relevant_global_object(relevant_global_scope());
}

HTML::WindowOrWorkerGlobalScopeMixin& IDBRequest::relevant_global_scope() const
{
    return HTML::relevant_window_or_worker_global_scope(*m_global_object);
}

DOM::EventTarget* IDBRequest::get_parent(DOM::Event const&)
{
    // https://w3c.github.io/IndexedDB/#request-construct
    // A request’s get the parent algorithm returns the request’s transaction.
    return m_transaction.ptr();
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
[[nodiscard]] GC::Ptr<WebIDL::DOMException> IDBRequest::error() const
{
    // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
    if (!m_done)
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(relevant_global_object()), "The request is not done"_utf16);

    // 2. Otherwise, return this's error, or null if no error occurred.
    return m_error.value_or(nullptr);
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-result
[[nodiscard]] WebIDL::ExceptionOr<JS::Value> IDBRequest::result() const
{
    // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
    if (!m_done)
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(relevant_global_object()), "The request is not done"_utf16);

    // 2. Otherwise, return this's result, or undefined if the request resulted in an error.
    if (m_error.has_value())
        return JS::js_undefined();

    return m_result;
}

}
