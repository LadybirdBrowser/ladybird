/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ToggleEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/ToggleEvent.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ToggleEvent);

GC::Ref<ToggleEvent> ToggleEvent::create(FlyString const& event_name, Bindings::ToggleEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<ToggleEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

WebIDL::ExceptionOr<GC::Ref<ToggleEvent>> ToggleEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::ToggleEventInit const& event_init)
{
    return GC::Heap::the().allocate<ToggleEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

ToggleEvent::ToggleEvent(FlyString const& event_name, Bindings::ToggleEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_old_state(move(event_init.old_state))
    , m_new_state(move(event_init.new_state))
    , m_source(event_init.source)
{
}

void ToggleEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

}
