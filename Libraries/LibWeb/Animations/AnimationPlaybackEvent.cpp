/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/AnimationPlaybackEvent.h>
#include <LibWeb/Bindings/AnimationPlaybackEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::Animations {

JS_DEFINE_ALLOCATOR(AnimationPlaybackEvent);

JS::NonnullGCPtr<AnimationPlaybackEvent> AnimationPlaybackEvent::create(JS::Realm& realm, FlyString const& type, AnimationPlaybackEventInit const& event_init)
{
    return realm.heap().allocate<AnimationPlaybackEvent>(realm, realm, type, event_init);
}

// https://www.w3.org/TR/web-animations-1/#dom-animationplaybackevent-animationplaybackevent
WebIDL::ExceptionOr<JS::NonnullGCPtr<AnimationPlaybackEvent>> AnimationPlaybackEvent::construct_impl(JS::Realm& realm, FlyString const& type, AnimationPlaybackEventInit const& event_init)
{
    return create(realm, type, event_init);
}

AnimationPlaybackEvent::AnimationPlaybackEvent(JS::Realm& realm, FlyString const& type, AnimationPlaybackEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_current_time(event_init.current_time)
    , m_timeline_time(event_init.timeline_time)
{
}

void AnimationPlaybackEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationPlaybackEvent);
}

}
