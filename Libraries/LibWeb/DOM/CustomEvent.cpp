/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/CustomEvent.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(CustomEvent);

GC::Ref<CustomEvent> CustomEvent::create(JS::Object const& relevant_global_object, FlyString const& event_name, CustomEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<CustomEvent> CustomEvent::create(FlyString const& event_name, CustomEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CustomEvent>(event_name, event_init, time_stamp);
}

CustomEvent::CustomEvent(FlyString const& event_name, CustomEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(event_name, event_init, time_stamp)
    , m_detail(event_init.detail)
{
}

CustomEvent::~CustomEvent() = default;

void CustomEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_detail);
}

// https://dom.spec.whatwg.org/#dom-customevent-initcustomevent
void CustomEvent::init_custom_event(String const& type, bool bubbles, bool cancelable, JS::Value detail)
{
    // 1. If this’s dispatch flag is set, then return.
    if (dispatched())
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);

    // 3. Set this’s detail attribute to detail.
    m_detail = detail;
}

}
