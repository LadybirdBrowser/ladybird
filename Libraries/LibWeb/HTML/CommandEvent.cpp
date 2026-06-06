/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/CommandEvent.h>
#include <LibWeb/HTML/CommandEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CommandEvent);

GC::Ref<CommandEvent> CommandEvent::create(FlyString const& event_name, Bindings::CommandEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CommandEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<CommandEvent>> CommandEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::CommandEventInit const& event_init)
{
    return GC::Heap::the().allocate<CommandEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

CommandEvent::CommandEvent(FlyString const& event_name, Bindings::CommandEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_source(event_init.source)
    , m_command(move(event_init.command))
{
}

void CommandEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

}
