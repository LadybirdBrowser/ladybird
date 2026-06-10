/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/SubmitEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SubmitEvent);

GC::Ref<SubmitEvent> SubmitEvent::create(FlyString const& event_name, SubmitEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<SubmitEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

SubmitEvent::SubmitEvent(FlyString const& event_name, SubmitEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_submitter(event_init.submitter)
{
}

SubmitEvent::~SubmitEvent() = default;

void SubmitEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_submitter);
}

}
