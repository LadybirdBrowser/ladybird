/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host's per-frame decisions read of an animation and its associated effect: the throttling of its style
//! updates and ticks, the wakeups it arms, and what the compositor update hands the compositor. The record resolves
//! all of it from one current time, and decides what follows from its timing alone.

use super::animation::{AnimationKind, AnimationRecord, TransitionPhase};
use super::timeline_state::time_unit_of;
use super::timing::{
    self, AnimationEffectPhase, AnimationEffectTiming, AnimationPlayState, AnimationPlayback, OptionalNumber,
    OptionalTimeValue, TimeValue, TimeValueType,
};
use crate::css::animation::FfiCompositeOperation;

/// What made an animation.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FfiAnimationKind {
    Script,
    CssAnimation,
    CssTransition,
    Smil,
}

/// The instant a compositor animation of an effect is anchored at, as far as the record decides it.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FfiCompositorAnchorKind {
    /// The timing of the animation keeps the compositor from running it.
    None,
    /// The animation is anchored at the timestamp of the document's last animation frame.
    LastAnimationFrame,
    /// The animation is anchored at an origin-relative time.
    OriginRelativeTime,
}

/// What the host's per-frame decisions read of an animation and its associated effect, all of it at the current time
/// of the animation.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiAnimationTimingState {
    pub kind: FfiAnimationKind,
    pub playback: AnimationPlayback,
    pub play_state: AnimationPlayState,
    pub has_monotonically_increasing_timeline: bool,
    /// https://www.w3.org/TR/web-animations-1/#animation-current-time
    pub current_time: OptionalTimeValue,
    /// The current time of the animation at the timestamp the host asked about, where it asked about one.
    pub current_time_at_timestamp: OptionalTimeValue,
    /// Whether the animation has an associated effect, which the rest describe.
    pub has_effect: bool,
    pub effect: AnimationEffectTiming,
    pub composite: FfiCompositeOperation,
    pub phase: AnimationEffectPhase,
    pub is_in_effect: bool,
    pub end_time: TimeValue,
    pub previous_current_iteration: f64,
    /// Whether the animation is a CSS transition that has not started yet, whose play is pending until the next
    /// animation update.
    pub is_initial_pending_css_transition: bool,
    /// Where the compositor may run the effect as far as its timing goes, the instant it is anchored at, and the local
    /// time it has there.
    pub compositor_anchor: FfiCompositorAnchorKind,
    pub compositor_anchor_origin_relative_time: f64,
    pub compositor_anchor_local_time: f64,
    /// Whether the timing of the effect lets a paint-only effect that nothing on screen shows skip its updates.
    pub can_throttle_offscreen_effect: bool,
    /// Whether the timing of the effect lets an effect that the winners of all its properties replace skip its
    /// updates.
    pub can_throttle_replaced_effect: bool,
    /// The time until the effect next changes phase, where it is ahead.
    pub next_phase_boundary_delay: OptionalNumber,
    /// The local time of the next iteration event of a CSS animation whose ticks are throttled, as far as its timing
    /// decides.
    pub next_throttled_iteration_event_time: OptionalNumber,
}

fn is_milliseconds(time: TimeValue) -> bool {
    time.r#type == TimeValueType::Milliseconds
}

impl AnimationRecord {
    /// What made the animation.
    pub(crate) fn ffi_kind(&self) -> FfiAnimationKind {
        match self.kind() {
            AnimationKind::Script => FfiAnimationKind::Script,
            AnimationKind::CssAnimation(_) => FfiAnimationKind::CssAnimation,
            AnimationKind::CssTransition(_) => FfiAnimationKind::CssTransition,
            AnimationKind::Smil { .. } => FfiAnimationKind::Smil,
        }
    }

    /// A zero time in the unit of the times of the timeline of the animation.
    pub(crate) fn zero_time(&self) -> TimeValue {
        let progress_based = self
            .timeline()
            .is_some_and(|timeline| timeline.borrow().is_progress_based());
        TimeValue::zero(time_unit_of(progress_based))
    }

    /// https://www.w3.org/TR/web-animations-1/#associated-effect-end
    pub(crate) fn associated_effect_end(&self) -> TimeValue {
        // The associated effect end of an animation is equal to the end time of the animation’s associated effect. If
        // the animation has no associated effect, the associated effect end is zero.
        match self.effect() {
            Some(effect) => timing::end_time(&effect.borrow().timing()),
            None => self.zero_time(),
        }
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-current-time
    pub(crate) fn current_time(&self) -> Option<TimeValue> {
        let timeline_time = self.timeline().and_then(|timeline| timeline.current_time());
        timing::current_time_at(self.playback(), timeline_time)
    }

    /// https://www.w3.org/TR/web-animations-1/#play-states
    pub(crate) fn play_state(&self) -> AnimationPlayState {
        self.play_state_at(self.current_time())
    }

    /// https://www.w3.org/TR/web-animations-1/#play-states
    /// At `current_time`, the current time of the animation.
    pub(crate) fn play_state_at(&self, current_time: Option<TimeValue>) -> AnimationPlayState {
        timing::play_state_at(self.playback(), current_time, self.associated_effect_end())
    }

    /// Whether the animation has a monotonically increasing timeline.
    pub(crate) fn has_monotonically_increasing_timeline(&self) -> bool {
        self.timeline()
            .is_some_and(|timeline| timeline.borrow().is_monotonically_increasing())
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-effect-phases-and-states
    /// The phase of the associated effect of the animation at the current time of the animation, idle without one.
    pub(crate) fn effect_phase(&self) -> AnimationEffectPhase {
        let Some(effect) = self.effect() else {
            return AnimationEffectPhase::Idle;
        };
        let sample = self.timeline().map(|timeline| timeline.sample());
        let timeline_time = sample.as_ref().and_then(|sample| sample.current_time.get());
        let current_time = timing::current_time_at(self.playback(), timeline_time);
        timing::resolve_timing(
            &effect.borrow().timing(),
            current_time,
            Some(self.playback()),
            sample.as_ref(),
        )
        .phase
    }

    /// What the host's per-frame decisions read of the animation now, with its current time at `timestamp`, where the
    /// host asks about one.
    pub(crate) fn timing_state(&self, timestamp: Option<f64>) -> FfiAnimationTimingState {
        let playback = *self.playback();
        let timeline = self.timeline();
        let sample = timeline.map(|timeline| timeline.sample());
        let current_time =
            timing::current_time_at(&playback, sample.as_ref().and_then(|sample| sample.current_time.get()));
        let current_time_at_timestamp = timestamp.and_then(|timestamp| {
            let timeline_time = timeline.and_then(|timeline| timeline.current_time_at_timestamp(timestamp, None));
            timing::current_time_at(&playback, timeline_time)
        });
        let has_monotonically_increasing_timeline = sample.is_some_and(|sample| sample.is_monotonically_increasing);

        let effect = self.effect().map(|effect| effect.borrow());
        let effect_timing = effect.as_ref().map(|effect| effect.timing());
        let resolved = effect_timing
            .as_ref()
            .map(|effect_timing| timing::resolve_timing(effect_timing, current_time, Some(&playback), sample.as_ref()));
        let associated_effect_end = self.associated_effect_end();
        let mut state = FfiAnimationTimingState {
            kind: self.ffi_kind(),
            playback,
            play_state: timing::play_state_at(&playback, current_time, associated_effect_end),
            has_monotonically_increasing_timeline,
            current_time: current_time.into(),
            current_time_at_timestamp: current_time_at_timestamp.into(),
            has_effect: effect.is_some(),
            effect: effect_timing.unwrap_or_default(),
            composite: effect
                .as_ref()
                .map_or(FfiCompositeOperation::Replace, |effect| effect.composite()),
            phase: resolved
                .as_ref()
                .map_or(AnimationEffectPhase::Idle, |resolved| resolved.phase),
            is_in_effect: resolved.as_ref().is_some_and(timing::is_in_effect),
            end_time: associated_effect_end,
            previous_current_iteration: effect.as_ref().map_or(0.0, |effect| effect.previous_current_iteration),
            is_initial_pending_css_transition: playback.pending()
                && playback.has_pending_play_task
                && matches!(self.kind(), AnimationKind::CssTransition(state) if state.previous_phase == TransitionPhase::Idle),
            compositor_anchor: FfiCompositorAnchorKind::None,
            compositor_anchor_origin_relative_time: 0.0,
            compositor_anchor_local_time: 0.0,
            can_throttle_offscreen_effect: false,
            can_throttle_replaced_effect: false,
            next_phase_boundary_delay: None.into(),
            next_throttled_iteration_event_time: None.into(),
        };
        drop(effect);
        if !state.has_effect {
            return state;
        }

        // NB: A compositor animation runs from an anchor on the monotonic clock, at the playback rate, through one
        //     iteration or an infinite number of them.
        let timing = state.effect;
        let rate = playback.playback_rate;
        let runs_forward = rate > 0.0 && rate.is_finite();
        let runs_on_a_monotonic_clock = state.play_state == AnimationPlayState::Running
            && !playback.pending()
            && rate > 0.0
            && has_monotonically_increasing_timeline;
        let runs_one_or_infinite_iterations = timing.iteration_count.is_infinite() || timing.iteration_count == 1.0;
        let times_in_milliseconds = is_milliseconds(timing.start_delay) && is_milliseconds(timing.iteration_duration);
        let has_finite_positive_duration =
            timing.iteration_duration.value > 0.0 && timing.iteration_duration.value.is_finite();

        let compositor_may_run_it = state.play_state == AnimationPlayState::Running
            && (!playback.pending() || state.is_initial_pending_css_transition)
            && has_monotonically_increasing_timeline
            && matches!(state.phase, AnimationEffectPhase::Before | AnimationEffectPhase::Active)
            && runs_one_or_infinite_iterations
            && state.composite == FfiCompositeOperation::Replace
            && runs_forward
            && times_in_milliseconds
            && timing.iteration_duration.value > 0.0
            && (state.phase != AnimationEffectPhase::Before
                || timing::before_active_boundary_time(&timing) == timing.start_delay);
        if let Some(local_time) = current_time.filter(|time| compositor_may_run_it && is_milliseconds(*time)) {
            state.compositor_anchor_local_time = local_time.value;
            if !state.is_initial_pending_css_transition {
                state.compositor_anchor = FfiCompositorAnchorKind::LastAnimationFrame;
            } else if let Some(origin_relative_time) = timeline.and_then(|timeline| {
                // OPTIMIZATION: A new CSS transition is play-pending until the next animation update. Start its
                //               compositor animation at the style change event time without resolving its pending task.
                timeline.convert_a_timeline_time_to_an_origin_relative_time(Some(TimeValue::milliseconds(
                    self.css_transition_style_change_event_time(),
                )))
            }) {
                state.compositor_anchor = FfiCompositorAnchorKind::OriginRelativeTime;
                state.compositor_anchor_origin_relative_time = origin_relative_time;
            }
        }

        state.can_throttle_offscreen_effect = runs_on_a_monotonic_clock
            && rate.is_finite()
            && state.phase == AnimationEffectPhase::Active
            && times_in_milliseconds
            && has_finite_positive_duration
            && timing.end_delay.value == 0.0;
        state.can_throttle_replaced_effect = runs_on_a_monotonic_clock
            && state.phase == AnimationEffectPhase::Active
            && runs_one_or_infinite_iterations
            && times_in_milliseconds;

        if let Some(current_time) =
            current_time.filter(|time| times_in_milliseconds && is_milliseconds(*time) && rate > 0.0)
        {
            let mut next_phase_boundary = timing.start_delay.value;
            let ahead = if current_time.value < next_phase_boundary {
                true
            } else {
                next_phase_boundary += timing.iteration_duration.value * timing.iteration_count;
                !timing.iteration_count.is_infinite()
            };
            let delay = (next_phase_boundary - current_time.value) / rate;
            if ahead && delay > 0.0 {
                state.next_phase_boundary_delay = Some(delay).into();
            }
        }

        // NB: Observable throttled animations need a rendering update at the next iteration boundary, not at every
        //     display refresh. Seeking and cancellation already request their own updates.
        if state.kind == FfiAnimationKind::CssAnimation
            && !playback.pending()
            && state.play_state == AnimationPlayState::Running
            && runs_forward
            && has_monotonically_increasing_timeline
            && times_in_milliseconds
            && has_finite_positive_duration
        {
            state.next_throttled_iteration_event_time = Some(
                timing.start_delay.value
                    + (state.previous_current_iteration + 1.0 - timing.iteration_start)
                        * timing.iteration_duration.value,
            )
            .into();
        }
        state
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::animations::animation::AnimationHandle;
    use crate::animations::effect::EffectHandle;
    use timing::{SpecifiedEffectTiming, SpecifiedIterationDuration};

    /// A script animation of an effect with an iteration duration of a second, `iterations` times, held at
    /// `current_time`.
    fn animation_held_at(current_time: f64, iterations: f64) -> AnimationHandle {
        let effect = EffectHandle::new();
        effect.borrow_mut().set_specified_timing(SpecifiedEffectTiming {
            iteration_duration: SpecifiedIterationDuration {
                is_auto: false,
                milliseconds: 1000.0,
            },
            start_delay: 100.0,
            iteration_count: iterations,
            ..SpecifiedEffectTiming::default()
        });
        effect.borrow_mut().normalize_specified_timing(None);
        let animation = AnimationHandle::new(AnimationKind::Script);
        animation.borrow_mut().set_effect(Some(effect));
        animation.borrow_mut().playback_mut().hold_time = Some(TimeValue::milliseconds(current_time)).into();
        animation
    }

    #[test]
    fn the_next_phase_boundary_is_the_start_delay_and_then_the_active_end() {
        let state = animation_held_at(50.0, 2.0).borrow().timing_state(None);
        assert_eq!(state.phase, AnimationEffectPhase::Before);
        assert_eq!(state.next_phase_boundary_delay.get(), Some(50.0));

        let state = animation_held_at(600.0, 2.0).borrow().timing_state(None);
        assert_eq!(state.next_phase_boundary_delay.get(), Some(1500.0));

        let state = animation_held_at(600.0, f64::INFINITY).borrow().timing_state(None);
        assert_eq!(state.next_phase_boundary_delay.get(), None);
    }
}
