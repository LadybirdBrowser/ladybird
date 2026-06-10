/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/AnimationEvent.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(AnimationEvent);

GC::Ref<AnimationEvent> AnimationEvent::create(FlyString const& type, AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<AnimationEvent>(type, event_init, time_stamp);
}

GC::Ref<AnimationEvent> AnimationEvent::create(FlyString const& type, FlyString animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<AnimationEvent>(type, move(animation_name), elapsed_time, move(pseudo_element), time_stamp);
    event->set_bubbles(true);
    return event;
}

AnimationEvent::AnimationEvent(FlyString const& type, AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_animation_name(event_init.animation_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
{
}

AnimationEvent::AnimationEvent(FlyString const& type, FlyString animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, time_stamp)
    , m_animation_name(move(animation_name))
    , m_elapsed_time(elapsed_time)
    , m_pseudo_element(move(pseudo_element))
{
}

}
