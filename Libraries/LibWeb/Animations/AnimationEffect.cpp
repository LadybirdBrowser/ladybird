/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Bitmap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/AnimationEffectPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

GC_DEFINE_ALLOCATOR(AnimationEffect);

Bindings::FillMode css_fill_mode_to_bindings_fill_mode(CSS::AnimationFillMode mode)
{
    switch (mode) {
    case CSS::AnimationFillMode::Backwards:
        return Bindings::FillMode::Backwards;
    case CSS::AnimationFillMode::Both:
        return Bindings::FillMode::Both;
    case CSS::AnimationFillMode::Forwards:
        return Bindings::FillMode::Forwards;
    case CSS::AnimationFillMode::None:
        return Bindings::FillMode::None;
    default:
        VERIFY_NOT_REACHED();
    }
}

Bindings::PlaybackDirection css_animation_direction_to_bindings_playback_direction(CSS::AnimationDirection direction)
{
    switch (direction) {
    case CSS::AnimationDirection::Alternate:
        return Bindings::PlaybackDirection::Alternate;
    case CSS::AnimationDirection::AlternateReverse:
        return Bindings::PlaybackDirection::AlternateReverse;
    case CSS::AnimationDirection::Normal:
        return Bindings::PlaybackDirection::Normal;
    case CSS::AnimationDirection::Reverse:
        return Bindings::PlaybackDirection::Reverse;
    default:
        VERIFY_NOT_REACHED();
    }
}

OptionalEffectTiming EffectTiming::to_optional_effect_timing() const
{
    return {
        .delay = delay,
        .end_delay = end_delay,
        .fill = fill,
        .iteration_start = iteration_start,
        .iterations = iterations,
        .duration = duration.visit(
            [](double const& value) -> Variant<double, String> { return value; },
            [](String const& value) -> Variant<double, String> { return value; },
            // NB: We check that this isn't the case in the caller
            [](GC::Root<CSS::CSSNumericValue> const&) -> Variant<double, String> { VERIFY_NOT_REACHED(); }),
        .direction = direction,
        .easing = easing,
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-gettiming
EffectTiming AnimationEffect::get_timing() const
{
    // 1. Returns the specified timing properties for this animation effect.
    return {
        .delay = m_specified_start_delay,
        .end_delay = m_specified_end_delay,
        .fill = m_fill_mode,
        .iteration_start = m_iteration_start,
        .iterations = m_iteration_count,
        .duration = m_specified_iteration_duration,
        .direction = m_playback_direction,
        .easing = m_timing_function.to_string(),
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-getcomputedtiming
// https://drafts.csswg.org/web-animations-2/#dom-animationeffect-getcomputedtiming
ComputedEffectTiming AnimationEffect::get_computed_timing() const
{
    // 1. Returns the calculated timing properties for this animation effect.

    // Note: Although some of the attributes of the object returned by getTiming() and getComputedTiming() are common,
    //       their values may differ in the following ways:

    //     - duration: while getTiming() may return the string auto, getComputedTiming() must return a number
    //       corresponding to the calculated value of the iteration duration as defined in the description of the
    //       duration member of the EffectTiming interface.
    //
    //       If duration is the string auto, this attribute will return the current calculated value of the intrinsic
    //       iteration duration, which may be a expressed as a double representing the duration in milliseconds or a
    //       percentage when the effect is associated with a progress-based timeline.
    auto duration = m_iteration_duration.as_css_numberish(realm());

    //     - fill: likewise, while getTiming() may return the string auto, getComputedTiming() must return the specific
    //       FillMode used for timing calculations as defined in the description of the fill member of the EffectTiming
    //       interface.
    //
    //       In this level of the specification, that simply means that an auto value is replaced by the none FillMode.
    auto fill = m_fill_mode == Bindings::FillMode::Auto ? Bindings::FillMode::None : m_fill_mode;

    return {
        {
            .delay = m_specified_start_delay,
            .end_delay = m_specified_end_delay,
            .fill = fill,
            .iteration_start = m_iteration_start,
            .iterations = m_iteration_count,
            .duration = duration,
            .direction = m_playback_direction,
            .easing = m_timing_function.to_string(),
        },

        end_time().as_css_numberish(realm()),
        active_duration().as_css_numberish(realm()),
        NullableCSSNumberish::from_optional_css_numberish_time(realm(), local_time()),
        transformed_progress(),
        current_iteration(),
    };
}

// https://drafts.csswg.org/web-animations-2/#intrinsic-iteration-duration
TimeValue AnimationEffect::intrinsic_iteration_duration() const
{
    // The intrinsic iteration duration is calculated from the first matching condition from below:

    // FIXME: If the animation effect is a group effect,
    if (false) {
        // Follow the procedure in § 2.10.3 The intrinsic iteration duration of a group effect
        TODO();
    }

    // FIXME: If the animation effect is a sequence effect,
    else if (false) {
        // Follow the procedure in § 2.10.4.2 The intrinsic iteration duration of a sequence effect
        TODO();
    }

    // If timeline duration is unresolved or iteration count is zero,
    else if (!timeline_duration().has_value() || m_iteration_count == 0.0) {
        // Return 0
        return TimeValue::create_zero(associated_timeline());
    }

    // Otherwise
    else {
        // Return(100% - start delay - end delay) / iteration count
        // Note : Presently start and end delays are zero until such time as percentage based delays are supported.
        auto one_hundred_percent = TimeValue { TimeValue::Type::Percentage, 100.0 };

        return (one_hundred_percent - m_start_delay - m_end_delay) / m_iteration_count;
    }
}

GC::Ptr<AnimationTimeline> AnimationEffect::associated_timeline() const
{
    if (!m_associated_animation)
        return {};

    return m_associated_animation->timeline();
}

Optional<TimeValue> AnimationEffect::timeline_duration() const
{
    auto timeline = associated_timeline();
    if (!timeline)
        return {};

    return timeline->duration();
}

// https://drafts.csswg.org/web-animations-2/#time-based-animation-to-a-proportional-animation
void AnimationEffect::convert_a_time_based_animation_to_a_proportional_animation()
{
    // AD-HOC: We use the specified interation duration instead of the iteration duration here, see
    //         https://github.com/w3c/csswg-drafts/pull/13170
    // If the iteration duration is auto, then perform the following steps.
    if (m_specified_iteration_duration.has<String>()) {
        // Set start delay and end delay to 0, as it is not possible to mix time and proportions.
        // Note: Future versions may allow these properties to be assigned percentages, at which point the delays are
        //       only to be ignored if their values are expressed as times and not as percentages.
        m_start_delay = TimeValue::create_zero(associated_timeline());
        m_end_delay = TimeValue::create_zero(associated_timeline());

        // AD-HOC: The spec doesn't say what to set iteration duration to in this case so we set it to the intrinsic
        //         iteration duration, see: https://github.com/w3c/csswg-drafts/issues/13220
        m_iteration_duration = intrinsic_iteration_duration();
        return;
    }

    // Otherwise:

    // NB: The caller asserts that timeline duration is resolved
    auto const& timeline_duration = this->timeline_duration().value();

    // 1. Let total time be equal to end time
    // AD-HOC: Using end time here only works if we haven't already converted to a proportional animation, we instead
    //         recompute the specified equivalent of "end time", see https://github.com/w3c/csswg-drafts/issues/13230
    auto total_time = max(m_specified_start_delay + (m_specified_iteration_duration.get<double>() * m_iteration_count) + m_specified_end_delay, 0);

    // AD-HOC: Avoid a division by zero below, see https://github.com/w3c/csswg-drafts/issues/11276
    if (total_time == 0) {
        m_start_delay = TimeValue::create_zero(associated_timeline());
        m_iteration_duration = TimeValue::create_zero(associated_timeline());
        m_end_delay = TimeValue::create_zero(associated_timeline());
        return;
    }

    // 2. Set start delay to be the result of evaluating specified start delay / total time * timeline duration.
    m_start_delay = timeline_duration * (m_specified_start_delay / total_time);

    // 3. Set iteration duration to be the result of evaluating specified iteration duration / total time * timeline duration.
    m_iteration_duration = timeline_duration * (m_specified_iteration_duration.get<double>() / total_time);

    // 4. Set end delay to be the result of evaluating specified end delay / total time * timeline duration.
    m_end_delay = timeline_duration * (m_specified_end_delay / total_time);
}

// https://drafts.csswg.org/web-animations-2/#normalize-specified-timing
void AnimationEffect::normalize_specified_timing()
{
    // If timeline duration is resolved:
    if (timeline_duration().has_value()) {
        // Follow the procedure to convert a time-based animation to a proportional animation
        convert_a_time_based_animation_to_a_proportional_animation();
    }
    // Otherwise:
    else {
        // 1. Set start delay = specified start delay
        m_start_delay = TimeValue { TimeValue::Type::Milliseconds, m_specified_start_delay };

        // 2. Set end delay = specified end delay
        m_end_delay = TimeValue { TimeValue::Type::Milliseconds, m_specified_end_delay };

        // 3. If iteration duration is auto:
        // AD-HOC: We use the specified interation duration instead of the iteration duration here, see
        //         https://github.com/w3c/csswg-drafts/pull/13170
        if (m_specified_iteration_duration.has<String>()) {
            // Set iteration duration = intrinsic iteration duration
            m_iteration_duration = intrinsic_iteration_duration();
        }
        // Otherwise:
        else {
            // Set iteration duration = specified iteration duration
            m_iteration_duration = TimeValue { TimeValue::Type::Milliseconds, m_specified_iteration_duration.get<double>() };
        }
    }
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-updatetiming
// https://www.w3.org/TR/web-animations-1/#update-the-timing-properties-of-an-animation-effect
// https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
WebIDL::ExceptionOr<void> AnimationEffect::update_timing(OptionalEffectTiming timing)
{
    // 1. If the iterationStart member of input exists and is less than zero, throw a TypeError and abort this
    //    procedure.
    if (timing.iteration_start.has_value() && timing.iteration_start.value() < 0.0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid iteration start value"sv };

    // 2. If the iterations member of input exists, and is less than zero or is the value NaN, throw a TypeError and
    //    abort this procedure.
    if (timing.iterations.has_value() && (timing.iterations.value() < 0.0 || isnan(timing.iterations.value())))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid iteration count value"sv };

    // 3. If the duration member of input exists, and is less than zero or is the value NaN, throw a TypeError and
    //    abort this procedure.
    // Note: "auto", the only valid string value, is treated as 0.
    auto& duration = timing.duration;
    auto has_valid_duration_value = [&] {
        if (!duration.has_value())
            return true;
        if (duration->has<double>() && (duration->get<double>() < 0.0 || isnan(duration->get<double>())))
            return false;
        if (duration->has<String>() && (duration->get<String>() != "auto"))
            return false;
        return true;
    }();
    if (!has_valid_duration_value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid duration value"sv };

    // 4. If the easing member of input exists but cannot be parsed using the <easing-function> production
    //    [CSS-EASING-1], throw a TypeError and abort this procedure.
    Optional<CSS::EasingFunction> easing_value;
    if (timing.easing.has_value()) {
        easing_value = parse_easing_string(timing.easing.value());
        if (!easing_value.has_value())
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid easing function"sv };
    }

    // 5. Assign each member that exists in input to the corresponding timing property of effect as follows:

    //    - delay → specified start delay
    if (timing.delay.has_value())
        m_specified_start_delay = timing.delay.value();

    //    - endDelay → specified end delay
    if (timing.end_delay.has_value())
        m_specified_end_delay = timing.end_delay.value();

    //    - fill → fill mode
    if (timing.fill.has_value())
        m_fill_mode = timing.fill.value();

    //    - iterationStart → iteration start
    if (timing.iteration_start.has_value())
        m_iteration_start = timing.iteration_start.value();

    //    - iterations → iteration count
    if (timing.iterations.has_value())
        m_iteration_count = timing.iterations.value();

    //    - duration → specified iteration duration
    if (timing.duration.has_value())
        m_specified_iteration_duration = timing.duration.value();

    //    - direction → playback direction
    if (timing.direction.has_value())
        m_playback_direction = timing.direction.value();

    //    - easing → timing function
    if (easing_value.has_value())
        m_timing_function = easing_value.value();

    // 6. Follow the procedure to normalize specified timing.
    normalize_specified_timing();

    // AD-HOC: Notify the associated animation that the effect timing has changed.
    if (auto animation = m_associated_animation)
        animation->effect_timing_changed({});

    return {};
}

void AnimationEffect::set_associated_animation(GC::Ptr<Animation> value)
{
    m_associated_animation = value;

    // NB: The normalization of the specified timing depends on the timeline of the associated animation.
    normalize_specified_timing();
}

// https://www.w3.org/TR/web-animations-1/#animation-direction
AnimationDirection AnimationEffect::animation_direction() const
{
    // "backwards" if the effect is associated with an animation and the associated animation’s playback rate is less
    // than zero; in all other cases, the animation direction is "forwards".
    if (m_associated_animation && m_associated_animation->playback_rate() < 0.0)
        return AnimationDirection::Backwards;
    return AnimationDirection::Forwards;
}

// https://www.w3.org/TR/web-animations-1/#end-time
TimeValue AnimationEffect::end_time() const
{
    // 1. The end time of an animation effect is the result of evaluating
    //    max(start delay + active duration + end delay, 0).
    return max(m_start_delay + active_duration() + m_end_delay, TimeValue::create_zero(associated_timeline()));
}

// https://www.w3.org/TR/web-animations-1/#local-time
Optional<TimeValue> AnimationEffect::local_time() const
{
    // The local time of an animation effect at a given moment is based on the first matching condition from the
    // following:

    // -> If the animation effect is associated with an animation,
    if (m_associated_animation) {
        // the local time is the current time of the animation.
        return m_associated_animation->current_time();
    }

    // -> Otherwise,
    //    the local time is unresolved.
    return {};
}

// https://www.w3.org/TR/web-animations-1/#active-duration
TimeValue AnimationEffect::active_duration() const
{
    // The active duration is calculated as follows:
    //     active duration = iteration duration × iteration count
    // If either the iteration duration or iteration count are zero, the active duration is zero. This clarification is
    // needed since the result of infinity multiplied by zero is undefined according to IEEE 754-2008.
    if (m_iteration_duration.value == 0 || m_iteration_count == 0.0)
        return TimeValue::create_zero(associated_timeline());

    return m_iteration_duration * m_iteration_count;
}

Optional<TimeValue> AnimationEffect::active_time() const
{
    return active_time_using_fill(m_fill_mode);
}

// https://www.w3.org/TR/web-animations-1/#calculating-the-active-time
Optional<TimeValue> AnimationEffect::active_time_using_fill(Bindings::FillMode fill_mode) const
{
    // The active time is based on the local time and start delay. However, it is only defined when the animation effect
    // should produce an output and hence depends on its fill mode and phase as follows,

    // -> If the animation effect is in the before phase,
    if (is_in_the_before_phase()) {
        // The result depends on the first matching condition from the following,

        // -> If the fill mode is backwards or both,
        if (fill_mode == Bindings::FillMode::Backwards || fill_mode == Bindings::FillMode::Both) {
            // Return the result of evaluating max(local time - start delay, 0).
            return max(local_time().value() - m_start_delay, TimeValue::create_zero(associated_timeline()));
        }

        // -> Otherwise,
        //    Return an unresolved time value.
        return {};
    }

    // -> If the animation effect is in the active phase,
    if (is_in_the_active_phase()) {
        // Return the result of evaluating local time - start delay.
        return local_time().value() - m_start_delay;
    }

    // -> If the animation effect is in the after phase,
    if (is_in_the_after_phase()) {
        // The result depends on the first matching condition from the following,

        // -> If the fill mode is forwards or both,
        if (fill_mode == Bindings::FillMode::Forwards || fill_mode == Bindings::FillMode::Both) {
            // Return the result of evaluating max(min(local time - start delay, active duration), 0).
            return max(min(local_time().value() - m_start_delay, active_duration()), TimeValue::create_zero(associated_timeline()));
        }

        // -> Otherwise,
        //    Return an unresolved time value.
        return {};
    }

    // -> Otherwise (the local time is unresolved),
    //    Return an unresolved time value.
    return {};
}

// https://www.w3.org/TR/web-animations-1/#in-play
bool AnimationEffect::is_in_play() const
{
    // An animation effect is in play if all of the following conditions are met:
    // - the animation effect is in the active phase, and
    // - the animation effect is associated with an animation that is not finished.
    return is_in_the_active_phase() && m_associated_animation && !m_associated_animation->is_finished();
}

// https://www.w3.org/TR/web-animations-1/#current
bool AnimationEffect::is_current() const
{
    // An animation effect is current if any of the following conditions are true:

    // - the animation effect is in play, or
    if (is_in_play())
        return true;

    if (auto animation = m_associated_animation) {
        auto playback_rate = animation->playback_rate();

        // - the animation effect is associated with an animation with a playback rate > 0 and the animation effect is
        //   in the before phase, or
        if (playback_rate > 0.0 && is_in_the_before_phase())
            return true;

        // - the animation effect is associated with an animation with a playback rate < 0 and the animation effect is
        //   in the after phase, or
        if (playback_rate < 0.0 && is_in_the_after_phase())
            return true;

        // - the animation effect is associated with an animation not in the idle play state with a non-null associated
        //   timeline that is not monotonically increasing.
        if (animation->play_state() != Bindings::AnimationPlayState::Idle && animation->timeline() && !animation->timeline()->is_monotonically_increasing())
            return true;
    }

    return false;
}

// https://www.w3.org/TR/web-animations-1/#in-effect
bool AnimationEffect::is_in_effect() const
{
    // An animation effect is in effect if its active time, as calculated according to the procedure in
    // §4.8.3.1 Calculating the active time, is not unresolved.
    return active_time().has_value();
}

// https://www.w3.org/TR/web-animations-1/#before-active-boundary-time
TimeValue AnimationEffect::before_active_boundary_time() const
{
    // max(min(start delay, end time), 0)
    return max(min(m_start_delay, end_time()), TimeValue::create_zero(associated_timeline()));
}

// https://www.w3.org/TR/web-animations-1/#active-after-boundary-time
TimeValue AnimationEffect::after_active_boundary_time() const
{
    // max(min(start delay + active duration, end time), 0)
    return max(min(m_start_delay + active_duration(), end_time()), TimeValue::create_zero(associated_timeline()));
}

// https://www.w3.org/TR/web-animations-1/#animation-effect-before-phase
bool AnimationEffect::is_in_the_before_phase() const
{
    // An animation effect is in the before phase if the animation effect’s local time is not unresolved and either of
    // the following conditions are met:
    auto local_time = this->local_time();
    if (!local_time.has_value())
        return false;

    // - the local time is less than the before-active boundary time, or
    auto before_active_boundary_time = this->before_active_boundary_time();
    if (local_time.value() < before_active_boundary_time)
        return true;

    // - the animation direction is "backwards" and the local time is equal to the before-active boundary time.
    return animation_direction() == AnimationDirection::Backwards && local_time.value() == before_active_boundary_time;
}

// https://www.w3.org/TR/web-animations-1/#animation-effect-after-phase
bool AnimationEffect::is_in_the_after_phase() const
{
    // An animation effect is in the after phase if the animation effect’s local time is not unresolved and either of
    // the following conditions are met:
    auto local_time = this->local_time();
    if (!local_time.has_value())
        return false;

    // - the local time is greater than the active-after boundary time, or
    auto after_active_boundary_time = this->after_active_boundary_time();
    if (local_time.value() > after_active_boundary_time)
        return true;

    // - the animation direction is "forwards" and the local time is equal to the active-after boundary time.
    return animation_direction() == AnimationDirection::Forwards && local_time.value() == after_active_boundary_time;
}

// https://www.w3.org/TR/web-animations-1/#animation-effect-active-phase
bool AnimationEffect::is_in_the_active_phase() const
{
    // An animation effect is in the active phase if the animation effect’s local time is not unresolved and it is not
    // in either the before phase nor the after phase.
    return local_time().has_value() && !is_in_the_before_phase() && !is_in_the_after_phase();
}

// https://www.w3.org/TR/web-animations-1/#animation-effect-idle-phase
bool AnimationEffect::is_in_the_idle_phase() const
{
    // It is often convenient to refer to the case when an animation effect is in none of the above phases as being in
    // the idle phase
    return !is_in_the_before_phase() && !is_in_the_active_phase() && !is_in_the_after_phase();
}

AnimationEffect::Phase AnimationEffect::phase() const
{
    // This is a convenience method that returns the phase of the animation effect, to avoid having to call all of the
    // phase functions separately.
    auto local_time = this->local_time();
    if (!local_time.has_value())
        return Phase::Idle;

    auto before_active_boundary_time = this->before_active_boundary_time();
    // - the local time is less than the before-active boundary time, or
    // - the animation direction is "backwards" and the local time is equal to the before-active boundary time.
    if (local_time.value() < before_active_boundary_time || (animation_direction() == AnimationDirection::Backwards && local_time.value() == before_active_boundary_time))
        return Phase::Before;

    auto after_active_boundary_time = this->after_active_boundary_time();
    // - the local time is greater than the active-after boundary time, or
    // - the animation direction is "forwards" and the local time is equal to the active-after boundary time.
    if (local_time.value() > after_active_boundary_time || (animation_direction() == AnimationDirection::Forwards && local_time.value() == after_active_boundary_time))
        return Phase::After;

    // - An animation effect is in the active phase if the animation effect’s local time is not unresolved and it is not
    // - in either the before phase nor the after phase.
    return Phase::Active;
}

// https://www.w3.org/TR/web-animations-1/#overall-progress
Optional<double> AnimationEffect::overall_progress() const
{
    // 1. If the active time is unresolved, return unresolved.
    auto active_time = this->active_time();
    if (!active_time.has_value())
        return {};

    // 2. Calculate an initial value for overall progress based on the first matching condition from below,
    double overall_progress;

    // -> If the iteration duration is zero,
    if (m_iteration_duration.value == 0) {
        // If the animation effect is in the before phase, let overall progress be zero, otherwise, let it be equal to
        // the iteration count.
        if (is_in_the_before_phase())
            overall_progress = 0.0;
        else
            overall_progress = m_iteration_count;
    }
    // Otherwise,
    else {
        // Let overall progress be the result of calculating active time / iteration duration.
        overall_progress = active_time.value() / m_iteration_duration;
    }

    // 3. Return the result of calculating overall progress + iteration start.
    return overall_progress + m_iteration_start;
}

// https://www.w3.org/TR/web-animations-1/#directed-progress
Optional<double> AnimationEffect::directed_progress() const
{
    // 1. If the simple iteration progress is unresolved, return unresolved.
    auto simple_iteration_progress = this->simple_iteration_progress();
    if (!simple_iteration_progress.has_value())
        return {};

    // 2. Calculate the current direction using the first matching condition from the following list:
    auto current_direction = this->current_direction();

    // 3. If the current direction is forwards then return the simple iteration progress.
    if (current_direction == AnimationDirection::Forwards)
        return simple_iteration_progress;

    //    Otherwise, return 1.0 - simple iteration progress.
    return 1.0 - simple_iteration_progress.value();
}

// https://www.w3.org/TR/web-animations-1/#directed-progress
AnimationDirection AnimationEffect::current_direction() const
{
    // 2. Calculate the current direction using the first matching condition from the following list:
    // -> If playback direction is normal,
    if (m_playback_direction == Bindings::PlaybackDirection::Normal) {
        // Let the current direction be forwards.
        return AnimationDirection::Forwards;
    }

    // -> If playback direction is reverse,
    if (m_playback_direction == Bindings::PlaybackDirection::Reverse) {
        // Let the current direction be reverse.
        return AnimationDirection::Backwards;
    }
    // -> Otherwise,
    //    1. Let d be the current iteration.
    double d = current_iteration().value();

    //    2. If playback direction is alternate-reverse increment d by 1.
    if (m_playback_direction == Bindings::PlaybackDirection::AlternateReverse)
        d += 1.0;

    //    3. If d % 2 == 0, let the current direction be forwards, otherwise let the current direction be reverse. If d
    //       is infinity, let the current direction be forwards.
    if (isinf(d))
        return AnimationDirection::Forwards;
    if (fmod(d, 2.0) == 0.0)
        return AnimationDirection::Forwards;
    return AnimationDirection::Backwards;
}

// https://www.w3.org/TR/web-animations-1/#simple-iteration-progress
Optional<double> AnimationEffect::simple_iteration_progress() const
{
    // 1. If the overall progress is unresolved, return unresolved.
    auto overall_progress = this->overall_progress();
    if (!overall_progress.has_value())
        return {};

    // 2. If overall progress is infinity, let the simple iteration progress be iteration start % 1.0, otherwise, let
    //    the simple iteration progress be overall progress % 1.0.
    double simple_iteration_progress = isinf(overall_progress.value()) ? fmod(m_iteration_start, 1.0) : fmod(overall_progress.value(), 1.0);

    // 3. If all of the following conditions are true,
    //    - the simple iteration progress calculated above is zero, and
    //    - the animation effect is in the active phase or the after phase, and
    //    - the active time is equal to the active duration, and
    //    - the iteration count is not equal to zero.
    auto active_time = this->active_time();
    if (simple_iteration_progress == 0.0 && (is_in_the_active_phase() || is_in_the_after_phase()) && active_time.has_value() && active_time.value() == active_duration() && m_iteration_count != 0.0) {
        // let the simple iteration progress be 1.0.
        simple_iteration_progress = 1.0;
    }

    // 4. Return simple iteration progress.
    return simple_iteration_progress;
}

// https://www.w3.org/TR/web-animations-1/#current-iteration
Optional<double> AnimationEffect::current_iteration() const
{
    // 1. If the active time is unresolved, return unresolved.
    auto active_time = this->active_time();
    if (!active_time.has_value())
        return {};

    // 2. If the animation effect is in the after phase and the iteration count is infinity, return infinity.
    if (is_in_the_after_phase() && isinf(m_iteration_count))
        return m_iteration_count;

    // 3. If the simple iteration progress is 1.0, return floor(overall progress) - 1.
    auto simple_iteration_progress = this->simple_iteration_progress();
    if (simple_iteration_progress.has_value() && simple_iteration_progress.value() == 1.0)
        return floor(overall_progress().value()) - 1.0;

    // 4. Otherwise, return floor(overall progress).
    return floor(overall_progress().value());
}

// https://www.w3.org/TR/web-animations-1/#transformed-progress
Optional<double> AnimationEffect::transformed_progress() const
{
    // 1. If the directed progress is unresolved, return unresolved.
    auto directed_progress = this->directed_progress();
    if (!directed_progress.has_value())
        return {};

    // 2. Calculate the value of the before flag as follows:

    //    1. Determine the current direction using the procedure defined in §4.9.1 Calculating the directed progress.
    auto current_direction = this->current_direction();

    //    2. If the current direction is forwards, let going forwards be true, otherwise it is false.
    auto going_forwards = current_direction == AnimationDirection::Forwards;

    //    3. The before flag is set if the animation effect is in the before phase and going forwards is true; or if the animation effect
    //       is in the after phase and going forwards is false.
    auto before_flag = (is_in_the_before_phase() && going_forwards) || (is_in_the_after_phase() && !going_forwards);

    // 3. Return the result of evaluating the animation effect’s timing function passing directed progress as the input progress value and
    //    before flag as the before flag.
    return m_timing_function.evaluate_at(directed_progress.value(), before_flag);
}

Optional<CSS::EasingFunction> AnimationEffect::parse_easing_string(StringView value)
{
    if (auto style_value = parse_css_value(CSS::Parser::ParsingParams(), value, CSS::PropertyID::AnimationTimingFunction)) {
        if (style_value->is_unresolved() || style_value->is_css_wide_keyword())
            return {};

        auto easing_values = style_value->as_value_list().values();

        if (easing_values.size() != 1)
            return {};

        // FIXME: We should absolutize the style value to resolve relative lengths within calcs
        return CSS::EasingFunction::from_style_value(easing_values[0]);
    }

    return {};
}

AnimationEffect::AnimationEffect(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void AnimationEffect::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationEffect);
    Base::initialize(realm);
}

void AnimationEffect::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_animation);
}

static CSS::RequiredInvalidationAfterStyleChange compute_required_invalidation_for_animated_properties(HashMap<CSS::PropertyID, NonnullRefPtr<CSS::StyleValue const>> const& old_properties, HashMap<CSS::PropertyID, NonnullRefPtr<CSS::StyleValue const>> const& new_properties)
{
    CSS::RequiredInvalidationAfterStyleChange invalidation;
    auto old_and_new_properties = MUST(Bitmap::create(CSS::number_of_longhand_properties, 0));
    for (auto const& [property_id, _] : old_properties)
        old_and_new_properties.set(to_underlying(property_id) - to_underlying(CSS::first_longhand_property_id), 1);
    for (auto const& [property_id, _] : new_properties)
        old_and_new_properties.set(to_underlying(property_id) - to_underlying(CSS::first_longhand_property_id), 1);
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        if (!old_and_new_properties.get(i - to_underlying(CSS::first_longhand_property_id)))
            continue;
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto const* old_value = old_properties.get(property_id).value_or({});
        auto const* new_value = new_properties.get(property_id).value_or({});
        if (!old_value && !new_value)
            continue;
        invalidation |= compute_property_invalidation(property_id, old_value, new_value);
    }
    return invalidation;
}

AnimationUpdateContext::~AnimationUpdateContext()
{
    for (auto& it : elements) {
        auto style = it.value->target_style;
        if (!style)
            continue;
        auto& element = it.key;
        GC::Ref<DOM::Element> target = element.element();
        auto invalidation = compute_required_invalidation_for_animated_properties(it.value->animated_properties_before_update, style->animated_property_values());

        if (invalidation.is_none())
            continue;

        // Traversal of the subtree is necessary to update the animated properties inherited from the target element.
        target->for_each_in_subtree_of_type<DOM::Element>([&](auto& element) {
            auto element_invalidation = element.recompute_inherited_style();
            if (element_invalidation.is_none())
                return TraversalDecision::SkipChildrenAndContinue;
            invalidation |= element_invalidation;
            return TraversalDecision::Continue;
        });

        if (!element.pseudo_element().has_value()) {
            if (target->layout_node())
                target->layout_node()->apply_style(*style);
        } else {
            if (auto pseudo_element_node = target->get_pseudo_element_node(element.pseudo_element().value()))
                pseudo_element_node->apply_style(*style);
        }

        if (invalidation.relayout && target->layout_node())
            target->layout_node()->set_needs_layout_update(DOM::SetNeedsLayoutReason::KeyframeEffect);
        if (invalidation.rebuild_layout_tree) {
            // We mark layout tree for rebuild starting from parent element to correctly invalidate
            // "display" property change to/from "contents" value.
            if (auto parent_element = target->parent_element()) {
                parent_element->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::KeyframeEffect);
            } else {
                target->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::KeyframeEffect);
            }
        }
        if (invalidation.repaint) {
            if (target->paintable())
                target->paintable()->set_needs_paint_only_properties_update(true);

            if (invalidation.rebuild_accumulated_visual_contexts)
                element.document().set_needs_accumulated_visual_contexts_update(true);

            element.document().set_needs_display();
        }
        if (invalidation.rebuild_stacking_context_tree)
            element.document().invalidate_stacking_context_tree();
    }
}

}
