/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Bindings/InternalAnimationTimelinePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalAnimationTimeline.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(InternalAnimationTimeline);

void InternalAnimationTimeline::update_current_time(double)
{
    // Do nothing
}

void InternalAnimationTimeline::set_time(Optional<double> time)
{
    set_current_time(time.map([](double value) -> Animations::TimeValue { return { Animations::TimeValue::Type::Milliseconds, value }; }));

    // https://drafts.csswg.org/web-animations-1/#animation-frame-loop
    // Note: Due to the hierarchical nature of the timing model, updating the current time of a timeline also involves:
    // - Updating the current time of any animations associated with the timeline.
    // - Running the update an animation's finished state procedure for any animations whose current time has been
    //   updated.
    // - Queueing animation events for any such animations.
    // NB: This mirrors what the event loop does for DocumentTimeline in Document::update_animations_and_send_events().
    for (auto& animation : associated_animations())
        animation.update();
}

InternalAnimationTimeline::InternalAnimationTimeline(JS::Realm& realm)
    : AnimationTimeline(realm)
{
    m_current_time = { Animations::TimeValue::Type::Milliseconds, 0.0 };
    m_is_monotonically_increasing = true;

    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    document.associate_with_timeline(*this);
}

void InternalAnimationTimeline::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(InternalAnimationTimeline);
    Base::initialize(realm);
}

}
