/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/AnimationEvent.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(AnimationEvent);

GC::Ref<AnimationEvent> AnimationEvent::create(FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<AnimationEvent>(type, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<AnimationEvent>> AnimationEvent::construct_impl(HTML::Window& window, FlyString const& type, Bindings::AnimationEventInit const& event_init)
{
    return GC::Heap::the().allocate<AnimationEvent>(type, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

AnimationEvent::AnimationEvent(FlyString const& type, Bindings::AnimationEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_animation_name(event_init.animation_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
{
}

}
