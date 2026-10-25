/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/EventTiming/EventCounts.h>
#include <LibWeb/EventTiming/EventTiming.h>

namespace Web::EventTiming {

GC_DEFINE_ALLOCATOR(EventCounts);

GC::Ref<EventCounts> EventCounts::create()
{
    return GC::Heap::the().allocate<EventCounts>();
}

// https://w3c.github.io/event-timing/#sec-modifications-HTML
EventCounts::EventCounts()
{
    // Upon construction of a Performance object whose relevant global object is a Window, its eventCounts must be
    // initialized to a map containing 0s for all event types that the user agent supports from the list described in
    // Events exposed.
    for_each_event_type_considered_for_event_timing([&](Utf16FlyString const& type) {
        m_entries.set(type, 0);
    });
}

EventCounts::~EventCounts() = default;

void EventCounts::increment(Utf16FlyString const& type)
{
    auto it = m_entries.find(type);
    VERIFY(it != m_entries.end());
    ++it->value;
}

}
