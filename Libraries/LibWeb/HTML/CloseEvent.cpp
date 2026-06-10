/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/CloseEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CloseEvent);

GC::Ref<CloseEvent> CloseEvent::create(FlyString const& event_name, CloseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CloseEvent>(event_name, event_init, time_stamp);
}

CloseEvent::CloseEvent(FlyString const& event_name, CloseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_was_clean(event_init.was_clean)
    , m_code(event_init.code)
    , m_reason(event_init.reason)
{
}

CloseEvent::~CloseEvent() = default;

}
