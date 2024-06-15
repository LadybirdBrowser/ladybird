/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibWeb/Animations/Animation.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Bindings/AnimationEffectPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Animations {

JS_DEFINE_ALLOCATOR(AnimationEffect);

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
        .duration = duration,
        .direction = direction,
        .easing = easing,
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-gettiming
EffectTiming AnimationEffect::get_timing() const
{
    // 1. Returns the specified timing properties for this animation effect.
    return {
        .delay = m_start_delay,
        .end_delay = m_end_delay,
        .fill = m_fill_mode,
        .iteration_start = m_iteration_start,
        .iterations = m_iteration_count,
        .duration = m_iteration_duration,
        .direction = m_playback_direction,
        .easing = m_timing_function.to_string(),
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-getcomputedtiming
ComputedEffectTiming AnimationEffect::get_computed_timing() const
{
    // 1. Returns the calculated timing properties for this animation effect.

    // Note: Although some of the attributes of the object returned by getTiming() and getComputedTiming() are common,
    //       their values may differ in the following ways:

    //     - duration: while getTiming() may return the string auto, getComputedTiming() must return a number
    //       corresponding to the calculated value of the iteration duration as defined in the description of the
    //       duration member of the EffectTiming interface.
    //
    //       In this level of the specification, that simply means that an auto value is replaced by zero.
    auto duration = m_iteration_duration.has<String>() ? 0.0 : m_iteration_duration.get<double>();

    //     - fill: likewise, while getTiming() may return the string auto, getComputedTiming() must return the specific
    //       FillMode used for timing calculations as defined in the description of the fill member of the EffectTiming
    //       interface.
    //
    //       In this level of the specification, that simply means that an auto value is replaced by the none FillMode.
    auto fill = m_fill_mode == Bindings::FillMode::Auto ? Bindings::FillMode::None : m_fill_mode;

    return {
        {
            .delay = m_start_delay,
            .end_delay = m_end_delay,
            .fill = fill,
            .iteration_start = m_iteration_start,
            .iterations = m_iteration_count,
            .duration = duration,
            .direction = m_playback_direction,
            .easing = m_timing_function.to_string(),
        },

        end_time(),
        active_duration(),
        local_time(),
        transformed_progress(),
        current_iteration(),
    };
}

// https://www.w3.org/TR/web-animations-1/#dom-animationeffect-updatetiming
// https://www.w3.org/TR/web-animations-1/#update-the-timing-properties-of-an-animation-effect
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
    RefPtr<CSS::StyleValue const> easing_value;
    if (timing.easing.has_value()) {
        easing_value = parse_easing_string(realm(), timing.easing.value());
        if (!easing_value)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid easing function"sv };
        VERIFY(easing_value->is_easing());
    }

    // 5. Assign each member that exists in input to the corresponding timing property of effect as follows:

    //    - delay → start delay
    if (timing.delay.has_value())
        m_start_delay = timing.delay.value();

    //    - endDelay → end delay
    if (timing.end_delay.has_value())
        m_end_delay = timing.end_delay.value();

    //    - fill → fill mode
    if (timing.fill.has_value())
        m_fill_mode = timing.fill.value();

    //    - iterationStart → iteration start
    if (timing.iteration_start.has_value())
        m_iteration_start = timing.iteration_start.value();

    //    - iterations → iteration count
    if (timing.iterations.has_value())
        m_iteration_count = timing.iterations.value();

    //    - duration → iteration duration
    if (timing.duration.has_value())
        m_iteration_duration = timing.duration.value();

    //    - direction → playback direction
    if (timing.direction.has_value())
        m_playback_direction = timing.direction.value();

    //    - easing → timing function
    if (easing_value)
        m_timing_function = easing_value->as_easing().function();

    if (auto animation = m_associated_animation)
        animation->effect_timing_changed({});

    return {};
}

void AnimationEffect::set_associated_animation(JS::GCPtr<Animation> value)
{
    m_associated_animation = value;
    if (auto* target = this->target())
        target->invalidate_style();
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
double AnimationEffect::end_time() const
{
    // 1. The end time of an animation effect is the result of evaluating
    //    max(start delay + active duration + end delay, 0).
    return max(m_start_delay + active_duration() + m_end_delay, 0.0);
}

// https://www.w3.org/TR/web-animations-1/#local-time
Optional<double> AnimationEffect::local_time() const
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
double AnimationEffect::active_duration() const
{
    // The active duration is calculated as follows:
    //     active duration = iteration duration × iteration count
    // If either the iteration duration or iteration count are zero, the active duration is zero. This clarification is
    // needed since the result of infinity multiplied by zero is undefined according to IEEE 754-2008.
    if (m_iteration_duration.has<String>() || m_iteration_duration.get<double>() == 0.0 || m_iteration_count == 0.0)
        return 0.0;

    return m_iteration_duration.get<double>() * m_iteration_count;
}

Optional<double> AnimationEffect::active_time() const
{
    return active_time_using_fill(m_fill_mode);
}

// https://www.w3.org/TR/web-animations-1/#calculating-the-active-time
Optional<double> AnimationEffect::active_time_using_fill(Bindings::FillMode fill_mode) const
{
    // The active time is based on the local time and start delay. However, it is only defined when the animation effect
    // should produce an output and hence depends on its fill mode and phase as follows,

    // -> If the animation effect is in the before phase,
    if (is_in_the_before_phase()) {
        // The result depends on the first matching condition from the following,

        // -> If the fill mode is backwards or both,
        if (fill_mode == Bindings::FillMode::Backwards || fill_mode == Bindings::FillMode::Both) {
            // Return the result of evaluating max(local time - start delay, 0).
            return max(local_time().value() - m_start_delay, 0.0);
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
            return max(min(local_time().value() - m_start_delay, active_duration()), 0.0);
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
double AnimationEffect::before_active_boundary_time() const
{
    // max(min(start delay, end time), 0)
    return max(min(m_start_delay, end_time()), 0.0);
}

// https://www.w3.org/TR/web-animations-1/#active-after-boundary-time
double AnimationEffect::after_active_boundary_time() const
{
    // max(min(start delay + active duration, end time), 0)
    return max(min(m_start_delay + active_duration(), end_time()), 0.0);
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
    // FIXME: There is a lot of duplicated condition checking here which can probably be inlined into this function

    if (is_in_the_before_phase())
        return Phase::Before;
    if (is_in_the_active_phase())
        return Phase::Active;
    if (is_in_the_after_phase())
        return Phase::After;
    return Phase::Idle;
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
    if (m_iteration_duration.has<String>() || m_iteration_duration.get<double>() == 0.0) {
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
        overall_progress = active_time.value() / m_iteration_duration.get<double>();
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

RefPtr<CSS::StyleValue const> AnimationEffect::parse_easing_string(JS::Realm& realm, StringView value)
{
    auto maybe_parser = CSS::Parser::Parser::create(CSS::Parser::ParsingContext(realm), value);
    if (maybe_parser.is_error())
        return {};

    if (auto style_value = maybe_parser.release_value().parse_as_css_value(CSS::PropertyID::AnimationTimingFunction)) {
        if (style_value->is_easing())
            return style_value;
    }

    return {};
}

AnimationEffect::AnimationEffect(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void AnimationEffect::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AnimationEffect);
}

void AnimationEffect::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_animation);
}

}
