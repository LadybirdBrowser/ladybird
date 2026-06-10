/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransitionEvent.h"
#include <LibGC/Heap.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(TransitionEvent);

GC::Ref<TransitionEvent> TransitionEvent::create(FlyString const& type, TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TransitionEvent>(type, event_init, time_stamp);
}

GC::Ref<TransitionEvent> TransitionEvent::create(FlyString const& type, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<TransitionEvent>(type, move(property_name), elapsed_time, move(pseudo_element), time_stamp);
    event->set_bubbles(true);
    event->set_is_trusted(true);
    return event;
}

TransitionEvent::TransitionEvent(FlyString const& type, TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_property_name(event_init.property_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
{
}

TransitionEvent::TransitionEvent(FlyString const& type, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, time_stamp)
    , m_property_name(move(property_name))
    , m_elapsed_time(elapsed_time)
    , m_pseudo_element(move(pseudo_element))
{
}

TransitionEvent::~TransitionEvent() = default;

}
