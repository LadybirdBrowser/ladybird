/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS transitions: what becomes of a transition from its provisional start until its element holds it, and how the
//! host takes in the transitions the hover of a clock lane left running.
//! https://drafts.csswg.org/css-transitions/#starting

use super::animation::{AnimationHandle, AnimationRecord};
use super::easing::CompactEasing;
use super::effect::{AnimationEffectId, EffectHandle};
use super::element_animations::{AnimationOwnerSlot, ElementAnimations};
use super::host_effects::{HostEffectKind, HostEffects};
use super::procedures::{Invalidate, Procedures};
use super::timing::{AnimationPlayState, FillMode, SpecifiedIterationDuration};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::transition::{FfiExistingTransition, FfiLaneTransition};

/// Who started a transition the element comes to hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransitionOrigin {
    /// A style change event of the host's.
    StyleChange,
    /// The hover of a clock lane, which the host took in.
    Lane,
}

/// What becomes of a running transition of the host's as it takes in what the hover of a clock lane left the element
/// running.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LaneAdoption {
    /// The transition stands: the lane runs it as the host does, or the lane's steps saw it without ending it.
    Stands,
    /// The transition ends: the lane's steps ended it as they ran, or the lane started it, the host took it in, and the
    /// lane no longer runs it.
    Ends,
    /// The host started the transition since the lane forked, so the lane decided nothing over it, and the host decides
    /// what becomes of it and of its property in its turn.
    HostDecides,
}

impl AnimationRecord {
    /// The identity of the effect of the transition, if it has one.
    fn transition_effect_id(&self) -> Option<AnimationEffectId> {
        self.effect().map(|effect| effect.borrow().id())
    }

    /// Whether `lane_transition` is the lane's run of the transition: the host's transition the lane took in by its
    /// effect, or, for one the lane started itself, one the host took in from the lane, started at the same time.
    fn is_run_by_lane_as(&self, lane_transition: &FfiLaneTransition) -> bool {
        let state = self.css_transition();
        if lane_transition.property_id != state.property_id {
            return false;
        }
        if lane_transition.host_identity != AnimationEffectId::default() {
            return self.transition_effect_id() == Some(lane_transition.host_identity);
        }
        state.adopted_from_lane && state.start_time == lane_transition.start_time + lane_transition.delay
    }

    /// Whether the transition already runs from when the lane started it, as one of `lane_transitions` does.
    pub(crate) fn runs_as_lane_does(&self, lane_transitions: &[FfiLaneTransition]) -> bool {
        lane_transitions
            .iter()
            .any(|lane_transition| self.is_run_by_lane_as(lane_transition))
    }

    /// Whether `lane_transition` is the lane's run of this transition of the host's, which the lane took to start.
    pub(crate) fn is_host_transition_of(&self, lane_transition: &FfiLaneTransition) -> bool {
        lane_transition.host_identity != AnimationEffectId::default()
            && self.transition_effect_id() == Some(lane_transition.host_identity)
    }

    /// What becomes of the transition, which runs, as the host takes in `lane_transitions`, the transitions the lane
    /// leaves its element running, where the lane's steps ended those of the host's transitions whose effects
    /// `cancelled` names, and saw those `seen` names.
    pub(crate) fn lane_adoption(
        &self,
        lane_transitions: &[FfiLaneTransition],
        cancelled: &[AnimationEffectId],
        seen: &[AnimationEffectId],
    ) -> LaneAdoption {
        if self.runs_as_lane_does(lane_transitions) {
            return LaneAdoption::Stands;
        }
        let identity = self.transition_effect_id();
        let is_named_by = |identities: &[AnimationEffectId]| identity.is_some_and(|id| identities.contains(&id));
        if is_named_by(cancelled) {
            LaneAdoption::Ends
        } else if is_named_by(seen) {
            LaneAdoption::Stands
        } else if self.css_transition().adopted_from_lane {
            LaneAdoption::Ends
        } else {
            LaneAdoption::HostDecides
        }
    }

    /// What a style change event that decides the transitions of the transition's target reads of the transition, at
    /// time `t` of the document timeline, in milliseconds. The host gives the current value of a running transition of
    /// a custom property.
    pub(crate) fn existing_transition(&self, t: f64) -> FfiExistingTransition {
        let state = self.css_transition();
        let running = !self.playback().is_finished && self.play_state() != AnimationPlayState::Idle;
        FfiExistingTransition {
            property_id: state.property_id,
            custom_name: state.custom_property_name.raw(),
            custom_current_value: std::ptr::null(),
            running,
            end_value: state.end_value.pointer(),
            reversing_adjusted_start_value: state.reversing_adjusted_start_value.pointer(),
            timing_function_output: if running {
                self.timing_function_output_at_time(t)
            } else {
                0.0
            },
            reversing_shortening_factor: state.reversing_shortening_factor,
        }
    }

    /// The output of the timing function of the effect the transition was started with at time `t` of the document
    /// timeline, in milliseconds, through the transition's interval.
    fn timing_function_output_at_time(&self, t: f64) -> f64 {
        let state = self.css_transition();
        let effect = state
            .effect_started_with
            .as_ref()
            .expect("a transition is started with an effect")
            .borrow();
        // AD-HOC: If the transition has an empty duration then we get NaN here,
        // setting progress to 1 because an instant transition may be considered "finished".
        let mut progress = 1.0;
        if state.start_time < state.end_time {
            progress = (t - state.start_time) / (state.end_time - state.start_time);
        }

        // FIXME: Is this before_flag value correct?
        let before_flag = t < state.start_time;
        effect.timing_function().as_easing().evaluate_at(progress, before_flag)
    }
}

/// The timing a transition starts with: its delay and duration, in milliseconds, and its timing function, with the
/// serialization of the value it was made from.
pub(crate) struct ProvisionalTransitionTiming {
    pub(crate) delay: f64,
    pub(crate) duration: f64,
    pub(crate) timing_function: CompactEasing,
    pub(crate) timing_function_serialization: RetainedUtf16FlyString,
}

impl Procedures<'_> {
    /// Starts the transition, whose timeline is set already, provisionally, with `effect`, which targets its owning
    /// element and runs its keyframes, as its associated effect: the transition plays, but its target does not observe
    /// it until the stabilization epoch that started it commits it.
    /// https://drafts.csswg.org/css-transitions/#starting
    pub(crate) fn start_a_provisional_transition(
        &mut self,
        effect: &EffectHandle,
        timing: ProvisionalTransitionTiming,
    ) {
        {
            let mut effect = effect.borrow_mut();
            let mut specified = *effect.specified_timing();
            specified.start_delay = timing.delay;
            specified.iteration_duration = SpecifiedIterationDuration {
                is_auto: false,
                milliseconds: timing.duration,
            };
            // AD-HOC: CSS Transitions require the start value to apply during transition-delay. A default
            //         KeyframeEffect does not fill in the before phase, so use backwards fill to keep the transition
            //         value in the cascade until the active interval starts.
            specified.fill_mode = FillMode::Backwards;
            effect.set_specified_timing(specified);
            effect.set_timing_function(timing.timing_function, timing.timing_function_serialization);
        }
        // NB: Setting the effect normalizes its specified timing against the timeline.
        self.animation().borrow_mut().css_transition_mut().effect_started_with = Some(effect.clone());
        self.set_provisional_effect(effect);
        // NB: The current style computation collects the effect before publishing its result, so invalidating it here
        //     would evaluate the same transition twice.
        self.play(Invalidate::No)
            .expect("playing a transition on a monotonically increasing timeline does not throw");
    }

    /// Discards the transition, which was started provisionally and has no timeline any more: a later pass of the
    /// stabilization epoch that started it decided otherwise.
    pub(crate) fn discard_provisional_transition(&mut self) {
        self.discard_provisional_effect();
        let mut record = self.animation().borrow_mut();
        let state = record.css_transition_mut();
        assert!(state.provisional, "the transition is provisional");
        state.provisional = false;
    }
}

/// Commits `transition`, which was started provisionally, as the running transition of its property the owner at
/// `slot` of `animations` holds, where it is one that can hold one, which `origin` started, and hands back what that
/// asks the host to do: associate the transition with its target.
pub(crate) fn commit_provisional_transition(
    animations: Option<(&mut ElementAnimations, AnimationOwnerSlot)>,
    transition: AnimationHandle,
    origin: TransitionOrigin,
) -> HostEffects {
    let mut effects = HostEffects::default();
    let target = transition
        .borrow()
        .effect()
        .and_then(|effect| effect.borrow().target_element())
        .expect("a transition targets its owning element");
    effects.push(&transition, HostEffectKind::AssociateWithTarget { element: target });
    if let Some((animations, slot)) = animations {
        animations.commit_provisional_transition(slot, transition, origin);
    }
    effects
}

impl ElementAnimations {
    /// Commits `transition`, which was started provisionally and whose target is now associated with it, as the running
    /// transition of its property the owner at `slot` holds, which `origin` started.
    pub(crate) fn commit_provisional_transition(
        &mut self,
        slot: AnimationOwnerSlot,
        transition: AnimationHandle,
        origin: TransitionOrigin,
    ) {
        {
            let mut record = transition.borrow_mut();
            let state = record.css_transition_mut();
            assert!(state.provisional, "the transition is provisional");
            state.provisional = false;
            state.adopted_from_lane = origin == TransitionOrigin::Lane;
        }
        self.add_transition(slot, transition);
    }
}
