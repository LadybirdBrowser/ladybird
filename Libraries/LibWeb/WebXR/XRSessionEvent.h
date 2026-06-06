/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::WebXR {

// https://immersive-web.github.io/webxr/#xrsessionevent
class XRSessionEvent : public DOM::Event {
    WEB_WRAPPABLE(XRSessionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(XRSessionEvent);

public:
    [[nodiscard]] static GC::Ref<XRSessionEvent> create(FlyString const&, Bindings::XRSessionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<XRSessionEvent> construct_impl(HTML::Window&, FlyString const&, Bindings::XRSessionEventInit const&);

    virtual ~XRSessionEvent() override = default;

    GC::Ptr<XRSession> session() const { return m_session; }

private:
    XRSessionEvent(FlyString const&, Bindings::XRSessionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://immersive-web.github.io/webxr/#dom-xrsessionevent-session
    GC::Ptr<XRSession> m_session;
};

}
