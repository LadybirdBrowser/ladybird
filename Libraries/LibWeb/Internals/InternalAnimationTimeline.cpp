/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    set_current_time(time);
}

InternalAnimationTimeline::InternalAnimationTimeline(JS::Realm& realm)
    : AnimationTimeline(realm)
{
    m_current_time = 0.0;
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
