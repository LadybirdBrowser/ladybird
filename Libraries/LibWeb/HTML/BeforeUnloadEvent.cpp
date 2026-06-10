/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/BeforeUnloadEvent.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BeforeUnloadEvent);

GC::Ref<BeforeUnloadEvent> BeforeUnloadEvent::create(FlyString const& event_name, DOM::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<BeforeUnloadEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

BeforeUnloadEvent::BeforeUnloadEvent(FlyString const& event_name, DOM::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
{
}

BeforeUnloadEvent::~BeforeUnloadEvent() = default;

}
