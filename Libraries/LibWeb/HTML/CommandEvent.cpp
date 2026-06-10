/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/CommandEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CommandEvent);

GC::Ref<CommandEvent> CommandEvent::create(FlyString const& event_name, CommandEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CommandEvent>(event_name, event_init, time_stamp);
}

CommandEvent::CommandEvent(FlyString const& event_name, CommandEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_source(event_init.source)
    , m_command(event_init.command)
{
}

void CommandEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

}
