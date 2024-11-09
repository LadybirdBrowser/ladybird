/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AnimationEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/AnimationEvent.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(AnimationEvent);

JS::NonnullGCPtr<AnimationEvent> AnimationEvent::create(JS::Realm& realm, FlyString const& type, AnimationEventInit const& event_init)
{
    return realm.heap().allocate<AnimationEvent>(realm, realm, type, event_init);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<AnimationEvent>> AnimationEvent::construct_impl(JS::Realm& realm, FlyString const& type, AnimationEventInit const& event_init)
{
    return create(realm, type, event_init);
}

AnimationEvent::AnimationEvent(JS::Realm& realm, FlyString const& type, AnimationEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_animation_name(event_init.animation_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
{
}

void AnimationEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationEvent);
}

}
