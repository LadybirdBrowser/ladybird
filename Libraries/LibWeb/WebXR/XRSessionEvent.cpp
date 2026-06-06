/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/XRSessionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebXR/XRSession.h>
#include <LibWeb/WebXR/XRSessionEvent.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRSessionEvent);

GC::Ref<XRSessionEvent> XRSessionEvent::create(FlyString const& type, Bindings::XRSessionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<XRSessionEvent>(type, event_init, time_stamp);
}

// https://immersive-web.github.io/webxr/#dom-xrsessionevent-xrsessionevent
GC::Ref<XRSessionEvent> XRSessionEvent::construct_impl(HTML::Window& window, FlyString const& type, Bindings::XRSessionEventInit const& event_init)
{
    return create(type, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

XRSessionEvent::XRSessionEvent(FlyString const& type, Bindings::XRSessionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_session(event_init.session)
{
}

void XRSessionEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_session);
}

}
