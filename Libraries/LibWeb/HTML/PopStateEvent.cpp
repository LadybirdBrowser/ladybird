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

GC::Ref<PopStateEvent> PopStateEvent::create(FlyString const& event_name, JS::Value state, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PopStateEvent>(event_name, state, time_stamp);
}

PopStateEvent::PopStateEvent(FlyString const& event_name, PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_state(event_init.state)
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
