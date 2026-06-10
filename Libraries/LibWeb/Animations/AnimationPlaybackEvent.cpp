/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/CSS/CSSNumericValue.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationPlaybackEvent);

GC::Ref<AnimationPlaybackEvent> AnimationPlaybackEvent::create(FlyString const& type, AnimationPlaybackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<AnimationPlaybackEvent>(type, event_init, time_stamp);
}

GC::Ref<AnimationPlaybackEvent> AnimationPlaybackEvent::create(FlyString const& type, NullableCSSNumberish current_time, NullableCSSNumberish timeline_time, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    AnimationPlaybackEventInit event_init;
    event_init.current_time = move(current_time);
    event_init.timeline_time = move(timeline_time);
    return create(type, event_init, time_stamp);
}

AnimationPlaybackEvent::AnimationPlaybackEvent(FlyString const& type, AnimationPlaybackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
