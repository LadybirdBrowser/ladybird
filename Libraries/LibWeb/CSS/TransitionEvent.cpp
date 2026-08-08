/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TransitionEvent.h"
#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSTransition.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(TransitionEvent);

GC::Ref<TransitionEvent> TransitionEvent::create(Utf16FlyString const& type, Bindings::TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TransitionEvent>(type, event_init, time_stamp);
}

GC::Ref<TransitionEvent> TransitionEvent::create(Utf16FlyString const& type, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp, GC::Ptr<CSSTransition> animation)
{
    auto event = GC::Heap::the().allocate<TransitionEvent>(type, move(property_name), elapsed_time, move(pseudo_element), time_stamp, animation);
    event->set_bubbles(true);
    event->set_is_trusted(true);
    return event;
}

GC::Ref<TransitionEvent> TransitionEvent::create_for_constructor(Utf16FlyString const& type, Bindings::TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return create(type, event_init, time_stamp);
}

TransitionEvent::TransitionEvent(Utf16FlyString const& type, Bindings::TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(FlyString { type.to_utf16_string().to_utf8() }, event_init, time_stamp)
    , m_property_name(event_init.property_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
    , m_animation(event_init.animation)
{
}

TransitionEvent::TransitionEvent(Utf16FlyString const& type, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp, GC::Ptr<CSSTransition> animation)
    : DOM::Event(FlyString { type.to_utf16_string().to_utf8() }, time_stamp)
    , m_property_name(Utf16String::from_utf8(property_name))
    , m_elapsed_time(elapsed_time)
    , m_pseudo_element(Utf16String::from_utf8(pseudo_element))
    , m_animation(animation)
{
}

TransitionEvent::~TransitionEvent() = default;

void TransitionEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
}

}
