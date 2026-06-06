/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransitionEvent.h"
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/TransitionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(TransitionEvent);

GC::Ref<TransitionEvent> TransitionEvent::create(FlyString const& type, Bindings::TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<TransitionEvent>(type, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

GC::Ref<TransitionEvent> TransitionEvent::construct_impl(HTML::Window& window, FlyString const& type, Bindings::TransitionEventInit const& event_init)
{
    return GC::Heap::the().allocate<TransitionEvent>(type, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

TransitionEvent::TransitionEvent(FlyString const& type, Bindings::TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_property_name(event_init.property_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
{
}

TransitionEvent::~TransitionEvent() = default;

}
