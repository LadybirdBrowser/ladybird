/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The events of a CSS animation or a CSS transition that a new animation frame establishes, which the record decides
//! from the phases its animation and associated effect go through.

use super::animation::{AnimationKind, AnimationRecord, TransitionPhase};
use super::timing::time_value::{max, min};
use super::timing::{self, AnimationEffectPhase, OptionalNumber, TimeValue, TimeValueType};

/// The type of an event of a CSS animation or a CSS transition.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FfiCssEventType {
    AnimationStart,
    AnimationEnd,
    AnimationIteration,
    AnimationCancel,
    TransitionRun,
    TransitionStart,
    TransitionEnd,
    TransitionCancel,
}

/// An event to dispatch, with its elapsed time in seconds.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CssEvent {
    pub r#type: FfiCssEventType,
    pub elapsed_time: f64,
}

/// The events an animation frame establishes for an animation, in the order they are dispatched, and the time they
/// were scheduled at. A frame establishes at most three, the run, start and end events of a transition that went from
/// idle to after.
#[derive(Clone, Copy, Debug)]
pub struct CssEvents {
    pub count: u8,
    pub events: [CssEvent; 3],
    /// The origin-relative time of the current time of the timeline, where it has one.
    pub scheduled_event_time: OptionalNumber,
}

impl CssEvents {
    fn new() -> Self {
        Self {
            count: 0,
            events: [CssEvent {
                r#type: FfiCssEventType::AnimationStart,
                elapsed_time: 0.0,
            }; 3],
            scheduled_event_time: None.into(),
        }
    }

    fn push(&mut self, r#type: FfiCssEventType, elapsed_time: TimeValue) {
        let elapsed_time = match elapsed_time.r#type {
            TimeValueType::Milliseconds => elapsed_time.value / 1000.0,
            // FIXME: The spec doesn't specify how to handle this case
            TimeValueType::Percentage => 0.0,
        };
        self.events[usize::from(self.count)] = CssEvent { r#type, elapsed_time };
        self.count += 1;
    }

    pub(crate) fn as_slice(&self) -> &[CssEvent] {
        &self.events[..usize::from(self.count)]
    }
}

impl TransitionPhase {
    fn of_effect(phase: AnimationEffectPhase) -> Self {
        match phase {
            AnimationEffectPhase::Before => Self::Before,
            AnimationEffectPhase::Active => Self::Active,
            AnimationEffectPhase::After => Self::After,
            AnimationEffectPhase::Idle => Self::Idle,
        }
    }
}

/// The intervals whose boundaries give the elapsed time of a transition event.
#[derive(Clone, Copy)]
enum Interval {
    Start,
    End,
    ActiveTime,
}

impl AnimationRecord {
    /// The events of a CSS animation or a CSS transition that the animation frame just established, recording the
    /// phases they were decided from for the next frame. Other animations have none.
    pub(crate) fn take_css_events(&mut self) -> CssEvents {
        let mut events = CssEvents::new();
        match self.kind() {
            AnimationKind::CssTransition(_) => self.establish_transition_events(&mut events),
            AnimationKind::CssAnimation(_) => self.establish_css_animation_events(&mut events),
            AnimationKind::Script | AnimationKind::Smil { .. } => {}
        }
        if events.count > 0 {
            events.scheduled_event_time = self
                .timeline()
                .and_then(|timeline| {
                    timeline.convert_a_timeline_time_to_an_origin_relative_time(timeline.current_time())
                })
                .into();
        }
        events
    }

    // https://drafts.csswg.org/css-transitions-2/#event-dispatch
    fn establish_transition_events(&mut self, events: &mut CssEvents) {
        let previous_phase = self.css_transition().previous_phase;
        let playback = *self.playback();
        let timeline = self.timeline();
        let current_time = timing::current_time_at(&playback, timeline.and_then(|timeline| timeline.current_time()));
        let sample = timeline.map(|timeline| timeline.sample());
        let effect_timing = self.effect().map(|effect| effect.borrow().timing());

        // The transition phase of a transition is initially ‘idle’ and is updated on each
        // animation frame according to the first matching condition from below:
        let transition_phase = match &effect_timing {
            // If the transition has no associated effect,
            None => match current_time {
                // If the transition has an unresolved current time,
                //   The transition phase is ‘idle’.
                None => TransitionPhase::Idle,
                // If the transition has a current time < 0,
                //   The transition phase is ‘before’.
                Some(current_time) if current_time.value < 0.0 => TransitionPhase::Before,
                // Otherwise,
                //   The transition phase is ‘after’.
                Some(_) => TransitionPhase::After,
            },
            // If the transition has a pending play task or a pending pause task
            // and its phase was previously ‘idle’ or ‘pending’,
            //   The transition phase is ‘pending’.
            Some(_)
                if playback.pending() && matches!(previous_phase, TransitionPhase::Idle | TransitionPhase::Pending) =>
            {
                TransitionPhase::Pending
            }
            // Otherwise,
            //   The transition phase is the phase of its associated effect.
            Some(effect_timing) => TransitionPhase::of_effect(
                timing::resolve_timing(effect_timing, current_time, Some(&playback), sample.as_ref()).phase,
            ),
        };

        // The target for a transition event is the transition’s owning element. If there is no owning element,
        // no transition events are dispatched.
        let effect_timing = effect_timing.filter(|_| self.owning_element().is_some());
        let zero = self.zero_time();
        let mut dispatch_event = |r#type: FfiCssEventType, interval: Interval| {
            let Some(effect_timing) = &effect_timing else {
                return;
            };
            let active_duration = timing::active_duration(effect_timing);
            let elapsed_time = match interval {
                Interval::Start => max(min(-effect_timing.start_delay, active_duration), zero),
                Interval::End => max(
                    min(
                        timing::end_time(effect_timing) - effect_timing.start_delay,
                        active_duration,
                    ),
                    zero,
                ),
                // The active time of the animation at the moment it was canceled calculated using a fill mode of both.
                // FIXME: Compute this properly.
                Interval::ActiveTime => zero,
            };
            events.push(r#type, elapsed_time);
        };

        use FfiCssEventType::{TransitionCancel, TransitionEnd, TransitionRun, TransitionStart};
        use TransitionPhase::{Active, After, Before, Idle, Pending};
        match (previous_phase, transition_phase) {
            (Idle, Pending | Before) => dispatch_event(TransitionRun, Interval::Start),
            (Idle, Active) => {
                dispatch_event(TransitionRun, Interval::Start);
                dispatch_event(TransitionStart, Interval::Start);
            }
            (Idle, After) => {
                dispatch_event(TransitionRun, Interval::Start);
                dispatch_event(TransitionStart, Interval::Start);
                dispatch_event(TransitionEnd, Interval::End);
            }
            (Pending | Before, Active) => dispatch_event(TransitionStart, Interval::Start),
            (Pending | Before, After) => {
                dispatch_event(TransitionStart, Interval::Start);
                dispatch_event(TransitionEnd, Interval::End);
            }
            (Active, After) => dispatch_event(TransitionEnd, Interval::End),
            (Active, Before) => dispatch_event(TransitionEnd, Interval::Start),
            (After, Active) => dispatch_event(TransitionStart, Interval::End),
            (After, Before) => {
                dispatch_event(TransitionStart, Interval::End);
                dispatch_event(TransitionEnd, Interval::Start);
            }
            _ => {}
        }

        if transition_phase == Idle && previous_phase != Idle && previous_phase != After {
            dispatch_event(TransitionCancel, Interval::ActiveTime);
        }

        self.css_transition_mut().previous_phase = transition_phase;
    }

    // https://www.w3.org/TR/css-animations-2/#event-dispatch
    fn establish_css_animation_events(&mut self, events: &mut CssEvents) {
        // Each time a new animation frame is established and the animation does not have a pending play task or pending
        // pause task, the events to dispatch are determined by comparing the animation’s phase before and after
        // establishing the new animation frame as follows:
        let playback = *self.playback();
        if playback.pending() || self.owning_element().is_none() {
            return;
        }
        let Some(effect) = self.effect().cloned() else {
            return;
        };
        let timeline = self.timeline();
        let current_time = timing::current_time_at(&playback, timeline.and_then(|timeline| timeline.current_time()));
        let sample = timeline.map(|timeline| timeline.sample());
        let (effect_timing, previous_phase, previous_current_iteration) = {
            let effect = effect.borrow();
            (
                effect.timing(),
                effect.previous_phase,
                effect.previous_current_iteration,
            )
        };
        let resolved = timing::resolve_timing(&effect_timing, current_time, Some(&playback), sample.as_ref());
        let current_phase = resolved.phase;
        let current_iteration = timing::current_iteration(&effect_timing, &resolved).unwrap_or(0.0);
        let zero = self.zero_time();

        // For calculating the elapsedTime of each event, the following definitions are used:

        // - interval start = max(min(-start delay, active duration), 0)
        let active_duration = timing::active_duration(&effect_timing);
        let interval_start = max(min(-effect_timing.start_delay, active_duration), zero);

        // - interval end = max(min(associated effect end - start delay, active duration), 0)
        let interval_end = max(
            min(
                timing::end_time(&effect_timing) - effect_timing.start_delay,
                active_duration,
            ),
            zero,
        );

        use AnimationEffectPhase::{Active, After, Before, Idle};
        use FfiCssEventType::{AnimationCancel, AnimationEnd, AnimationIteration, AnimationStart};
        match (previous_phase, current_phase) {
            (Before | Idle, Active) => events.push(AnimationStart, interval_start),
            (Before | Idle, After) => {
                events.push(AnimationStart, interval_start);
                events.push(AnimationEnd, interval_end);
            }
            (Active, Before) => events.push(AnimationEnd, interval_start),
            (Active, Active) if previous_current_iteration != current_iteration => {
                // The elapsed time for an animationiteration event is defined as follows:

                // 1. Let previous current iteration be the current iteration from the previous animation frame.

                // 2. If previous current iteration is greater than current iteration, let iteration boundary be current iteration + 1,
                //    otherwise let it be current iteration.
                let iteration_boundary = if previous_current_iteration > current_iteration {
                    current_iteration + 1.0
                } else {
                    current_iteration
                };

                // 3. The elapsed time is the result of evaluating (iteration boundary - iteration start) × iteration duration).
                let elapsed_time =
                    effect_timing.iteration_duration * (iteration_boundary - effect_timing.iteration_start);

                events.push(AnimationIteration, elapsed_time);
            }
            (Active, After) => events.push(AnimationEnd, interval_end),
            (After, Active) => events.push(AnimationStart, interval_end),
            (After, Before) => {
                events.push(AnimationStart, interval_end);
                events.push(AnimationEnd, interval_start);
            }
            _ => {}
        }

        if current_phase == Idle && previous_phase != Idle && previous_phase != After {
            // FIXME: Calculate a non-zero time when the animation is canceled by means other than calling cancel()
            let cancel_time = self.saved_cancel_time.take().unwrap_or(zero);
            events.push(AnimationCancel, cancel_time);
        }

        let mut effect = effect.borrow_mut();
        effect.previous_phase = current_phase;
        effect.previous_current_iteration = current_iteration;
    }
}

#[cfg(test)]
mod tests {
    use core::ffi::c_void;
    use core::ptr::NonNull;

    use super::*;
    use crate::animations::animation::{AnimationHandle, CssTransitionState, OwningElement, TransitionPhase};
    use crate::animations::effect::EffectHandle;
    use crate::css::retained_fly_string::RetainedUtf16FlyString;
    use crate::css::style_value::RetainedStyleValueData;
    use timing::{SpecifiedEffectTiming, SpecifiedIterationDuration};

    /// A transition of a second after a delay of 100 milliseconds on an element, held at `current_time`.
    fn transition_held_at(current_time: f64) -> AnimationHandle {
        let effect = EffectHandle::new();
        effect.borrow_mut().set_specified_timing(SpecifiedEffectTiming {
            iteration_duration: SpecifiedIterationDuration {
                is_auto: false,
                milliseconds: 1000.0,
            },
            start_delay: 100.0,
            ..SpecifiedEffectTiming::default()
        });
        effect.borrow_mut().normalize_specified_timing(None);
        let animation = AnimationHandle::new(AnimationKind::CssTransition(CssTransitionState {
            custom_property_name: RetainedUtf16FlyString::none(),
            generation: 0,
            start_time: 0.0,
            start_value: RetainedStyleValueData::none(),
            end_value: RetainedStyleValueData::none(),
            reversing_adjusted_start_value: RetainedStyleValueData::none(),
            reversing_shortening_factor: 1.0,
            property_id: 1,
            previous_phase: TransitionPhase::Idle,
            provisional: false,
            initial_ready_time: None,
            adopted_from_lane: false,
            effect_started_with: None,
        }));
        animation.borrow_mut().set_owning_element(Some(OwningElement {
            element: NonNull::<c_void>::dangling(),
            pseudo_element: None,
        }));
        animation.borrow_mut().set_effect(Some(effect));
        animation.borrow_mut().playback_mut().hold_time = Some(TimeValue::milliseconds(current_time)).into();
        animation
    }

    fn event(r#type: FfiCssEventType, elapsed_time: f64) -> CssEvent {
        CssEvent { r#type, elapsed_time }
    }

    #[test]
    fn a_transition_that_finished_within_a_frame_runs_starts_and_ends() {
        let animation = transition_held_at(2000.0);
        let events = animation.borrow_mut().take_css_events();
        assert_eq!(
            events.as_slice(),
            [
                event(FfiCssEventType::TransitionRun, 0.0),
                event(FfiCssEventType::TransitionStart, 0.0),
                event(FfiCssEventType::TransitionEnd, 1.0),
            ]
        );
        assert_eq!(events.scheduled_event_time.get(), None);
        assert_eq!(animation.borrow_mut().take_css_events().count, 0);
    }

    #[test]
    fn a_transition_moving_back_before_its_start_ends_at_its_start() {
        let animation = transition_held_at(500.0);
        let events = animation.borrow_mut().take_css_events();
        assert_eq!(
            events.as_slice(),
            [
                event(FfiCssEventType::TransitionRun, 0.0),
                event(FfiCssEventType::TransitionStart, 0.0)
            ]
        );
        animation.borrow_mut().playback_mut().hold_time = Some(TimeValue::milliseconds(50.0)).into();
        let events = animation.borrow_mut().take_css_events();
        assert_eq!(events.as_slice(), [event(FfiCssEventType::TransitionEnd, 0.0)]);
    }

    #[test]
    fn a_transition_without_an_owning_element_has_no_events_but_keeps_its_phase() {
        let animation = transition_held_at(500.0);
        animation.borrow_mut().set_owning_element(None);
        assert_eq!(animation.borrow_mut().take_css_events().count, 0);
        animation.borrow_mut().set_owning_element(Some(OwningElement {
            element: NonNull::<c_void>::dangling(),
            pseudo_element: None,
        }));
        assert_eq!(animation.borrow_mut().take_css_events().count, 0);
    }
}
