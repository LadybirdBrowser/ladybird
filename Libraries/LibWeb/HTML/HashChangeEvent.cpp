/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/HashChangeEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HashChangeEvent);

[[nodiscard]] GC::Ref<HashChangeEvent> HashChangeEvent::create(FlyString const& event_name, HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<HashChangeEvent>(event_name, event_init, time_stamp);
}

GC::Ref<HashChangeEvent> HashChangeEvent::create(FlyString const& event_name, String old_url, String new_url, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<HashChangeEvent>(event_name, move(old_url), move(new_url), time_stamp);
}

HashChangeEvent::HashChangeEvent(FlyString const& event_name, HashChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_old_url(event_init.old_url)
    , m_new_url(event_init.new_url)
{
}

HashChangeEvent::HashChangeEvent(FlyString const& event_name, String old_url, String new_url, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
    , m_old_url(move(old_url))
    , m_new_url(move(new_url))
{
}

void HashChangeEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
