/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/ErrorEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ErrorEvent);

GC::Ref<ErrorEvent> ErrorEvent::create(FlyString const& event_name, ErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<ErrorEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

ErrorEvent::ErrorEvent(FlyString const& event_name, ErrorEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_message(event_init.message)
    , m_filename(event_init.filename)
    , m_lineno(event_init.lineno)
    , m_colno(event_init.colno)
    , m_error(event_init.error.value_or(JS::js_undefined()))
{
}

ErrorEvent::~ErrorEvent() = default;

void ErrorEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error);
}

}
