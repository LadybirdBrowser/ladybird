/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/XRSystem.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebXR {

using XRSessionMode = Bindings::XRSessionMode;

using XRSessionInit = Bindings::XRSessionInit;

// https://immersive-web.github.io/webxr/#xrsystem-interface
class XRSystem final : public DOM::EventTarget {
    WEB_WRAPPABLE(XRSystem, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(XRSystem);

public:
    static GC::Ref<XRSystem> create(HTML::Window&);
    virtual ~XRSystem() override = default;

    // https://immersive-web.github.io/webxr/#dom-xrsystem-issessionsupported
    bool is_session_mode_supported(XRSessionMode) const;
    void is_session_supported(JS::Realm&, XRSessionMode, GC::Ref<WebIDL::Promise>) const;

    // https://immersive-web.github.io/webxr/#dom-xrsystem-requestsession
    void request_session(JS::Realm&, XRSessionMode, XRSessionInit const&, GC::Ref<WebIDL::Promise>);

    JS::Object& relevant_global_object() const;

    void set_pending_immersive_session(bool pending_immersive_session) { m_pending_immersive_session = pending_immersive_session; }

    GC::Ptr<XRSession> active_immersive_session() const { return m_active_immersive_session; }
    void set_active_immersive_session(GC::Ptr<XRSession> active_immersive_session) { m_active_immersive_session = active_immersive_session; }

    void remove_inline_session(GC::Ref<XRSession>);

private:
    XRSystem(HTML::Window&);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://immersive-web.github.io/webxr/#pending-immersive-session
    bool m_pending_immersive_session { false };

    // https://immersive-web.github.io/webxr/#active-immersive-session
    GC::Ptr<XRSession> m_active_immersive_session {};

    // https://immersive-web.github.io/webxr/#list-of-inline-sessions
    Vector<GC::Ref<XRSession>> m_list_of_inline_sessions {};

    GC::Ref<HTML::Window> m_window;
};

}
