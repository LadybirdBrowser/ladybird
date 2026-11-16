/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The timing calculations of Web Animations, from the normalized timing of an animation effect and
//! the state of its animation and timeline.

use super::effect_timing::{
    AnimationDirection, AnimationEffectPhase, AnimationEffectTiming, FillMode, PlaybackDirection, SpecifiedEffectTiming,
};
use super::playback::{AnimationPlayState, AnimationPlayback, AnimationReplaceState, TimelineSample};
use super::time_value::{TimeValue, TimeValueType, max, min};
use crate::easing::Easing;

/// Two times of a progress-based timeline, in percent, closer than this are the same but for
/// floating point error.
pub const PROGRESS_TIMELINE_TOLERANCE: f64 = 1e-6;

/// Everything a progress calculation needs, resolved together from one local time.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ResolvedEffectTiming {
    pub phase: AnimationEffectPhase,
    pub local_time: Option<TimeValue>,
    pub active_time: Option<TimeValue>,
    pub overall_progress: Option<f64>,
}

/// https://drafts.csswg.org/web-animations-2/#intrinsic-iteration-duration
/// `time_unit` is the unit of the associated timeline's times, milliseconds without one.
#[must_use]
pub fn intrinsic_iteration_duration(
    timeline_duration: Option<TimeValue>,
    iteration_count: f64,
    start_delay: TimeValue,
    end_delay: TimeValue,
    time_unit: TimeValueType,
) -> TimeValue {
    // The intrinsic iteration duration is calculated from the first matching condition from below:

    // FIXME: If the animation effect is a group effect,
    //            Follow the procedure in § 2.10.3 The intrinsic iteration duration of a group effect

    // FIXME: If the animation effect is a sequence effect,
    //            Follow the procedure in § 2.10.4.2 The intrinsic iteration duration of a sequence effect

    match timeline_duration {
        // If timeline duration is unresolved or iteration count is zero,
        //     Return 0
        None => TimeValue::zero(time_unit),
        Some(_) if iteration_count == 0.0 => TimeValue::zero(time_unit),
        // Otherwise
        Some(timeline_duration) => {
            // Return (100% - start delay - end delay) / iteration count
            // Note: Presently start and end delays are zero until such time as percentage based delays are supported.
            // AD-HOC: An effect fills its animation's attachment range rather than the whole timeline, which is what the
            //         timeline duration is here. This matches WebKit.
            (timeline_duration - start_delay - end_delay) / iteration_count
        }
    }
}

/// https://drafts.csswg.org/web-animations-2/#time-based-animation-to-a-proportional-animation
/// `timeline_duration` is resolved, and in the unit of the associated timeline's times, `time_unit`.
#[must_use]
pub fn convert_a_time_based_animation_to_a_proportional_animation(
    specified: &SpecifiedEffectTiming,
    timeline_duration: TimeValue,
    time_unit: TimeValueType,
) -> AnimationEffectTiming {
    let mut timing = specified_timing_with(specified, TimeValue::zero(time_unit));

    // AD-HOC: We use the specified iteration duration instead of the iteration duration here, see
    //         https://github.com/w3c/csswg-drafts/pull/13170
    // If the iteration duration is auto, then perform the following steps.
    let Some(specified_iteration_duration) = specified.iteration_duration.time() else {
        // Set start delay and end delay to 0, as it is not possible to mix time and proportions.
        // Note: Future versions may allow these properties to be assigned percentages, at which point the delays are
        //       only to be ignored if their values are expressed as times and not as percentages.
        timing.start_delay = TimeValue::zero(time_unit);
        timing.end_delay = TimeValue::zero(time_unit);

        // AD-HOC: The spec doesn't say what to set iteration duration to in this case so we set it to the intrinsic
        //         iteration duration, see: https://github.com/w3c/csswg-drafts/issues/13220
        timing.iteration_duration = intrinsic_iteration_duration(
            Some(timeline_duration),
            specified.iteration_count,
            timing.start_delay,
            timing.end_delay,
            time_unit,
        );
        return timing;
    };

    // Otherwise:

    // 1. Let total time be equal to end time
    // AD-HOC: Using end time here only works if we haven't already converted to a proportional animation, we instead
    //         recompute the specified equivalent of "end time", see https://github.com/w3c/csswg-drafts/issues/13230
    // NB: As for the active duration, an iteration duration or iteration count of zero makes the specified active
    //     duration zero, where infinity times zero would be NaN.
    let specified_active_duration = if specified_iteration_duration == 0.0 || specified.iteration_count == 0.0 {
        0.0
    } else {
        specified_iteration_duration * specified.iteration_count
    };
    let total_time = max(
        specified.start_delay + specified_active_duration + specified.end_delay,
        0.0,
    );

    // AD-HOC: Avoid a division by zero below, see https://github.com/w3c/csswg-drafts/issues/11276
    if total_time == 0.0 {
        return timing;
    }

    // INTEROP: An infinite iteration duration or iteration count makes the total time infinite, where the steps below
    //          would divide infinity by infinity. Like other browsers, take their limit instead: the delays, which are
    //          finite, become 0, and the iteration duration becomes timeline duration / iteration count.
    if total_time.is_infinite() {
        timing.iteration_duration = timeline_duration / specified.iteration_count;
        return timing;
    }

    // 2. Set start delay to be the result of evaluating specified start delay / total time * timeline duration.
    timing.start_delay = timeline_duration * (specified.start_delay / total_time);

    // 3. Set iteration duration to be the result of evaluating specified iteration duration / total time * timeline duration.
    timing.iteration_duration = timeline_duration * (specified_iteration_duration / total_time);

    // 4. Set end delay to be the result of evaluating specified end delay / total time * timeline duration.
    timing.end_delay = timeline_duration * (specified.end_delay / total_time);

    timing
}

/// The timing properties `specified` names, with every time `zero`.
fn specified_timing_with(specified: &SpecifiedEffectTiming, zero: TimeValue) -> AnimationEffectTiming {
    AnimationEffectTiming {
        start_delay: zero,
        end_delay: zero,
        iteration_duration: zero,
        iteration_start: specified.iteration_start,
        iteration_count: specified.iteration_count,
        fill_mode: specified.fill_mode,
        playback_direction: specified.playback_direction,
    }
}

/// https://drafts.csswg.org/web-animations-2/#normalize-specified-timing
/// `timeline_duration` is that of the associated timeline, as an animation attached to a range of
/// it sees it, and `time_unit` the unit of the timeline's times, milliseconds without one.
#[must_use]
pub fn normalize_specified_timing(
    specified: &SpecifiedEffectTiming,
    timeline_duration: Option<TimeValue>,
    time_unit: TimeValueType,
) -> AnimationEffectTiming {
    // If timeline duration is resolved:
    if let Some(timeline_duration) = timeline_duration {
        // Follow the procedure to convert a time-based animation to a proportional animation
        return convert_a_time_based_animation_to_a_proportional_animation(specified, timeline_duration, time_unit);
    }

    // Otherwise:
    let mut timing = specified_timing_with(specified, TimeValue::zero(time_unit));

    // 1. Set start delay = specified start delay
    timing.start_delay = TimeValue::milliseconds(specified.start_delay);

    // 2. Set end delay = specified end delay
    timing.end_delay = TimeValue::milliseconds(specified.end_delay);

    // 3. If iteration duration is auto:
    // AD-HOC: We use the specified iteration duration instead of the iteration duration here, see
    //         https://github.com/w3c/csswg-drafts/pull/13170
    timing.iteration_duration = match specified.iteration_duration.time() {
        // Set iteration duration = intrinsic iteration duration
        None => intrinsic_iteration_duration(
            timeline_duration,
            specified.iteration_count,
            timing.start_delay,
            timing.end_delay,
            time_unit,
        ),
        // Otherwise:
        //     Set iteration duration = specified iteration duration
        Some(specified_iteration_duration) => TimeValue::milliseconds(specified_iteration_duration),
    };

    timing
}

/// https://www.w3.org/TR/web-animations-1/#animation-direction
#[inline]
#[must_use]
pub fn animation_direction(animation: Option<&AnimationPlayback>) -> AnimationDirection {
    // “backwards” if the effect is associated with an animation and the associated animation’s playback rate is less
    // than zero; in all other cases, the animation direction is “forward”.
    match animation {
        Some(animation) if animation.playback_rate < 0.0 => AnimationDirection::Backwards,
        _ => AnimationDirection::Forwards,
    }
}

/// https://www.w3.org/TR/web-animations-1/#active-duration
#[inline]
#[must_use]
pub fn active_duration(timing: &AnimationEffectTiming) -> TimeValue {
    // The length of the active interval is called the active duration, and is determined by the iteration duration and
    // iteration count as defined below.
    //     active duration = iteration duration × iteration count
    // If either the iteration duration or iteration count are zero, the active duration is zero.
    // Note: This clarification is needed since the result of infinity multiplied by zero is undefined according to
    //       IEEE 754-2008.
    if timing.iteration_duration.value == 0.0 || timing.iteration_count == 0.0 {
        return timing.zero();
    }
    timing.iteration_duration * timing.iteration_count
}

/// https://www.w3.org/TR/web-animations-1/#end-time
#[inline]
#[must_use]
pub fn end_time(timing: &AnimationEffectTiming) -> TimeValue {
    // The end time, of an animation effect is the result of evaluating
    // max(start delay + active duration + end delay, 0).
    max(
        timing.start_delay + active_duration(timing) + timing.end_delay,
        timing.zero(),
    )
}

/// https://www.w3.org/TR/web-animations-1/#before-active-boundary-time
#[inline]
#[must_use]
pub fn before_active_boundary_time(timing: &AnimationEffectTiming) -> TimeValue {
    // max(min(start delay, end time), 0)
    max(min(timing.start_delay, end_time(timing)), timing.zero())
}

/// https://www.w3.org/TR/web-animations-1/#active-after-boundary-time
#[inline]
#[must_use]
pub fn after_active_boundary_time(timing: &AnimationEffectTiming) -> TimeValue {
    // max(min(start delay + active duration, end time), 0)
    max(
        min(timing.start_delay + active_duration(timing), end_time(timing)),
        timing.zero(),
    )
}

/// https://drafts.csswg.org/web-animations-2/#at-timeline-boundary
/// `timeline` is the associated animation's timeline.
#[inline]
#[must_use]
pub fn is_at_timeline_boundary(timeline: Option<&TimelineSample>) -> bool {
    let Some(timeline) = timeline else {
        return false;
    };
    let Some(timeline_time) = timeline.current_time.get() else {
        return false;
    };
    let matches = |time: Option<TimeValue>| {
        time.is_some_and(|time| (timeline_time.value - time.value).abs() < PROGRESS_TIMELINE_TOLERANCE)
    };

    // Determined based on the first matching condition:
    // If the minimum timeline time matches the timeline current time: Return true
    // If the maximum timeline time matches the timeline current time: Return true
    // Otherwise: Return false
    matches(timeline.minimum_timeline_time.get()) || matches(timeline.maximum_timeline_time.get())
}

/// https://drafts.csswg.org/web-animations-1/#animation-effect-phases-and-states
/// Furthermore, an endpoint-inclusive active interval flag may be specified when determining the phase. If not
/// specified, it is assumed to be false.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum EndpointInclusiveActiveInterval {
    #[default]
    No,
    Yes,
}

/// https://drafts.csswg.org/web-animations-2/#animation-effect-phases-and-states
/// The phase of an animation effect at `local_time`, Idle where it is unresolved.
#[must_use]
pub fn phase(
    timing: &AnimationEffectTiming,
    local_time: Option<TimeValue>,
    animation_direction: AnimationDirection,
    at_timeline_boundary: bool,
    endpoint_inclusive: EndpointInclusiveActiveInterval,
) -> AnimationEffectPhase {
    let Some(local_time) = local_time else {
        return AnimationEffectPhase::Idle;
    };
    let endpoint_exclusive = endpoint_inclusive == EndpointInclusiveActiveInterval::No;

    let before_active_boundary_time = before_active_boundary_time(timing);
    // An animation effect is in the before phase if the animation effect’s local time is not unresolved and either of
    // the following conditions are met:
    // 1. the local time is less than the before-active boundary time, or
    // 2. the animation direction is "backwards" and the local time is equal to the before-active boundary time and
    //    not at timeline boundary.
    if local_time < before_active_boundary_time
        || (animation_direction == AnimationDirection::Backwards
            && local_time == before_active_boundary_time
            && !at_timeline_boundary
            && endpoint_exclusive)
    {
        return AnimationEffectPhase::Before;
    }

    let after_active_boundary_time = after_active_boundary_time(timing);
    // An animation effect is in the after phase if the animation effect’s local time is not unresolved and either of
    // the following conditions are met:
    // 1. the local time is greater than the active-after boundary time, or
    // 2. the animation direction is "forwards" and the local time is equal to the active-after boundary time and not
    //    at timeline boundary.
    if local_time > after_active_boundary_time
        || (animation_direction == AnimationDirection::Forwards
            && local_time == after_active_boundary_time
            && !at_timeline_boundary
            && endpoint_exclusive)
    {
        return AnimationEffectPhase::After;
    }

    // https://www.w3.org/TR/web-animations-1/#animation-effect-active-phase
    // An animation effect is in the active phase if the animation effect’s local time is not unresolved and it is
    // neither in the before phase nor in the after phase.
    AnimationEffectPhase::Active
}

/// https://www.w3.org/TR/web-animations-1/#calculating-the-active-time
#[must_use]
pub fn active_time(
    timing: &AnimationEffectTiming,
    phase: AnimationEffectPhase,
    local_time: Option<TimeValue>,
    fill_mode: FillMode,
) -> Option<TimeValue> {
    // The active time is based on the local time and start delay. However, it is only defined when the animation effect
    // should produce an output and hence depends on its fill mode and phase as follows,
    let local_time = || local_time.expect("an effect in a phase has a resolved local time");

    match phase {
        // -> If the animation effect is in the before phase,
        AnimationEffectPhase::Before => {
            // The result depends on the first matching condition from the following,

            // -> If the fill mode is backwards or both,
            if matches!(fill_mode, FillMode::Backwards | FillMode::Both) {
                // Return the result of evaluating max(local time - start delay, 0).
                return Some(max(local_time() - timing.start_delay, timing.zero()));
            }

            // -> Otherwise,
            //    Return an unresolved time value.
            None
        }

        // -> If the animation effect is in the active phase,
        AnimationEffectPhase::Active => {
            // Return the result of evaluating local time - start delay.
            Some(local_time() - timing.start_delay)
        }

        // -> If the animation effect is in the after phase,
        AnimationEffectPhase::After => {
            // The result depends on the first matching condition from the following,

            // -> If the fill mode is forwards or both,
            if matches!(fill_mode, FillMode::Forwards | FillMode::Both) {
                // Return the result of evaluating max(min(local time - start delay, active duration), 0).
                return Some(max(
                    min(local_time() - timing.start_delay, active_duration(timing)),
                    timing.zero(),
                ));
            }

            // -> Otherwise,
            //    Return an unresolved time value.
            None
        }

        // -> Otherwise (the local time is unresolved),
        //    Return an unresolved time value.
        AnimationEffectPhase::Idle => None,
    }
}

/// https://www.w3.org/TR/web-animations-1/#overall-progress
#[must_use]
pub fn overall_progress(
    timing: &AnimationEffectTiming,
    phase: AnimationEffectPhase,
    active_time: Option<TimeValue>,
) -> Option<f64> {
    // 1. If the active time is unresolved, return unresolved.
    let active_time = active_time?;

    // 2. Calculate an initial value for overall progress based on the first matching condition below:
    // -> If the iteration duration is zero,
    let overall_progress = if timing.iteration_duration.value == 0.0 {
        // If the animation effect is in the before phase, let overall progress be zero, otherwise, let it be equal to
        // the iteration count.
        match phase {
            AnimationEffectPhase::Before => 0.0,
            _ => timing.iteration_count,
        }
    }
    // Otherwise,
    else {
        // Let overall progress be the result of calculating active time / iteration duration.
        active_time / timing.iteration_duration
    };

    // 3. Return the result of calculating overall progress + iteration start.
    Some(overall_progress + timing.iteration_start)
}

/// The phase, active time and overall progress of an effect at `local_time`, with the effect's fill
/// mode. `animation` is the associated animation, if any, and `timeline` its timeline.
#[must_use]
pub fn resolve_timing(
    timing: &AnimationEffectTiming,
    local_time: Option<TimeValue>,
    animation: Option<&AnimationPlayback>,
    timeline: Option<&TimelineSample>,
) -> ResolvedEffectTiming {
    resolve_timing_using_fill(timing, local_time, animation, timeline, timing.fill_mode)
}

/// `resolve_timing()` with the fill mode `fill_mode` in place of the effect's own.
#[must_use]
pub fn resolve_timing_using_fill(
    timing: &AnimationEffectTiming,
    local_time: Option<TimeValue>,
    animation: Option<&AnimationPlayback>,
    timeline: Option<&TimelineSample>,
    fill_mode: FillMode,
) -> ResolvedEffectTiming {
    resolve_timing_with(
        timing,
        local_time,
        animation,
        timeline,
        fill_mode,
        EndpointInclusiveActiveInterval::No,
    )
}

/// `resolve_timing()` with the fill mode `fill_mode` in place of the effect's own, determining the phase with the
/// endpoint-inclusive active interval flag `endpoint_inclusive`.
#[must_use]
pub fn resolve_timing_with(
    timing: &AnimationEffectTiming,
    local_time: Option<TimeValue>,
    animation: Option<&AnimationPlayback>,
    timeline: Option<&TimelineSample>,
    fill_mode: FillMode,
    endpoint_inclusive: EndpointInclusiveActiveInterval,
) -> ResolvedEffectTiming {
    let at_timeline_boundary = local_time.is_some() && is_at_timeline_boundary(timeline);
    let phase = phase(
        timing,
        local_time,
        animation_direction(animation),
        at_timeline_boundary,
        endpoint_inclusive,
    );
    let active_time = active_time(timing, phase, local_time, fill_mode);
    ResolvedEffectTiming {
        phase,
        local_time,
        active_time,
        overall_progress: overall_progress(timing, phase, active_time),
    }
}

/// https://www.w3.org/TR/web-animations-1/#simple-iteration-progress
#[must_use]
pub fn simple_iteration_progress(timing: &AnimationEffectTiming, resolved: &ResolvedEffectTiming) -> Option<f64> {
    // 1. If the overall progress is unresolved, return unresolved.
    let overall_progress = resolved.overall_progress?;

    // 2. If overall progress is infinity, let the simple iteration progress be iteration start % 1.0, otherwise, let
    //    the simple iteration progress be overall progress % 1.0.
    let mut simple_iteration_progress = match overall_progress.is_infinite() {
        true => timing.iteration_start % 1.0,
        false => overall_progress % 1.0,
    };

    // 3. If all of the following conditions are true,
    //    - the simple iteration progress calculated above is zero, and
    //    - the animation effect is in the active phase or the after phase, and
    //    - the active time is equal to the active duration, and
    //    - the iteration count is not equal to zero.
    if simple_iteration_progress == 0.0
        && matches!(
            resolved.phase,
            AnimationEffectPhase::Active | AnimationEffectPhase::After
        )
        && resolved.active_time == Some(active_duration(timing))
        && timing.iteration_count != 0.0
    {
        // let the simple iteration progress be 1.0.
        simple_iteration_progress = 1.0;
    }

    // 4. Return simple iteration progress.
    Some(simple_iteration_progress)
}

/// https://drafts.csswg.org/web-animations-1/#current-iteration-index
#[must_use]
pub fn current_iteration(timing: &AnimationEffectTiming, resolved: &ResolvedEffectTiming) -> Option<f64> {
    current_iteration_with(timing, resolved, || simple_iteration_progress(timing, resolved))
}

/// `current_iteration()` with the simple iteration progress given by `simple_iteration_progress`.
fn current_iteration_with(
    timing: &AnimationEffectTiming,
    resolved: &ResolvedEffectTiming,
    simple_iteration_progress: impl FnOnce() -> Option<f64>,
) -> Option<f64> {
    // 1. If the active time is unresolved, return unresolved.
    resolved.active_time?;

    // 2. If the animation effect is in the after phase and the iteration count is infinity, return infinity.
    if resolved.phase == AnimationEffectPhase::After && timing.iteration_count.is_infinite() {
        return Some(timing.iteration_count);
    }

    let overall_progress = resolved
        .overall_progress
        .expect("a resolved active time has a resolved overall progress");

    // 3. If the simple iteration progress is 1.0, return floor(overall progress) - 1.
    if simple_iteration_progress() == Some(1.0) {
        return Some(overall_progress.floor() - 1.0);
    }

    // 4. Otherwise, return floor(overall progress).
    Some(overall_progress.floor())
}

/// https://www.w3.org/TR/web-animations-1/#directed-progress
/// The current direction, which step 2 of calculating the directed progress determines.
#[must_use]
pub fn current_direction(timing: &AnimationEffectTiming, resolved: &ResolvedEffectTiming) -> AnimationDirection {
    current_direction_with(timing, resolved, || simple_iteration_progress(timing, resolved))
}

/// `current_direction()` with the simple iteration progress given by `simple_iteration_progress`.
fn current_direction_with(
    timing: &AnimationEffectTiming,
    resolved: &ResolvedEffectTiming,
    simple_iteration_progress: impl FnOnce() -> Option<f64>,
) -> AnimationDirection {
    // 2. Calculate the current direction using the first matching condition from the following list:
    match timing.playback_direction {
        // -> If playback direction is normal,
        //    Let the current direction be forwards.
        PlaybackDirection::Normal => AnimationDirection::Forwards,

        // -> If playback direction is reverse,
        //    Let the current direction be reverse.
        PlaybackDirection::Reverse => AnimationDirection::Backwards,

        // -> Otherwise,
        PlaybackDirection::Alternate | PlaybackDirection::AlternateReverse => {
            //    1. Let d be the current iteration index.
            let mut d = current_iteration_with(timing, resolved, simple_iteration_progress)
                .expect("an effect with a resolved simple iteration progress has a current iteration");

            //    2. If playback direction is alternate-reverse increment d by 1.
            if timing.playback_direction == PlaybackDirection::AlternateReverse {
                d += 1.0;
            }

            //    3. If d % 2 == 0, let the current direction be forwards, otherwise let the current direction be
            //       reverse. If d is infinity, let the current direction be forwards.
            if d.is_infinite() || d % 2.0 == 0.0 {
                AnimationDirection::Forwards
            } else {
                AnimationDirection::Backwards
            }
        }
    }
}

/// https://www.w3.org/TR/web-animations-1/#directed-progress
#[must_use]
pub fn directed_progress(timing: &AnimationEffectTiming, resolved: &ResolvedEffectTiming) -> Option<f64> {
    directed_progress_and_current_direction(timing, resolved).map(|(directed_progress, _)| directed_progress)
}

/// https://www.w3.org/TR/web-animations-1/#directed-progress
/// The directed progress, and the current direction it was calculated with.
fn directed_progress_and_current_direction(
    timing: &AnimationEffectTiming,
    resolved: &ResolvedEffectTiming,
) -> Option<(f64, AnimationDirection)> {
    // 1. If the simple iteration progress is unresolved, return unresolved.
    let simple_iteration_progress = simple_iteration_progress(timing, resolved)?;

    // 2. Calculate the current direction using the first matching condition from the following list:
    let current_direction = current_direction_with(timing, resolved, || Some(simple_iteration_progress));

    // 3. If the current direction is forwards then return the simple iteration progress.
    //    Otherwise, return 1.0 - simple iteration progress.
    let directed_progress = match current_direction {
        AnimationDirection::Forwards => simple_iteration_progress,
        AnimationDirection::Backwards => 1.0 - simple_iteration_progress,
    };
    Some((directed_progress, current_direction))
}

/// https://www.w3.org/TR/web-animations-1/#transformed-progress
#[inline]
#[must_use]
pub fn transformed_progress(
    timing: &AnimationEffectTiming,
    resolved: &ResolvedEffectTiming,
    timing_function: &Easing,
) -> Option<f64> {
    transformed_progress_with(timing, resolved, |input_progress, before_flag| {
        timing_function.evaluate_at(input_progress, before_flag)
    })
}

/// `transformed_progress()` with the timing function evaluated by `evaluate_timing_function`, which
/// takes the input progress value and the before flag.
#[must_use]
pub fn transformed_progress_with(
    timing: &AnimationEffectTiming,
    resolved: &ResolvedEffectTiming,
    evaluate_timing_function: impl FnOnce(f64, bool) -> f64,
) -> Option<f64> {
    // 1. If the directed progress is unresolved, return unresolved.
    let (directed_progress, current_direction) = directed_progress_and_current_direction(timing, resolved)?;

    // 2. Calculate the value of the before flag as follows:

    //    1. Determine the current direction using the procedure defined in Calculating the directed progress.
    // NB: Calculating the directed progress in step 1 determined it.

    //    2. If the current direction is forwards, let going forwards be true, otherwise it is false.
    let going_forwards = current_direction == AnimationDirection::Forwards;

    //    3. The before flag is set if the animation effect is in the before phase and going forwards is true; or if the
    //       animation effect is in the after phase and going forwards is false.
    let before_flag = (resolved.phase == AnimationEffectPhase::Before && going_forwards)
        || (resolved.phase == AnimationEffectPhase::After && !going_forwards);

    // 3. Return the result of evaluating the effect easing function passing directed progress as the input progress
    //    value and before flag as the before flag.
    Some(evaluate_timing_function(directed_progress, before_flag))
}

/// https://www.w3.org/TR/web-animations-1/#in-play
#[inline]
#[must_use]
pub fn is_in_play(phase: AnimationEffectPhase, animation: Option<&AnimationPlayback>) -> bool {
    // An animation effect is in play if all of the following conditions are met:
    // - the animation effect is in the active phase, and
    // - the animation effect is associated with an animation that is not finished.
    phase == AnimationEffectPhase::Active && animation.is_some_and(|animation| !animation.is_finished)
}

/// https://www.w3.org/TR/web-animations-1/#current
/// `animation` is the associated animation, if any, and `timeline` its timeline.
#[must_use]
pub fn is_current(
    timing: &AnimationEffectTiming,
    phase: AnimationEffectPhase,
    animation: Option<&AnimationPlayback>,
    timeline: Option<&TimelineSample>,
) -> bool {
    // An animation effect is current if any of the following conditions are true:

    // - the animation effect is in play, or
    if is_in_play(phase, animation) {
        return true;
    }

    let Some(animation) = animation else {
        return false;
    };
    let playback_rate = animation.playback_rate;

    // - the animation effect is in the before phase and is associated with an animation with a playback rate > 0, or
    if playback_rate > 0.0 && phase == AnimationEffectPhase::Before {
        return true;
    }

    // - the animation effect is in the after phase and is associated with an animation with a playback rate < 0, or
    if playback_rate < 0.0 && phase == AnimationEffectPhase::After {
        return true;
    }

    // - the animation effect is associated with an animation not in the idle play state with a non-null associated
    //   timeline that is not monotonically increasing.
    let current_time = current_time_at(animation, timeline.and_then(|timeline| timeline.current_time.get()));
    play_state_at(animation, current_time, end_time(timing)) != AnimationPlayState::Idle
        && timeline.is_some_and(|timeline| !timeline.is_monotonically_increasing)
}

/// https://www.w3.org/TR/web-animations-1/#in-effect
#[inline]
#[must_use]
pub fn is_in_effect(resolved: &ResolvedEffectTiming) -> bool {
    // An animation effect is in effect if its active time, as calculated according to the procedure in Calculating the
    // active time, is not unresolved.
    resolved.active_time.is_some()
}

/// https://www.w3.org/TR/web-animations-1/#animation-current-time
/// The current time of `animation` with its timeline at `timeline_time`, which is unresolved where
/// the animation has no timeline.
#[inline]
#[must_use]
pub fn current_time_at(animation: &AnimationPlayback, timeline_time: Option<TimeValue>) -> Option<TimeValue> {
    // The current time is calculated from the first matching condition below:

    // -> If the animation’s hold time is resolved,
    if let Some(hold_time) = animation.hold_time.get() {
        // The current time is the animation’s hold time.
        return Some(hold_time);
    }

    // -> If any of the following are true:
    //    - the animation has no associated timeline, or
    //    - the associated timeline is inactive, or
    //    - the animation’s start time is unresolved.
    //    The current time is an unresolved time value.
    let (timeline_time, start_time) = (timeline_time?, animation.start_time.get()?);

    // -> Otherwise,
    //    current time = (timeline time - start time) × playback rate
    //    Where timeline time is the current time value of the associated timeline. The playback rate value is defined
    //    in Speed control.
    Some((timeline_time - start_time) * animation.playback_rate)
}

/// https://www.w3.org/TR/web-animations-1/#play-states
/// The play state of `animation` at its current time `current_time`, with the end time of its
/// associated effect, or zero without one, `associated_effect_end`.
#[must_use]
pub fn play_state_at(
    animation: &AnimationPlayback,
    current_time: Option<TimeValue>,
    associated_effect_end: TimeValue,
) -> AnimationPlayState {
    // The play state of animation, animation, at a given moment is the state corresponding to the first matching
    // condition from the following:

    // -> All of the following conditions are true:
    //    - The current time of animation is unresolved, and
    //    - the start time of animation is unresolved, and
    //    - animation does not have either a pending play task or a pending pause task,
    if current_time.is_none() && animation.start_time.get().is_none() && !animation.pending() {
        // → idle
        return AnimationPlayState::Idle;
    }

    // -> Either of the following conditions are true:
    //    - animation has a pending pause task, or
    //    - both the start time of animation is unresolved and it does not have a pending play task,
    if animation.has_pending_pause_task || (animation.start_time.get().is_none() && !animation.has_pending_play_task) {
        // → paused
        return AnimationPlayState::Paused;
    }

    // -> For animation, current time is resolved and either of the following conditions are true:
    //    - animation’s effective playback rate > 0 and current time ≥ associated effect end; or
    //    - animation’s effective playback rate < 0 and current time ≤ 0,
    let effective_playback_rate = animation.effective_playback_rate();
    if let Some(current_time) = current_time
        && ((effective_playback_rate > 0.0 && current_time >= associated_effect_end)
            || (effective_playback_rate < 0.0 && current_time.value <= 0.0))
    {
        // → finished
        return AnimationPlayState::Finished;
    }

    // -> Otherwise,
    //    → running
    AnimationPlayState::Running
}

/// https://www.w3.org/TR/web-animations-1/#animation-relevant
/// Whether `animation`, on `timeline`, is relevant, with its associated effect, if any, of
/// `effect_timing` at the local time `local_time`.
#[must_use]
pub fn is_relevant(
    animation: &AnimationPlayback,
    timeline: Option<&TimelineSample>,
    effect_timing: Option<&AnimationEffectTiming>,
    local_time: Option<TimeValue>,
) -> bool {
    // An animation is relevant if:
    // - Its associated effect is current or in effect, and
    // - Its replace state is not removed.
    effect_timing.is_some_and(|timing| {
        let resolved = resolve_timing(timing, local_time, Some(animation), timeline);
        is_current(timing, resolved.phase, Some(animation), timeline) || is_in_effect(&resolved)
    }) && animation.replace_state != AnimationReplaceState::Removed
}
