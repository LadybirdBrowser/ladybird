/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBCursor.h>
#include <LibWeb/IndexedDB/IDBIndex.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/IDBRequestObserver.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBRequest);

IDBRequest::~IDBRequest() = default;

IDBRequest::IDBRequest(JS::Realm& realm, IDBRequestSource source)
    : EventTarget(realm)
    , m_source(source)
{
    m_uuid = MUST(Crypto::generate_random_uuid());
}

void IDBRequest::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBRequest);
    Base::initialize(realm);
}

GC::Ref<IDBRequest> IDBRequest::create(JS::Realm& realm, IDBRequestSource source)
{
    return realm.create<IDBRequest>(realm, source);
}

void IDBRequest::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_result);
    visitor.visit(m_transaction);

    m_source.visit(
        [&](Empty) {},
        [&](auto const& object) { visitor.visit(object); });

    visitor.visit(m_error);

    visitor.visit(m_request_observers_being_notified);
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
        return WebIDL::InvalidStateError::create(realm(), "The request is not done"_utf16);

    // 2. Otherwise, return this's error, or null if no error occurred.
    return m_error.value_or(nullptr);
}

// https://w3c.github.io/IndexedDB/#dom-idbrequest-result
[[nodiscard]] WebIDL::ExceptionOr<JS::Value> IDBRequest::result() const
{
    // 1. If this's done flag is false, then throw an "InvalidStateError" DOMException.
    if (!m_done)
        return WebIDL::InvalidStateError::create(realm(), "The request is not done"_utf16);

    // 2. Otherwise, return this's result, or undefined if the request resulted in an error.
    if (m_error.has_value())
        return JS::js_undefined();

    return m_result;
}

void IDBRequest::register_request_observer(Badge<IDBRequestObserver>, IDBRequestObserver& request_observer)
{
    auto result = m_request_observers.set(request_observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void IDBRequest::unregister_request_observer(Badge<IDBRequestObserver>, IDBRequestObserver& request_observer)
{
    bool was_removed = m_request_observers.remove(request_observer);
    VERIFY(was_removed);
}

void IDBRequest::set_processed(bool processed)
{
    m_processed = processed;
    notify_each_request_observer([](IDBRequestObserver const& request_observer) {
        return request_observer.request_processed_changed_observer();
    });
}

}
