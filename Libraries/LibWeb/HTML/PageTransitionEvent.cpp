/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/PageTransitionEvent.h>
#include <LibWeb/HTML/PageTransitionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PageTransitionEvent);

GC::Ref<PageTransitionEvent> PageTransitionEvent::create(FlyString const& event_name, Bindings::PageTransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PageTransitionEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<PageTransitionEvent>> PageTransitionEvent::construct_impl(Window& window, FlyString const& event_name, Bindings::PageTransitionEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(window)));
}

PageTransitionEvent::PageTransitionEvent(FlyString const& event_name, Bindings::PageTransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_persisted(event_init.persisted)
{
}

PageTransitionEvent::~PageTransitionEvent() = default;

}
