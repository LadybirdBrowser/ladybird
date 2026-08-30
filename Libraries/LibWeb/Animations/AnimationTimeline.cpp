/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationTimeline);

AnimationTimeline::CurrentTimeOverrideScope::CurrentTimeOverrideScope(AnimationTimeline& timeline, Optional<TimeValue> current_time)
    : m_timeline(timeline)
    , m_previous_current_time_override(timeline.m_current_time_override_for_style_sampling)
    , m_had_previous_current_time_override(timeline.m_has_current_time_override_for_style_sampling)
{
    m_timeline->set_current_time_override_for_style_sampling(move(current_time));
}

AnimationTimeline::CurrentTimeOverrideScope::~CurrentTimeOverrideScope()
{
    m_timeline->m_current_time_override_for_style_sampling = move(m_previous_current_time_override);
    m_timeline->m_has_current_time_override_for_style_sampling = m_had_previous_current_time_override;
}

// https://drafts.csswg.org/web-animations-1/#dom-animationtimeline-currenttime
Optional<TimeValue> AnimationTimeline::current_time() const
{
    // Returns the current time for this timeline or null if this timeline is inactive.
    if (is_inactive())
        return {};
    return effective_current_time();
}

NullableCSSNumberish AnimationTimeline::current_time_for_bindings()
{
    return NullableCSSNumberish::from_optional_css_numberish_time(current_time_for_observation());
}

Optional<TimeValue> AnimationTimeline::current_time_for_observation()
{
    if (!m_is_monotonically_increasing || !can_sample_current_time_at_timestamp())
        return current_time();

    bool has_animation_without_per_frame_tick = false;
    for (auto const& animation : m_associated_animations) {
        auto effect = animation.effect();
        if (!effect || !is<KeyframeEffect>(*effect))
            continue;
        auto& keyframe_effect = static_cast<KeyframeEffect&>(*effect);
        if (keyframe_effect.per_frame_animation_tick_was_skipped() || keyframe_effect.can_skip_per_frame_animation_tick()) {
            has_animation_without_per_frame_tick = true;
            break;
        }
    }
    if (!has_animation_without_per_frame_tick)
        return current_time();

    auto& settings = associated_document()->relevant_settings_object();
    auto task_generation = settings.responsible_event_loop().task_generation();
    if (m_last_current_time_update_task_generation == task_generation)
        return m_observed_current_time;

    // OPTIMIZATION: An animation skipped by the per-frame sampler does not keep the rendering update loop alive.
    //               Sample document timelines only when script observes them, and at most once per event-loop task
    //               so all reads within a stable state agree.
    auto timestamp = HighResolutionTime::relative_high_resolution_time(
        HighResolutionTime::unsafe_shared_current_time(), settings.global_object());
    m_observed_current_time = current_time_at_timestamp(timestamp);
    m_last_current_time_update_task_generation = task_generation;
    ++associated_document()->style_invalidation_counters().animation_timeline_synchronizations;
    return m_observed_current_time;
}

void AnimationTimeline::set_current_time(Optional<TimeValue> value)
{
    if (m_is_monotonically_increasing && m_current_time.has_value() && (!value.has_value() || *value < *m_current_time)) {
        dbgln("AnimationTimeline::set_current_time({}): monotonically increasing timeline can only move forward", value);
        return;
    }

    m_current_time = value;
    m_observed_current_time = value;
    m_last_current_time_update_task_generation = associated_document()->relevant_settings_object().responsible_event_loop().task_generation();

    update_associated_animations();
}

void AnimationTimeline::update_associated_animations()
{
    // https://drafts.csswg.org/web-animations-1/#animation-frame-loop
    // Note: Due to the hierarchical nature of the timing model, updating the current time of a timeline also involves:
    // - Updating the current time of any animations associated with the timeline.
    // - Running the update an animation's finished state procedure for any animations whose current time has been
    //   updated.
    // - Queueing animation events for any such animations.
    // NB: Since we dispatch events for all animations regardless of whether they have a timeline we handle them all together in Document::update_animations_and_send_events()
    ++associated_document()->style_invalidation_counters().animation_timeline_associated_animation_updates;
    for (auto& animation : m_associated_animations)
        animation.update();
}

Optional<TimeValue> AnimationTimeline::effective_current_time() const
{
    if (m_has_current_time_override_for_style_sampling)
        return m_current_time_override_for_style_sampling;
    return m_current_time;
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
    return NullableCSSNumberish::from_optional_css_numberish_time(duration());
}

// https://drafts.csswg.org/web-animations-1/#timeline
bool AnimationTimeline::is_inactive() const
{
    // A timeline is considered to be inactive when its time value is unresolved, and active otherwise.
    return !effective_current_time().has_value();
}

AnimationTimeline::AnimationTimeline(GC::Ref<DOM::Document> document)
    : m_associated_document(document)
{
    m_associated_document->associate_with_timeline(*this);
}

void AnimationTimeline::finalize()
{
    Base::finalize();
    m_associated_document->disassociate_with_timeline(*this);
}

void AnimationTimeline::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_document);
    // We intentionally don't visit m_associated_animations here to avoid keeping Animations alive solely because they
    // are associated with a timeline. Animations are disassociated from timelines in Animation::finalize() so we don't
    // need to worry about dangling references.
}

}
