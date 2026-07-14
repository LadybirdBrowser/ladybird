/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AnimationEvent.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(AnimationEvent);

GC::Ref<AnimationEvent> AnimationEvent::create(JS::Realm& realm, Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init)
{
    return realm.create<AnimationEvent>(realm, type, event_init);
}

WebIDL::ExceptionOr<GC::Ref<AnimationEvent>> AnimationEvent::construct_impl(JS::Realm& realm, Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init)
{
    return create(realm, type, event_init);
}

AnimationEvent::AnimationEvent(JS::Realm& realm, Utf16FlyString const& type, Bindings::AnimationEventInit const& event_init)
    : DOM::Event(realm, type, event_init)
    , m_animation_name(event_init.animation_name)
    , m_elapsed_time(event_init.elapsed_time)
    , m_pseudo_element(event_init.pseudo_element)
    , m_animation(event_init.animation)
{
}

void AnimationEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationEvent);
    Base::initialize(realm);
}

void AnimationEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
}

}
