/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/XRSessionPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebXR/XRSession.h>
#include <LibWeb/WebXR/XRSessionEvent.h>
#include <LibWeb/WebXR/XRSystem.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRSession);

GC::Ref<XRSession> XRSession::create(JS::Realm& realm, GC::Ref<XRSystem> xr_system)
{
    return realm.create<XRSession>(realm, xr_system);
}

XRSession::XRSession(JS::Realm& realm, XRSystem& xr_system)
    : DOM::EventTarget(realm)
    , m_xr_system(xr_system)
{
}

void XRSession::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XRSession);
    Base::initialize(realm);
}

void XRSession::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_xr_system);
    visitor.visit(m_outstanding_promises);
}

GC::Ref<WebIDL::Promise> XRSession::create_promise(JS::Realm& realm)
{
    auto promise = WebIDL::create_promise(realm);

    m_outstanding_promises.append(promise);

    return promise;
}
void XRSession::resolve_promise(JS::Realm& realm, WebIDL::Promise const& promise, JS::Value value)
{
    WebIDL::resolve_promise(realm, promise, value);
    m_outstanding_promises.remove_first_matching([&](auto& entry) { return entry == &promise; });
}
void XRSession::reject_promise(JS::Realm& realm, WebIDL::Promise const& promise, JS::Value value)
{
    WebIDL::reject_promise(realm, promise, value);
    m_outstanding_promises.remove_first_matching([&](auto& entry) { return entry == &promise; });
}

GC::Ref<WebIDL::Promise> XRSession::end()
{
    // 1. Let promise be a new Promise in the relevant realm of this XRSession.
    auto& realm = HTML::relevant_realm(*this);
    auto promise = WebIDL::create_promise(realm);

    // 2. If the ended value of this is true, reject promise with a "InvalidStateError" DOMException and return promise.
    if (m_ended)
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Session already ended."_utf16));

    // 3. Shut down this.
    shut_down();

    // 4. Queue a task to perform the following steps:
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise]() {
        // 1. Wait until any platform-specific steps related to shutting down the session have completed.
        // FIXME: Do this once we have any.

        // 2. Resolve promise.
        HTML::TemporaryExecutionContext context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
        WebIDL::resolve_promise(realm, promise);
    }));

    // 5. Return promise.
    return promise;
}

// https://immersive-web.github.io/webxr/#shut-down-the-session
void XRSession::shut_down()
{
    auto& realm = HTML::relevant_realm(*this);

    // 1. Set session’s ended value to true.
    m_ended = true;

    // 2. If the active immersive session is equal to session, set the active immersive session to null.
    if (m_xr_system->active_immersive_session() == this)
        m_xr_system->set_active_immersive_session({});

    // 3. Remove session from the list of inline sessions.
    m_xr_system->remove_inline_session(*this);

    // 4. Reject any outstanding promises returned by session with an InvalidStateError, except for any promises returned by end().
    for (auto promise : m_outstanding_promises) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Session ended."_utf16));
    }
    m_outstanding_promises.clear();

    // 5. If no other features of the user agent are actively using them, perform the necessary platform-specific steps to shut down the device’s tracking and rendering capabilities. This MUST include:
    // - Releasing exclusive access to the XR device if session is an immersive session.
    // - Deallocating any graphics resources acquired by session for presentation to the XR device.
    // - Putting the XR device in a state such that a different source may be able to initiate a session with the same device if session is an immersive session.
    // FIXME: Implement this once we have any of this.

    // 6. Queue a task that fires an XRSessionEvent named end on session.
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [this, &realm]() {
        XRSessionEventInit init;
        init.session = this;
        auto event = XRSessionEvent::create(realm, HTML::EventNames::end, init);
        this->dispatch_event(event);
    }));
}

// https://immersive-web.github.io/webxr/#dom-xrsession-onend
GC::Ptr<WebIDL::CallbackType> XRSession::onend()
{
    return event_handler_attribute(HTML::EventNames::end);
}

// https://www.w3.org/TR/web-animations-1/#dom-animation-onfinish
void XRSession::set_onend(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::end, event_handler);
}

}
