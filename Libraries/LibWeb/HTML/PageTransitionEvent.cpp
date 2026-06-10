/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/PageTransitionEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PageTransitionEvent);

GC::Ref<PageTransitionEvent> PageTransitionEvent::create(FlyString const& event_name, PageTransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<PageTransitionEvent>(event_name, event_init, time_stamp);
}

PageTransitionEvent::PageTransitionEvent(FlyString const& event_name, PageTransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_persisted(event_init.persisted)
{
}

PageTransitionEvent::~PageTransitionEvent() = default;

}
