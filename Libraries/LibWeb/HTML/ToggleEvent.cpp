/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/ToggleEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ToggleEvent);

GC::Ref<ToggleEvent> ToggleEvent::create(Utf16FlyString const& event_name, ToggleEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<ToggleEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

ToggleEvent::ToggleEvent(Utf16FlyString const& event_name, ToggleEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(FlyString { event_name.to_utf16_string().to_utf8() }, event_init, time_stamp)
    , m_old_state(event_init.old_state)
    , m_new_state(event_init.new_state)
    , m_source(event_init.source)
{
}

void ToggleEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

}
