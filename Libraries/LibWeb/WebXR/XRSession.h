/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#XRSession-interface
class XRSession final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(XRSession, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(XRSession);

public:
    static GC::Ref<XRSession> create(JS::Realm&, GC::Ref<XRSystem>);
    virtual ~XRSession() override = default;

    // https://immersive-web.github.io/webxr/#dom-xrsession-end
    GC::Ref<WebIDL::Promise> end();

    // https://immersive-web.github.io/webxr/#shut-down-the-session
    void shut_down();

    GC::Ptr<WebIDL::CallbackType> onend();
    void set_onend(GC::Ptr<WebIDL::CallbackType>);

    bool promise_resolved() const { return m_promise_resolved; }
    void set_promise_resolved(bool promise_resolved) { m_promise_resolved = promise_resolved; }

private:
    XRSession(JS::Realm&, XRSystem&);
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<XRSystem> m_xr_system;

    // NB: These are for step 4 of Shut Down the Session, which requires us to reject all outstanding promises created by this session.
    Vector<GC::Ref<WebIDL::Promise>> m_outstanding_promises {};
    GC::Ref<WebIDL::Promise> create_promise(JS::Realm&);
    void resolve_promise(JS::Realm&, WebIDL::Promise const&, JS::Value = JS::js_undefined());
    void reject_promise(JS::Realm&, WebIDL::Promise const&, JS::Value);

    // https://immersive-web.github.io/webxr/#xrsession-promise-resolved
    bool m_promise_resolved { false };

    // https://immersive-web.github.io/webxr/#xrsession-ended
    bool m_ended { false };
};

}
