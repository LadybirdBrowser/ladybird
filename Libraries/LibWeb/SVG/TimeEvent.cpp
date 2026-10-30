/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/SVG/TimeEvent.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(TimeEvent);

GC::Ref<TimeEvent> TimeEvent::create(Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TimeEvent>(type, view, detail, time_stamp);
}

TimeEvent::TimeEvent(Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(type, time_stamp)
    , m_view(view)
    , m_detail(detail)
{
}

void TimeEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_view);
}

// https://svgwg.org/specs/animations/#__svg__TimeEvent__initTimeEvent
void TimeEvent::init_time_event(Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail)
{
    // The initTimeEvent method is used to initialize the value of a TimeEvent created with document.createEvent(). This
    // method may only be called before the TimeEvent has been dispatched via the dispatchEvent method, though it may be
    // called multiple times during that phase if necessary. If called multiple times, the final invocation takes
    // precedence.
    if (dispatched())
        return;

    // NB: Time events neither bubble nor are cancelable.
    initialize_event(type, false, false);
    m_view = view;
    m_detail = detail;
}

}
