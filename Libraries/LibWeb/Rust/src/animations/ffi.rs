/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the C++ animation classes reach the timing model, until they move to Rust.

use super::timing::{
    self, AnimationEffectPhase, AnimationEffectTiming, AnimationPlayState, AnimationPlayback, OptionalNumber,
    OptionalTimeValue, SpecifiedEffectTiming, TimeValue, TimeValueType, TimelineSample,
};
use libcompositing_rust::easing::{FfiEasingDescriptor, evaluate_easing_descriptor};

/// The timing of an effect specified as `specified`, normalized against its associated timeline, whose
/// duration, as the effect's animation sees it, is `timeline_duration`, and whose times are in `time_unit`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_normalize_specified_timing(
    specified: &SpecifiedEffectTiming,
    timeline_duration: OptionalTimeValue,
    time_unit: TimeValueType,
) -> AnimationEffectTiming {
    timing::normalize_specified_timing(specified, timeline_duration.get(), time_unit)
}

/// The timing of an effect at one local time, and what follows from its timing alone.
#[repr(C)]
pub struct FfiResolvedEffectTiming {
    pub phase: AnimationEffectPhase,
    pub local_time: OptionalTimeValue,
    pub active_time: OptionalTimeValue,
    pub overall_progress: OptionalNumber,
    pub current_iteration: OptionalNumber,
    /// Unresolved where no timing function was given.
    pub transformed_progress: OptionalNumber,
    pub is_current: bool,
    pub is_in_effect: bool,
    pub active_duration: TimeValue,
    pub end_time: TimeValue,
    pub before_active_boundary_time: TimeValue,
    pub after_active_boundary_time: TimeValue,
}

/// The timing of an effect of `timing` at `local_time`, associated with `animation`, if any, which runs on
/// `timeline`, if any, and with its transformed progress through `timing_function`, if given, with the
/// endpoint-inclusive active interval flag set where `endpoint_inclusive` is.
///
/// # Safety
/// The control points a linear `timing_function` names must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_resolve_effect_timing(
    timing: &AnimationEffectTiming,
    local_time: OptionalTimeValue,
    animation: Option<&AnimationPlayback>,
    timeline: Option<&TimelineSample>,
    timing_function: Option<&FfiEasingDescriptor>,
    endpoint_inclusive: bool,
) -> FfiResolvedEffectTiming {
    let endpoint_inclusive = if endpoint_inclusive {
        timing::EndpointInclusiveActiveInterval::Yes
    } else {
        timing::EndpointInclusiveActiveInterval::No
    };
    let resolved = timing::resolve_timing_with(
        timing,
        local_time.get(),
        animation,
        timeline,
        timing.fill_mode,
        endpoint_inclusive,
    );
    let transformed_progress = timing_function.and_then(|timing_function| {
        timing::transformed_progress_with(timing, &resolved, |input_progress, before_flag| {
            evaluate_easing_descriptor(timing_function, input_progress, before_flag)
        })
    });
    FfiResolvedEffectTiming {
        phase: resolved.phase,
        local_time: resolved.local_time.into(),
        active_time: resolved.active_time.into(),
        overall_progress: resolved.overall_progress.into(),
        current_iteration: timing::current_iteration(timing, &resolved).into(),
        transformed_progress: transformed_progress.into(),
        is_current: timing::is_current(timing, resolved.phase, animation, timeline),
        is_in_effect: timing::is_in_effect(&resolved),
        active_duration: timing::active_duration(timing),
        end_time: timing::end_time(timing),
        before_active_boundary_time: timing::before_active_boundary_time(timing),
        after_active_boundary_time: timing::after_active_boundary_time(timing),
    }
}

/// The current time of `animation` with its timeline at `timeline_time`, which is unresolved where the
/// animation has no timeline.
#[unsafe(no_mangle)]
pub extern "C" fn animations_current_time_at(
    animation: &AnimationPlayback,
    timeline_time: OptionalTimeValue,
) -> OptionalTimeValue {
    timing::current_time_at(animation, timeline_time.get()).into()
}

/// The play state of `animation` at its current time `current_time`, with the end time of its associated
/// effect, or zero without one, `associated_effect_end`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_play_state_at(
    animation: &AnimationPlayback,
    current_time: OptionalTimeValue,
    associated_effect_end: TimeValue,
) -> AnimationPlayState {
    timing::play_state_at(animation, current_time.get(), associated_effect_end)
}

/// Whether `animation`, on `timeline`, if any, is relevant, with its associated effect, if any, of
/// `effect_timing` at the local time `local_time`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_is_relevant(
    animation: &AnimationPlayback,
    timeline: Option<&TimelineSample>,
    effect_timing: Option<&AnimationEffectTiming>,
    local_time: OptionalTimeValue,
) -> bool {
    timing::is_relevant(animation, timeline, effect_timing, local_time.get())
}
