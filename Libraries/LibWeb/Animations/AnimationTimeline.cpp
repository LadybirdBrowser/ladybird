/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/AnimationTimelinePrototype.h>
#include <LibWeb/DOM/Document.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationTimeline);

// https://drafts.csswg.org/web-animations-1/#dom-animationtimeline-currenttime
Optional<TimeValue> AnimationTimeline::current_time() const
{
    // Returns the current time for this timeline or null if this timeline is inactive.
    if (is_inactive())
        return {};
    return m_current_time;
}

void AnimationTimeline::set_current_time(Optional<TimeValue> value)
{
    if (value == m_current_time)
        return;

    if (m_is_monotonically_increasing && m_current_time.has_value() && (!value.has_value() || *value < *m_current_time)) {
        dbgln("AnimationTimeline::set_current_time({}): monotonically increasing timeline can only move forward", value);
        return;
    }

    m_current_time = value;
}

// https://drafts.csswg.org/web-animations-2/#timeline-duration
NullableCSSNumberish AnimationTimeline::duration_for_bindings() const
{
    // The duration of a timeline gives the maximum value a timeline may generate for its current time. This value is
    // used to calculate the intrinsic iteration duration for the target effect of an animation that is associated with
    // the timeline when the effect’s iteration duration is "auto". The value is computed such that the effect fills the
    // available time. For a monotonic timeline, there is no upper bound on current time, and timeline duration is
    // unresolved. For a non-monotonic (e.g. scroll) timeline, the duration has a fixed upper bound. In this case, the
    // timeline is a progress-based timeline, and its timeline duration is 100%.
    return NullableCSSNumberish::from_optional_css_numberish_time(realm(), duration());
}

void AnimationTimeline::set_associated_document(GC::Ptr<DOM::Document> document)
{
    if (document)
        document->associate_with_timeline(*this);
    if (m_associated_document)
        m_associated_document->disassociate_with_timeline(*this);
    m_associated_document = document;
}

// https://drafts.csswg.org/web-animations-1/#timeline
bool AnimationTimeline::is_inactive() const
{
    // A timeline is considered to be inactive when its time value is unresolved, and active otherwise.
    return !m_current_time.has_value();
}

AnimationTimeline::AnimationTimeline(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void AnimationTimeline::finalize()
{
    Base::finalize();
    if (m_associated_document)
        m_associated_document->disassociate_with_timeline(*this);
}

void AnimationTimeline::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationTimeline);
    Base::initialize(realm);
}

void AnimationTimeline::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_document);
    // We intentionally don't visit m_associated_animations here to avoid keeping Animations alive solely because they
    // are associated with a timeline. Animations are disassociated from timelines in Animation::finalize() so we don't
    // need to worry about dangling references.
}

}
