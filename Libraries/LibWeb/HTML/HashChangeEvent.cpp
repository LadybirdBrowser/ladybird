/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/HashChangeEvent.h>
#include <LibWeb/HTML/HashChangeEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HashChangeEvent);

[[nodiscard]] GC::Ref<HashChangeEvent> HashChangeEvent::create(FlyString const& event_name, Bindings::HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<HashChangeEvent>(event_name, event_init, time_stamp);
}

GC::Ref<HashChangeEvent> HashChangeEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::HashChangeEventInit const& event_init)
{
    return GC::Heap::the().allocate<HashChangeEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

HashChangeEvent::HashChangeEvent(FlyString const& event_name, Bindings::HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_old_url(event_init.old_url)
    , m_new_url(event_init.new_url)
{
}

void HashChangeEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
