/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#dictdef-xrsessioneventinit
struct XRSessionEventInit : public DOM::EventInit {
    GC::Ptr<XRSession> session;
};

// https://immersive-web.github.io/webxr/#xrsessionevent
class XRSessionEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(XRSessionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(XRSessionEvent);

public:
    [[nodiscard]] static GC::Ref<XRSessionEvent> create(JS::Realm&, FlyString const&, XRSessionEventInit const&);
    static GC::Ref<XRSessionEvent> construct_impl(JS::Realm&, FlyString const&, XRSessionEventInit const&);

    virtual ~XRSessionEvent() override = default;

    GC::Ptr<XRSession> session() const { return m_session; }

private:
    XRSessionEvent(JS::Realm&, FlyString const&, XRSessionEventInit const&);
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://immersive-web.github.io/webxr/#dom-xrsessionevent-session
    GC::Ptr<XRSession> m_session;
};

}
