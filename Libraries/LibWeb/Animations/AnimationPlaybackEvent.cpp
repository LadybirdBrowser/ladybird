/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/Bindings/AnimationPlaybackEvent.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationPlaybackEvent);

GC::Ref<AnimationPlaybackEvent> AnimationPlaybackEvent::create(FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<AnimationPlaybackEvent>(type, event_init, time_stamp);
}

// https://www.w3.org/TR/web-animations-1/#dom-animationplaybackevent-animationplaybackevent
WebIDL::ExceptionOr<GC::Ref<AnimationPlaybackEvent>> AnimationPlaybackEvent::construct_impl(HTML::Window& window, FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init)
{
    return GC::Heap::the().allocate<AnimationPlaybackEvent>(type, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

AnimationPlaybackEvent::AnimationPlaybackEvent(FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_current_time(event_init.current_time)
    , m_timeline_time(event_init.timeline_time)
{
}

void AnimationPlaybackEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_current_time);
    visitor.visit(m_timeline_time);
}

NullableCSSNumberish AnimationPlaybackEvent::current_time() const
{
    return m_current_time;
}

NullableCSSNumberish AnimationPlaybackEvent::timeline_time() const
{
    return m_timeline_time;
}

}
