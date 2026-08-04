/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/PopStateEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PopStateEvent);

[[nodiscard]] GC::Ref<PopStateEvent> PopStateEvent::create(FlyString const& event_name, PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PopStateEvent>(event_name, event_init, time_stamp);
}

GC::Ref<PopStateEvent> PopStateEvent::create(Utf16String const& event_name, PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PopStateEvent>(Utf16FlyString::from_utf16(event_name.utf16_view()), event_init, time_stamp);
}

GC::Ref<PopStateEvent> PopStateEvent::create(Utf16FlyString const& event_name, PopStateEventInit const& event_init)
{
    return GC::Heap::the().allocate<PopStateEvent>(event_name, event_init, 0);
}

GC::Ref<PopStateEvent> PopStateEvent::create(FlyString const& event_name, JS::Value state, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PopStateEvent>(event_name, state, time_stamp);
}

PopStateEvent::PopStateEvent(FlyString const& event_name, PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_state(event_init.state)
    , m_has_ua_visual_transition(event_init.has_ua_visual_transition)
{
}

PopStateEvent::PopStateEvent(Utf16FlyString const& event_name, PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_state(event_init.state)
    , m_has_ua_visual_transition(event_init.has_ua_visual_transition)
{
}

PopStateEvent::PopStateEvent(FlyString const& event_name, JS::Value state, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
    , m_state(state)
{
}

void PopStateEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_state);
}

}
