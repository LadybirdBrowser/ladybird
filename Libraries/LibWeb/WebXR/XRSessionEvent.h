/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/XRSessionEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::WebXR {

class XRSession;

using XRSessionEventInit = Bindings::XRSessionEventInit;

// https://immersive-web.github.io/webxr/#xrsessionevent
class XRSessionEvent : public DOM::Event {
    WEB_WRAPPABLE(XRSessionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(XRSessionEvent);

public:
    [[nodiscard]] static GC::Ref<XRSessionEvent> create(FlyString const&, XRSessionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~XRSessionEvent() override = default;

    GC::Ptr<XRSession> session() const { return m_session; }

private:
    XRSessionEvent(FlyString const&, XRSessionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://immersive-web.github.io/webxr/#dom-xrsessionevent-session
    GC::Ptr<XRSession> m_session;
};

}
