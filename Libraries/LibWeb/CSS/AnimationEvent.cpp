/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(AnimationEvent);

GC::Ref<AnimationEvent> AnimationEvent::create(Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<AnimationEvent>(type, event_init, time_stamp);
}

GC::Ref<AnimationEvent> AnimationEvent::create(Utf16FlyString const& type, Utf16String animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<AnimationEvent>(type, move(animation_name), elapsed_time, move(pseudo_element), time_stamp);
    event->set_is_trusted(true);
    return event;
}

WebIDL::ExceptionOr<GC::Ref<AnimationEvent>> AnimationEvent::create_for_constructor(Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return create(type, event_init, time_stamp);
}

AnimationEvent::AnimationEvent(Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(FlyString { type.view().to_utf8_but_should_be_ported_to_utf16() }, event_init, time_stamp)
    , m_animation_name(event_init.animation_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
    , m_animation(event_init.animation)
{
}

AnimationEvent::AnimationEvent(Utf16FlyString const& type, Utf16String animation_name, double elapsed_time, FlyString pseudo_element, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(FlyString { type.to_utf16_string().to_utf8() }, time_stamp)
    , m_animation_name(move(animation_name))
    , m_elapsed_time(elapsed_time)
    , m_pseudo_element(Utf16String::from_utf8(pseudo_element))
{
}

void AnimationEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
}

}
