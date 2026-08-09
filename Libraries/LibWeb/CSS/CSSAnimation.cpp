/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSAnimation);

GC::Ref<CSSAnimation> CSSAnimation::create(HTML::EnvironmentSettingsObject& environment)
{
    return GC::Heap::the().allocate<CSSAnimation>(environment);
}

// https://www.w3.org/TR/css-animations-2/#animation-composite-order
int CSSAnimation::class_specific_composite_order(GC::Ref<Animations::Animation> other_animation) const
{
    auto other = GC::Ref { as<CSSAnimation>(*other_animation) };

    // The existence of an owning element determines the animation class, so both animations should have their owning
    // element in the same state
    VERIFY(owning_element().has_value() == other->owning_element().has_value());

    // Within the set of CSS Animations with an owning element, two animations A and B are sorted in composite order
    // (first to last) as follows:
    if (owning_element().has_value()) {
        // 1. If the owning element of A and B differs, sort A and B by tree order of their corresponding owning elements.
        //    With regard to pseudo-elements, the sort order is as follows:
        //    - element
        //    - ::marker
        //    - ::before
        //    - any other pseudo-elements not mentioned specifically in this list, sorted in ascending order by the Unicode
        //      codepoints that make up each selector
        //    - ::after
        //    - element children
        if (owning_element() != other->owning_element()) {
            // FIXME: Sort by tree order
            return 0;
        }

        // 2. Otherwise, sort A and B based on their position in the computed value of the animation-name property of the
        //    (common) owning element.
        return m_animation_name_index - other->m_animation_name_index;
    }

    // The composite order of CSS Animations without an owning element is based on their position in the global animation list.
    return global_animation_list_order() - other->global_animation_list_order();
}

Animations::AnimationClass CSSAnimation::animation_class() const
{
    if (owning_element().has_value())
        return Animations::AnimationClass::CSSAnimationWithOwningElement;
    return Animations::AnimationClass::CSSAnimationWithoutOwningElement;
}

// NB: Unrelated style changes shouldn't cause us to recreate anonymous timelines, to achieve this we drop updates
//     between two equivalent anonymous timelines.
static bool should_update_timeline(GC::Ptr<Animations::AnimationTimeline> old_timeline, GC::Ptr<Animations::AnimationTimeline> new_timeline)
{
    if (old_timeline == new_timeline)
        return false;

    if (!old_timeline || !new_timeline)
        return true;

    if (is<Animations::ScrollTimeline>(*old_timeline) && is<Animations::ScrollTimeline>(*new_timeline)) {
        auto const& old_scroll_timeline = as<Animations::ScrollTimeline>(*old_timeline);
        auto const& new_scroll_timeline = as<Animations::ScrollTimeline>(*new_timeline);

        if (!old_scroll_timeline.source_internal().has<Animations::ScrollTimeline::AnonymousSource>() || !new_scroll_timeline.source_internal().has<Animations::ScrollTimeline::AnonymousSource>())
            return true;

        return old_scroll_timeline.source_internal().get<Animations::ScrollTimeline::AnonymousSource>() != new_scroll_timeline.source_internal().get<Animations::ScrollTimeline::AnonymousSource>();
    }

    return true;
}

void CSSAnimation::apply_css_properties(AnimationProperties const& animation_properties)
{
    // FIXME: Don't apply overridden properties as defined here: https://drafts.csswg.org/css-animations-2/#animations

    VERIFY(effect());

    auto& effect = as<Animations::KeyframeEffect>(*this->effect());

    auto const update_timeline = !m_ignored_css_properties.contains(PropertyID::AnimationTimeline)
        && should_update_timeline(timeline(), animation_properties.timeline);
    AppliedCSSProperties applied_properties {
        .duration = animation_properties.duration,
        .timing_function = animation_properties.timing_function,
        .iteration_count = animation_properties.iteration_count,
        .direction = animation_properties.direction,
        .play_state = animation_properties.play_state,
        .delay = animation_properties.delay,
        .fill_mode = animation_properties.fill_mode,
        .composition = animation_properties.composition,
    };
    // NB: Style feedback can recompute an animation's owning element. Reapplying unchanged
    //     properties would invalidate the effect and turn that feedback into an endless cycle.
    if (!update_timeline && m_applied_css_properties == applied_properties)
        return;
    m_applied_css_properties = applied_properties;

    if (update_timeline) {
        HTML::TemporaryExecutionContext context(relevant_settings_object());
        set_timeline(animation_properties.timeline);
    }

    effect.set_specified_iteration_duration(animation_properties.duration);
    effect.set_specified_start_delay(animation_properties.delay);
    effect.set_iteration_count(animation_properties.iteration_count);
    // https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
    // Timing properties may also be updated due to a style change. Any change to a CSS animation property that affects
    // timing requires rerunning the procedure to normalize specified timing.
    effect.normalize_specified_timing();
    // NB: animation-timing-function is applied per-keyframe, not as the effect-level timing function.
    //     The effect-level timing function remains linear.
    m_default_easing = animation_properties.timing_function;
    effect.set_fill_mode(Animations::css_fill_mode_to_bindings_fill_mode(animation_properties.fill_mode));
    effect.set_playback_direction(Animations::css_animation_direction_to_bindings_playback_direction(animation_properties.direction));
    effect.set_composite(Animations::css_animation_composition_to_composite_operation(animation_properties.composition));

    if (animation_properties.play_state != last_css_animation_play_state()) {
        if (!m_script_overrode_play_state) {
            if (animation_properties.play_state == CSS::AnimationPlayState::Running && play_state() != Animations::AnimationPlayState::Running) {
                HTML::TemporaryExecutionContext context(relevant_settings_object());
                play_from_css();
            } else if (animation_properties.play_state == CSS::AnimationPlayState::Paused && play_state() != Animations::AnimationPlayState::Paused) {
                HTML::TemporaryExecutionContext context(relevant_settings_object());
                pause_from_css();
            }
        }

        set_last_css_animation_play_state(animation_properties.play_state);
    }
}

void CSSAnimation::mark_script_play_state_override()
{
    if (!m_applying_css_play_state)
        m_script_overrode_play_state = true;
}

void CSSAnimation::play_from_css()
{
    m_applying_css_play_state = true;
    Animations::Animation::play().release_value_but_fixme_should_propagate_errors();
    m_applying_css_play_state = false;
}

void CSSAnimation::pause_from_css()
{
    m_applying_css_play_state = true;
    Animations::Animation::pause().release_value_but_fixme_should_propagate_errors();
    m_applying_css_play_state = false;
}

void CSSAnimation::cancel_from_css()
{
    m_applying_css_play_state = true;
    Animations::Animation::cancel();
    m_applying_css_play_state = false;
}

WebIDL::ExceptionOr<void> CSSAnimation::set_start_time_for_bindings(Animations::NullableCSSNumberish const& value)
{
    auto previous_play_state = play_state();
    auto result = Animations::Animation::set_start_time_for_bindings(value);
    if (!result.is_error() && previous_play_state != play_state())
        mark_script_play_state_override();
    return result;
}

WebIDL::ExceptionOr<void> CSSAnimation::set_current_time_for_bindings(Animations::NullableCSSNumberish const& value)
{
    auto previous_play_state = play_state();
    auto result = Animations::Animation::set_current_time_for_bindings(value);
    if (!result.is_error() && previous_play_state != play_state())
        mark_script_play_state_override();
    return result;
}

WebIDL::ExceptionOr<void> CSSAnimation::set_playback_rate(double value)
{
    return Animations::Animation::set_playback_rate(value);
}

void CSSAnimation::cancel(Animations::Animation::ShouldInvalidate should_invalidate)
{
    mark_script_play_state_override();
    Animations::Animation::cancel(should_invalidate);
}

WebIDL::ExceptionOr<void> CSSAnimation::play(Animations::Animation::ShouldInvalidate should_invalidate)
{
    auto previous_play_state = play_state();
    auto result = Animations::Animation::play(should_invalidate);
    if (!result.is_error() && previous_play_state != play_state())
        mark_script_play_state_override();
    return result;
}

WebIDL::ExceptionOr<void> CSSAnimation::pause()
{
    auto result = Animations::Animation::pause();
    if (!result.is_error())
        mark_script_play_state_override();
    return result;
}

WebIDL::ExceptionOr<void> CSSAnimation::update_playback_rate(double value)
{
    return Animations::Animation::update_playback_rate(value);
}

WebIDL::ExceptionOr<void> CSSAnimation::reverse()
{
    auto previous_play_state = play_state();
    auto result = Animations::Animation::reverse();
    if (!result.is_error() && previous_play_state != play_state())
        mark_script_play_state_override();
    return result;
}

void CSSAnimation::set_timeline_for_bindings(GC::Ptr<Animations::AnimationTimeline> timeline)
{
    // AD-HOC: When the timeline of a CSS animation is modified by the author from JS we should no longer apply changes
    //         to the `animation-timeline` property. See https://github.com/w3c/csswg-drafts/issues/13472
    m_ignored_css_properties.set(PropertyID::AnimationTimeline);
    set_timeline(timeline);
}

CSSAnimation::CSSAnimation(HTML::EnvironmentSettingsObject& environment)
    : Animations::Animation(environment)
{
    // FIXME:
    // CSS Animations generated using the markup defined in this specification are not added to the global animation
    // list when they are created. Instead, these animations are appended to the global animation list at the first
    // moment when they transition out of the idle play state after being disassociated from their owning element. CSS
    // Animations that have been disassociated from their owning element but are still idle do not have a defined
    // composite order.
}

}
