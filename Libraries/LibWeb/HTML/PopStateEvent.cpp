/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PopStateEvent.h>
#include <LibWeb/HTML/PopStateEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PopStateEvent);

[[nodiscard]] GC::Ref<PopStateEvent> PopStateEvent::create(FlyString const& event_name, Bindings::PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PopStateEvent>(event_name, event_init, time_stamp);
}

GC::Ref<PopStateEvent> PopStateEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::PopStateEventInit const& event_init)
{
    return GC::Heap::the().allocate<PopStateEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

PopStateEvent::PopStateEvent(FlyString const& event_name, Bindings::PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_state(event_init.state)
{
}

void PopStateEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_state);
}

}
