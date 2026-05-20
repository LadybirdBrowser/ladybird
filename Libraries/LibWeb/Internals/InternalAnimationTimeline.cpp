/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Bindings/InternalAnimationTimeline.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalAnimationTimeline.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(InternalAnimationTimeline);

void InternalAnimationTimeline::update_current_time(double)
{
    update_associated_animations_and_dispatch_events();
}

void InternalAnimationTimeline::set_time(Optional<double> time)
{
    set_current_time(time.map([](double value) -> Animations::TimeValue { return { Animations::TimeValue::Type::Milliseconds, value }; }));
}

InternalAnimationTimeline::InternalAnimationTimeline(JS::Realm& realm, GC::Ref<DOM::Document> document)
    : AnimationTimeline(realm, document)
{
    m_current_time = { Animations::TimeValue::Type::Milliseconds, 0.0 };
    m_is_monotonically_increasing = true;
}

void InternalAnimationTimeline::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(InternalAnimationTimeline);
    Base::initialize(realm);
}

}
