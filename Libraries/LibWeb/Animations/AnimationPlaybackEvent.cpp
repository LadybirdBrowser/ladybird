/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/Bindings/AnimationPlaybackEvent.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationPlaybackEvent);

GC::Ref<AnimationPlaybackEvent> AnimationPlaybackEvent::create(JS::Realm& realm, FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init)
{
    return realm.create<AnimationPlaybackEvent>(realm, type, event_init);
}

// https://www.w3.org/TR/web-animations-1/#dom-animationplaybackevent-animationplaybackevent
WebIDL::ExceptionOr<GC::Ref<AnimationPlaybackEvent>> AnimationPlaybackEvent::construct_impl(JS::Realm& realm, FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init)
{
    return create(realm, type, event_init);
}

AnimationPlaybackEvent::AnimationPlaybackEvent(JS::Realm& realm, FlyString const& type, Bindings::AnimationPlaybackEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_current_time(event_init.current_time)
    , m_timeline_time(event_init.timeline_time)
{
}

void AnimationPlaybackEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationPlaybackEvent);
    Base::initialize(realm);
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
