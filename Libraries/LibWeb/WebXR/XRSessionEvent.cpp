/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebXR/XRSession.h>
#include <LibWeb/WebXR/XRSessionEvent.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRSessionEvent);

GC::Ref<XRSessionEvent> XRSessionEvent::create(FlyString const& type, XRSessionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<XRSessionEvent>(type, event_init, time_stamp);
}

XRSessionEvent::XRSessionEvent(FlyString const& type, XRSessionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
