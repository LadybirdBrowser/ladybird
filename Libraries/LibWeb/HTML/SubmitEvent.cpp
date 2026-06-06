/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/SubmitEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SubmitEvent.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SubmitEvent);

GC::Ref<SubmitEvent> SubmitEvent::create(FlyString const& event_name, Bindings::SubmitEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<SubmitEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

WebIDL::ExceptionOr<GC::Ref<SubmitEvent>> SubmitEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::SubmitEventInit const& event_init)
{
    return GC::Heap::the().allocate<SubmitEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

SubmitEvent::SubmitEvent(FlyString const& event_name, Bindings::SubmitEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
