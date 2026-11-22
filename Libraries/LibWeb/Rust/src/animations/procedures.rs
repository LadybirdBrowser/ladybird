/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The procedures of the Web Animations timing model that run on an animation.
//! https://www.w3.org/TR/web-animations-1/#animations
//!
//! A procedure changes the records it runs on and records what it asks the host to do as `HostEffects`, in the order
//! its steps ask for them, which the host performs once it has returned.

use super::animation::{
    AnimationCell, AnimationHandle, AnimationKind, AnimationRecord, CssAnimationFlag, TransitionPhase,
};
use super::composite_order::{TreeOrder, composite_order};
use super::effect::{EffectFlag, EffectHandle};
use super::host_effects::{HostEffectKind, HostEffects, PlaybackEventType};
use super::timeline::{TimelineCell, TimelineHandle};
use super::timeline_state::{TimelineState, zero_time};
use super::timing::{
    self, AnimationEffectPhase, AnimationEffectTiming, AnimationPlayState, AnimationPlayback, AnimationReplaceState,
    FillMode, TimeValue, TimeValueType, time_value,
};
use std::collections::HashSet;
use std::ffi::c_void;
use std::ptr::NonNull;

/// An exception a procedure throws, as the host throws it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AnimationError {
    /// An "InvalidStateError" DOMException.
    InvalidState(&'static str),
    TypeError(&'static str),
}

/// Whether a procedure updates the finished state of an animation after seeking it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DidSeek {
    No,
    Yes,
}

/// Whether the finish notification steps of an animation run right away rather than in a task.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SynchronouslyNotify {
    No,
    Yes,
}

/// Whether a procedure invalidates the style its animation's effect produces.
/// NB: Script always wants this. The style computation does not, as it runs a procedure before it computes the effect
///     stack it would invalidate.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Invalidate {
    No,
    Yes,
}

/// https://www.w3.org/TR/web-animations-1/#playing-an-animation-section
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AutoRewind {
    No,
    Yes,
}

/// Runs `procedure` on `animation` and hands back what it asks the host to do.
pub(crate) fn run<T>(
    animation: &AnimationHandle,
    procedure: impl FnOnce(&mut Procedures<'_>) -> Result<T, AnimationError>,
) -> Result<(T, HostEffects), AnimationError> {
    let timeline = animation.borrow().timeline_state();
    run_on(animation, timeline, procedure)
}

/// Answers what `procedure`, which only reads, finds of `animation`. Unlike `run`, this does not note that the
/// animation may need updates.
pub(crate) fn query<T>(animation: &AnimationHandle, procedure: impl FnOnce(&Procedures<'_>) -> T) -> T {
    let timeline = animation.borrow().timeline_state();
    let mut effects = HostEffects::default();
    procedure(&Procedures {
        animation,
        timeline,
        effects: &mut effects,
    })
}

/// Runs `procedure` on `animation`, whose timeline reads `timeline` now, if it has one, and hands back what it asks
/// the host to do.
fn run_on<T>(
    animation: &AnimationHandle,
    timeline: Option<TimelineState>,
    procedure: impl FnOnce(&mut Procedures<'_>) -> Result<T, AnimationError>,
) -> Result<(T, HostEffects), AnimationError> {
    let mut effects = HostEffects::default();
    let value = procedure(&mut Procedures {
        animation,
        timeline,
        effects: &mut effects,
    });
    note_animation_may_need_update(animation);
    Ok((value?, effects))
}

/// OPTIMIZATION: Notes that an update of the current time of the timeline of `animation`, if any, may change it, as a
///               procedure ran on it. The timeline updates only those of its animations.
pub(crate) fn note_animation_may_need_update(animation: &AnimationHandle) {
    let timeline = animation.borrow().timeline().cloned();
    if let Some(timeline) = timeline {
        timeline.animations_needing_update().borrow_mut().insert(animation);
    }
}

/// Whether an update of the current time of its timeline leaves `animation` as it is until a procedure runs on it: it
/// has finished on a monotonically increasing timeline that is not progress-based, and has no pending task, which is
/// what the update of an animation as its timeline updates skips.
pub(crate) fn update_leaves_animation_as_it_is(animation: &AnimationCell) -> bool {
    let record = animation.borrow();
    let playback = record.playback();
    if !playback.is_finished || playback.pending() {
        return false;
    }
    record
        .timeline_state()
        .is_some_and(|timeline| timeline.sample.is_monotonically_increasing && !timeline.is_progress_based)
        && record.play_state() == AnimationPlayState::Finished
}

/// An animation and its timeline as it is now, on which the procedures run, and what they ask the host to do so far.
pub(crate) struct Procedures<'a> {
    animation: &'a AnimationHandle,
    /// The animation's timeline as the animation reads it as the procedures begin, if it has one.
    timeline: Option<TimelineState>,
    effects: &'a mut HostEffects,
}

impl Procedures<'_> {
    /// The animation the procedures run on.
    pub(crate) fn animation(&self) -> &AnimationHandle {
        self.animation
    }

    fn emit(&mut self, kind: HostEffectKind) {
        self.effects.push(self.animation, kind);
    }

    fn playback(&self) -> AnimationPlayback {
        *self.animation.borrow().playback()
    }

    fn update_playback(&self, update: impl FnOnce(&mut AnimationPlayback)) {
        update(self.animation.borrow_mut().playback_mut());
    }

    fn start_time(&self) -> Option<TimeValue> {
        self.playback().start_time.get()
    }

    fn hold_time(&self) -> Option<TimeValue> {
        self.playback().hold_time.get()
    }

    fn playback_rate(&self) -> f64 {
        self.playback().playback_rate
    }

    fn pending_playback_rate(&self) -> Option<f64> {
        self.playback().pending_playback_rate.get()
    }

    fn pending(&self) -> bool {
        self.playback().pending()
    }

    fn set_start_time(&self, time: Option<TimeValue>) {
        self.update_playback(|playback| playback.start_time = time.into());
    }

    fn set_hold_time(&self, time: Option<TimeValue>) {
        self.update_playback(|playback| playback.hold_time = time.into());
    }

    fn set_pending_playback_rate(&self, playback_rate: Option<f64>) {
        self.update_playback(|playback| playback.pending_playback_rate = playback_rate.into());
    }

    fn set_has_pending_play_task(&self, value: bool) {
        self.update_playback(|playback| playback.has_pending_play_task = value);
    }

    fn set_has_pending_pause_task(&self, value: bool) {
        self.update_playback(|playback| playback.has_pending_pause_task = value);
    }

    fn set_auto_align_start_time(&self, value: bool) {
        self.animation.borrow_mut().auto_align_start_time = value;
    }

    /// The current time of the timeline, which is unresolved without one or where it is inactive.
    fn timeline_time(&self) -> Option<TimeValue> {
        self.timeline.as_ref().and_then(TimelineState::current_time)
    }

    /// Whether the animation is associated with an active timeline.
    fn has_active_timeline(&self) -> bool {
        self.timeline.as_ref().is_some_and(|timeline| !timeline.is_inactive())
    }

    /// Whether the animation has an associated timeline that is not monotonically increasing.
    fn has_finite_timeline(&self) -> bool {
        self.timeline
            .as_ref()
            .is_some_and(|timeline| !timeline.sample.is_monotonically_increasing)
    }

    /// Whether the animation is associated with a monotonically increasing timeline.
    fn has_monotonically_increasing_timeline(&self) -> bool {
        self.timeline
            .as_ref()
            .is_some_and(|timeline| timeline.sample.is_monotonically_increasing)
    }

    fn zero(&self) -> TimeValue {
        zero_time(self.timeline.as_ref())
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-current-time
    fn current_time(&self) -> Option<TimeValue> {
        timing::current_time_at(&self.playback(), self.timeline_time())
    }

    /// The current time of the animation, substituting an unresolved time value for the hold time.
    fn current_time_without_hold_time(&self) -> Option<TimeValue> {
        let playback = AnimationPlayback {
            hold_time: None.into(),
            ..self.playback()
        };
        timing::current_time_at(&playback, self.timeline_time())
    }

    /// The normalized timing of the associated effect, if any.
    fn effect_timing(&self) -> Option<AnimationEffectTiming> {
        self.animation.borrow().effect().map(|effect| effect.borrow().timing())
    }

    /// https://www.w3.org/TR/web-animations-1/#associated-effect-end
    fn associated_effect_end(&self) -> TimeValue {
        // The associated effect end of an animation is equal to the end time of the animation’s associated effect. If
        // the animation has no associated effect, the associated effect end is zero.
        match self.effect_timing() {
            Some(timing) => timing::end_time(&timing),
            None => self.zero(),
        }
    }

    /// https://www.w3.org/TR/web-animations-1/#play-states
    pub(crate) fn play_state(&self) -> AnimationPlayState {
        timing::play_state_at(&self.playback(), self.current_time(), self.associated_effect_end())
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-relevant
    pub(crate) fn is_relevant(&self) -> bool {
        let effect_timing = self.effect_timing();
        timing::is_relevant(
            &self.playback(),
            self.timeline.as_ref().map(|timeline| &timeline.sample),
            effect_timing.as_ref(),
            self.current_time(),
        )
    }

    /// The phase of the associated effect, if any, at its local time, the current time of the animation.
    /// https://www.w3.org/TR/web-animations-1/#animation-effect-phases-and-states
    fn effect_phase(&self) -> Option<AnimationEffectPhase> {
        let timing = self.effect_timing()?;
        let playback = self.playback();
        let resolved = timing::resolve_timing(
            &timing,
            self.current_time(),
            Some(&playback),
            self.timeline.as_ref().map(|timeline| &timeline.sample),
        );
        Some(resolved.phase)
    }

    /// The host's `DOM::Element` the associated effect targets, if any.
    fn effect_target(&self) -> Option<NonNull<c_void>> {
        self.animation
            .borrow()
            .effect()
            .and_then(|effect| effect.borrow().target_element())
    }

    /// Whether the associated effect has a target.
    fn effect_has_target(&self) -> bool {
        self.effect_target().is_some()
    }

    fn invalidate_effect(&mut self) {
        if self.animation.borrow().effect().is_some() {
            self.emit(HostEffectKind::InvalidateEffect { changes_plan: true });
        }
    }

    /// https://www.w3.org/TR/web-animations-1/#apply-any-pending-playback-rate
    fn apply_any_pending_playback_rate(&self) {
        self.update_playback(|playback| {
            // 1. If animation does not have a pending playback rate, abort these steps.
            let Some(pending_playback_rate) = playback.pending_playback_rate.get() else {
                return;
            };

            // 2. Set animation’s playback rate to its pending playback rate.
            playback.playback_rate = pending_playback_rate;

            // 3. Clear animation’s pending playback rate.
            playback.pending_playback_rate = None.into();
        });
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-silently-set-the-current-time
    /// NB: The host validates seek time as a CSSNumberish time before it runs this procedure.
    fn silently_set_current_time(&self, valid_seek_time: Option<TimeValue>) -> Result<(), AnimationError> {
        // 1. If seek time is an unresolved time value, then perform the following steps.
        let Some(valid_seek_time) = valid_seek_time else {
            // 1. If the current time is resolved, then throw a TypeError.
            if self.current_time().is_some() {
                return Err(AnimationError::TypeError(
                    "Cannot change an animation's current time from a resolve value to an unresolved value",
                ));
            }

            // 2. Abort these steps.
            return Ok(());
        };

        // 2. Let valid seek time be the result of running the validate a CSSNumberish time procedure with seek time as
        //    the input.
        // 3. If valid seek time is false, abort this procedure.
        // AD-HOC: The host has already validated seek time.

        // 4. Set auto align start time to false.
        self.set_auto_align_start_time(false);

        // 5. Update either animation’s hold time or start time as follows:
        let playback = self.playback();

        // -> If any of the following conditions are true:
        //    - animation’s hold time is resolved, or
        //    - animation’s start time is unresolved, or
        //    - animation has no associated timeline or the associated timeline is inactive, or
        //    - animation’s playback rate is 0,
        match self.timeline_time() {
            Some(timeline_time)
                if playback.hold_time.get().is_none()
                    && playback.start_time.get().is_some()
                    && playback.playback_rate != 0.0 =>
            {
                // -> Otherwise,
                //    Set animation’s start time to the result of evaluating timeline time - (seek time / playback
                //    rate) where timeline time is the current time value of timeline associated with animation.
                self.set_start_time(Some(timeline_time - (valid_seek_time / playback.playback_rate)));
            }
            _ => {
                // Set animation’s hold time to seek time.
                self.set_hold_time(Some(valid_seek_time));
            }
        }

        // 6. If animation has no associated timeline or the associated timeline is inactive, make animation’s start
        //    time unresolved.
        if !self.has_active_timeline() {
            self.set_start_time(None);
        }

        // 7. Make animation’s previous current time unresolved.
        self.animation.borrow_mut().previous_current_time = None;

        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-set-the-current-time
    /// NB: The host validates seek time as a CSSNumberish time before it runs this procedure.
    fn set_current_time(&mut self, seek_time: Option<TimeValue>) -> Result<(), AnimationError> {
        // 1. Run the steps to silently set the current time of animation to seek time.
        self.silently_set_current_time(seek_time)?;

        // 2. If animation has a pending pause task, synchronously complete the pause operation by performing the
        //    following steps:
        if self.playback().has_pending_pause_task {
            // 1. Set animation’s hold time to seek time.
            self.set_hold_time(seek_time);

            // 2. Apply any pending playback rate to animation.
            self.apply_any_pending_playback_rate();

            // 3. Make animation’s start time unresolved.
            self.set_start_time(None);

            // 4. Cancel the pending pause task.
            self.set_has_pending_pause_task(false);

            // 5. Resolve animation’s current ready promise with animation.
            self.emit(HostEffectKind::ResolveReadyPromise);
        }

        // 3. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //    true, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::Yes, SynchronouslyNotify::No, Invalidate::Yes, None);

        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#set-the-start-time
    /// NB: The host validates new start time as a CSSNumberish time before it runs this procedure.
    fn set_the_start_time(&mut self, new_start_time: Option<TimeValue>) {
        // 1. Let valid start time be the result of running the validate a CSSNumberish time procedure with new start
        //    time as the input.
        // 2. If valid start time is false, abort this procedure.
        // AD-HOC: The host has already validated new start time.

        // 3. Set auto align start time to false.
        self.set_auto_align_start_time(false);

        // 4. Let timeline time be the current time value of the timeline that animation is associated with. If there
        //    is no timeline associated with animation or the associated timeline is inactive, let the timeline time be
        //    unresolved.
        let timeline_time = self.timeline_time();

        // 5. If timeline time is unresolved and new start time is resolved, make animation’s hold time unresolved.
        if timeline_time.is_none() && new_start_time.is_some() {
            self.set_hold_time(None);
        }

        // 6. Let previous current time be animation’s current time.
        let previous_current_time = self.current_time();

        // 7. Apply any pending playback rate on animation.
        self.apply_any_pending_playback_rate();

        // 8. Set animation’s start time to new start time.
        self.set_start_time(new_start_time);

        // 9. Update animation’s hold time based on the first matching condition from the following,

        // -> If new start time is resolved,
        if new_start_time.is_some() {
            // If animation’s playback rate is not zero, make animation’s hold time unresolved.
            if self.playback_rate() != 0.0 {
                self.set_hold_time(None);
            }
        }
        // -> Otherwise (new start time is unresolved),
        else {
            // Set animation’s hold time to previous current time even if previous current time is unresolved.
            self.set_hold_time(previous_current_time);
        }

        // 10. If animation has a pending play task or a pending pause task, cancel that task and resolve animation’s
        //     current ready promise with animation.
        if self.pending() {
            self.set_has_pending_play_task(false);
            self.set_has_pending_pause_task(false);
            self.emit(HostEffectKind::ResolveReadyPromise);
        }

        // 11. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //     true, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::Yes, SynchronouslyNotify::No, Invalidate::Yes, None);
    }

    /// https://www.w3.org/TR/web-animations-1/#set-the-playback-rate
    pub(crate) fn set_playback_rate(&mut self, new_playback_rate: f64) -> Result<(), AnimationError> {
        // 1. Clear any pending playback rate on animation.
        self.set_pending_playback_rate(None);

        // 2. Let previous time be the value of the current time of animation before changing the playback rate.
        let previous_time = self.current_time();

        // 3. Let previous playback rate be the current effective playback rate of animation.
        let previous_playback_rate = self.playback_rate();

        // 4. Set the playback rate to new playback rate.
        self.update_playback(|playback| playback.playback_rate = new_playback_rate);

        // 5. Perform the steps corresponding to the first matching condition from the following, if any:

        // -> If animation is associated with a monotonically increasing timeline and the previous time is resolved,
        if self.has_monotonically_increasing_timeline()
            && let Some(previous_time) = previous_time
        {
            // set the current time of animation to previous time.
            self.as_script_changing_the_play_state(|procedures| procedures.set_current_time(Some(previous_time)))?;
        }
        // -> If animation is associated with a non-null timeline that is not monotonically increasing, the start time
        //    of animation is resolved, associated effect end is not infinity, and either:
        //    - the previous playback rate < 0 and the new playback rate ≥ 0, or
        //    - the previous playback rate ≥ 0 and the new playback rate < 0,
        else if self.has_finite_timeline()
            && let Some(start_time) = self.start_time()
            && !self.associated_effect_end().value.is_infinite()
            && ((previous_playback_rate < 0.0 && new_playback_rate >= 0.0)
                || (previous_playback_rate >= 0.0 && new_playback_rate < 0.0))
        {
            // Set animation’s start time to the result of evaluating associated effect end - start time for animation.
            self.set_start_time(Some(self.associated_effect_end() - start_time));
        }

        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
    /// `observed_current_time`, where given, is the unconstrained current time.
    pub(crate) fn update_finished_state(
        &mut self,
        did_seek: DidSeek,
        synchronously_notify: SynchronouslyNotify,
        invalidate: Invalidate,
        observed_current_time: Option<TimeValue>,
    ) {
        // 1. Let the unconstrained current time be the result of calculating the current time substituting an
        //    unresolved time value for the hold time if did seek is false. If did seek is true, the unconstrained
        //    current time is equal to the current time.
        //
        // Note: This is required to accommodate timelines that can change direction. Without this definition, a once-
        //       finished animation would remain finished even when its timeline progresses in the opposite direction.
        let unconstrained_current_time = observed_current_time.or_else(|| match did_seek {
            DidSeek::No => self.current_time_without_hold_time(),
            DidSeek::Yes => self.current_time(),
        });

        // 2. If all three of the following conditions are true,
        //    - the unconstrained current time is resolved, and
        //    - animation’s start time is resolved, and
        //    - animation does not have a pending play task or a pending pause task,
        if let Some(unconstrained_current_time) = unconstrained_current_time
            && self.start_time().is_some()
            && !self.pending()
        {
            // then update animation’s hold time based on the first matching condition for animation from below, if any:
            self.update_hold_time_for_finished_state(did_seek, unconstrained_current_time);
        }

        // 3. Set the previous current time of animation to be the result of calculating its current time.
        let current_time = self.current_time();
        self.animation.borrow_mut().previous_current_time = current_time;

        // 4. Let current finished state be true if the play state of animation is finished. Otherwise, let it be false.
        let current_finished_state = self.play_state() == AnimationPlayState::Finished;

        // 5. If current finished state is true and the current finished promise is not yet resolved, perform the
        //    following steps:
        if current_finished_state && !self.playback().is_finished {
            // 1. Let finish notification steps refer to the following procedure:
            // NB: See finish_notification_steps().

            // 2. If synchronously notify is true, cancel any queued microtask to run the finish notification steps for
            //    this animation, and run the finish notification steps immediately.
            if synchronously_notify == SynchronouslyNotify::Yes {
                self.emit(HostEffectKind::CancelFinishNotificationTask);
                self.finish_notification_steps();
            }
            //    Otherwise, if synchronously notify is false, queue a microtask to run finish notification steps for
            //    animation unless there is already a microtask queued to run those steps for animation.
            else {
                self.emit(HostEffectKind::QueueFinishNotificationTask);
            }
        }

        // 6. If current finished state is false and animation’s current finished promise is already resolved, set
        //    animation’s current finished promise to a new promise in the relevant Realm of animation.
        if !current_finished_state && self.playback().is_finished {
            self.emit(HostEffectKind::ReplaceFinishedPromise);
            self.update_playback(|playback| playback.is_finished = false);
        }

        // AD-HOC: An animation disassociated from its target while inert can only be revived through here.
        if let Some(play_state_when_disassociated) = self.animation.borrow().play_state_when_disassociated_from_target
            && (self.pending() || self.is_relevant() || self.play_state() != play_state_when_disassociated)
            && let Some(element) = self.effect_target()
        {
            self.emit(HostEffectKind::AssociateWithTarget { element });
        }

        // AD-HOC: An animation that has finished without filling produces nothing more, so it leaves its target's list.
        if current_finished_state && !self.pending() && !self.is_relevant() {
            self.animation.borrow_mut().disassociation_from_target_pending = true;
        }

        if invalidate == Invalidate::Yes {
            self.invalidate_effect();
        }
    }

    /// Step 2 of https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
    fn update_hold_time_for_finished_state(&self, did_seek: DidSeek, unconstrained_current_time: TimeValue) {
        let playback_rate = self.playback_rate();
        let previous_current_time = self.animation.borrow().previous_current_time;

        // -> If playback rate > 0 and unconstrained current time is greater than or equal to associated effect end,
        let associated_effect_end = self.associated_effect_end();
        if playback_rate > 0.0 && unconstrained_current_time >= associated_effect_end {
            // If did seek is true, let the hold time be the value of unconstrained current time.
            if did_seek == DidSeek::Yes {
                self.set_hold_time(Some(unconstrained_current_time));
            }
            // If did seek is false, let the hold time be the maximum value of previous current time and associated
            // effect end. If the previous current time is unresolved, let the hold time be associated effect end.
            else if let Some(previous_current_time) = previous_current_time {
                self.set_hold_time(Some(time_value::max(previous_current_time, associated_effect_end)));
            } else {
                self.set_hold_time(Some(associated_effect_end));
            }
        }
        // -> If playback rate < 0 and unconstrained current time is less than or equal to 0,
        else if playback_rate < 0.0 && unconstrained_current_time.value <= 0.0 {
            // If did seek is true, let the hold time be the value of unconstrained current time.
            if did_seek == DidSeek::Yes {
                self.set_hold_time(Some(unconstrained_current_time));
            }
            // If did seek is false, let the hold time be the minimum value of previous current time and zero. If the
            // previous current time is unresolved, let the hold time be zero.
            else if let Some(previous_current_time) = previous_current_time {
                self.set_hold_time(Some(time_value::min(previous_current_time, self.zero())));
            } else {
                self.set_hold_time(Some(self.zero()));
            }
        }
        // -> If playback rate ≠ 0, and animation is associated with an active timeline,
        else if playback_rate != 0.0
            && let Some(timeline_time) = self.timeline_time()
        {
            // Perform the following steps:

            // 1. If did seek is true and the hold time is resolved, let animation’s start time be equal to the result
            //    of evaluating timeline time - (hold time / playback rate) where timeline time is the current time
            //    value of the timeline associated with animation.
            if did_seek == DidSeek::Yes
                && let Some(hold_time) = self.hold_time()
            {
                self.set_start_time(Some(timeline_time - (hold_time / playback_rate)));
            }

            // 2. Let the hold time be unresolved.
            self.set_hold_time(None);
        }
    }

    /// The finish notification steps of step 5 of
    /// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
    pub(crate) fn finish_notification_steps(&mut self) {
        // 1. If animation’s play state is not equal to finished, abort these steps.
        if self.play_state() != AnimationPlayState::Finished {
            return;
        }

        // 2. Resolve animation’s current finished promise object with animation.
        self.emit(HostEffectKind::ResolveFinishedPromise);
        self.update_playback(|playback| playback.is_finished = true);

        // 3. Create an AnimationPlaybackEvent, finishEvent.
        // 4. Set finishEvent’s type attribute to finish.
        // 5. Set finishEvent’s currentTime attribute to the current time of animation.
        // 6. Set finishEvent’s timelineTime attribute to the current time of the timeline with which animation is
        //    associated. If animation is not associated with a timeline, or the timeline is inactive, let timelineTime
        //    be null.
        // 7. If animation has a document for timing, then append finishEvent to its document for timing's pending
        //    animation event queue along with its target, animation. For the scheduled event time, use the result of
        //    converting animation’s associated effect end to an origin-relative time.
        //    Otherwise, queue a task to dispatch finishEvent at animation. The task source for this task is the DOM
        //    manipulation task source.
        // NB: The host creates and queues the event. The conversion of the associated effect end to an origin-relative
        //     time starts here, from the animation as it is now, by converting it to a timeline time.
        let current_time = self.current_time();
        let timeline_time = self.timeline_time();
        let scheduled_timeline_time =
            self.convert_an_animation_time_to_timeline_time(Some(self.associated_effect_end()));
        self.emit(HostEffectKind::QueuePlaybackEvent {
            r#type: PlaybackEventType::Finish,
            current_time,
            timeline_time,
            scheduled_timeline_time,
        });
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-time-to-timeline-time
    fn convert_an_animation_time_to_timeline_time(&self, time: Option<TimeValue>) -> Option<TimeValue> {
        // 1. If time is unresolved, return time.
        let time = time?;

        // 2. If time is infinity, return an unresolved time value.
        if time.value.is_infinite() {
            return None;
        }

        // 3. If animation’s playback rate is zero, return an unresolved time value.
        let playback_rate = self.playback_rate();
        if playback_rate == 0.0 {
            return None;
        }

        // 4. If animation’s start time is unresolved, return an unresolved time value.
        let start_time = self.start_time()?;

        // 5. Return the result of calculating: time × (1 / playback rate) + start time (where playback rate and start
        //    time are the playback rate and start time of animation, respectively).
        Some((time * (1.0 / playback_rate)) + start_time)
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-reset-an-animations-pending-tasks
    fn reset_an_animations_pending_tasks(&mut self) {
        // 1. If animation does not have a pending play task or a pending pause task, abort this procedure.
        if !self.pending() {
            return;
        }

        // 2. If animation has a pending play task, cancel that task.
        self.set_has_pending_play_task(false);

        // 3. If animation has a pending pause task, cancel that task.
        self.set_has_pending_pause_task(false);

        // 4. Apply any pending playback rate on animation.
        self.apply_any_pending_playback_rate();

        // 5. Reject animation’s current ready promise with a DOMException named "AbortError".
        // 6. Set the [[PromiseIsHandled]] internal slot of animation’s current ready promise to true.
        // 7. Let animation’s current ready promise be the result of creating a new resolved Promise object with value
        //    animation in the relevant Realm of animation.
        self.emit(HostEffectKind::RejectReadyPromise);
    }

    /// https://www.w3.org/TR/web-animations-1/#cancel-an-animation
    fn cancel_an_animation(&mut self, invalidate: Invalidate) {
        // 1. If animation’s play state is not idle, perform the following steps:
        if self.play_state() != AnimationPlayState::Idle {
            // 1. Run the procedure to reset an animation’s pending tasks on animation.
            self.reset_an_animations_pending_tasks();

            // 2. Reject the current finished promise with a DOMException named "AbortError".
            // 3. Set the [[PromiseIsHandled]] internal slot of the current finished promise to true.
            // 4. Let current finished promise be a new promise in the relevant Realm of animation.
            self.emit(HostEffectKind::RejectFinishedPromise);
            self.update_playback(|playback| playback.is_finished = false);

            // 5. Create an AnimationPlaybackEvent, cancelEvent.
            // 6. Set cancelEvent’s type attribute to cancel.
            // 7. Set cancelEvent’s currentTime to null.
            // 8. Let timeline time be the current time of the timeline with which animation is associated. If
            //    animation is not associated with an active timeline, let timeline time be an unresolved time value.
            // 9. Set cancelEvent’s timelineTime to timeline time. If timeline time is unresolved, set it to null.
            // 10. If animation has a document for timing, then append cancelEvent to its document for timing's pending
            //     animation event queue along with its target, animation. If animation is associated with an active
            //     timeline that defines a procedure to convert timeline times to origin-relative time, let the
            //     scheduled event time be the result of applying that procedure to timeline time. Otherwise, the
            //     scheduled event time is an unresolved time value.
            //     Otherwise, queue a task to dispatch cancelEvent at animation. The task source for this task is the
            //     DOM manipulation task source.
            // NB: The host creates and queues the event.
            let timeline_time = self.timeline_time();
            self.emit(HostEffectKind::QueuePlaybackEvent {
                r#type: PlaybackEventType::Cancel,
                current_time: None,
                timeline_time,
                scheduled_timeline_time: timeline_time,
            });
        }

        // 2. Make animation’s hold time unresolved.
        self.set_hold_time(None);

        // 3. Make animation’s start time unresolved.
        self.set_start_time(None);

        // NB: This time is needed for dispatching the animationcancel DOM event.
        if let Some(timing) = self.effect_timing() {
            let timing = AnimationEffectTiming {
                fill_mode: FillMode::Both,
                ..timing
            };
            let playback = self.playback();
            let active_time = timing::resolve_timing(
                &timing,
                self.current_time(),
                Some(&playback),
                self.timeline.as_ref().map(|timeline| &timeline.sample),
            )
            .active_time;
            self.animation.borrow_mut().saved_cancel_time = active_time;
        }

        // AD-HOC: The target's animation list is our own cache of the animations whose effect targets it, and holding a
        //         canceled animation there keeps it alive and walked every frame. It is reassociated if revived.
        self.animation.borrow_mut().disassociation_from_target_pending = true;

        if invalidate == Invalidate::Yes {
            self.invalidate_effect();
        }
    }

    /// https://www.w3.org/TR/web-animations-1/#finish-an-animation
    pub(crate) fn finish_an_animation(&mut self) -> Result<(), AnimationError> {
        // 1. If animation’s effective playback rate is zero, or if animation’s effective playback rate > 0 and
        //    associated effect end is infinity, throw an "InvalidStateError" DOMException and abort these steps.
        let effective_playback_rate = self.playback().effective_playback_rate();
        if effective_playback_rate == 0.0 {
            return Err(AnimationError::InvalidState(
                "Animation with a playback rate of 0 cannot be finished",
            ));
        }
        if effective_playback_rate > 0.0 && self.associated_effect_end().value.is_infinite() {
            return Err(AnimationError::InvalidState("Animation with no end cannot be finished"));
        }

        // 2. Apply any pending playback rate to animation.
        self.apply_any_pending_playback_rate();

        // 3. Set limit as follows:
        //    -> If playback rate > 0,
        //       Let limit be associated effect end.
        //    -> Otherwise,
        //       Let limit be zero.
        let playback_rate = self.playback_rate();
        let limit = if playback_rate > 0.0 {
            self.associated_effect_end()
        } else {
            self.zero()
        };

        // 4. Silently set the current time to limit.
        self.silently_set_current_time(Some(limit))?;

        // 5. If animation’s start time is unresolved and animation has an associated active timeline, let the start
        //    time be the result of evaluating timeline time - (limit / playback rate) where timeline time is the
        //    current time value of the associated timeline.
        if self.start_time().is_none()
            && let Some(timeline_time) = self.timeline_time()
        {
            self.set_start_time(Some(timeline_time - (limit / playback_rate)));
        }

        // 6. If there is a pending pause task and start time is resolved,
        let mut should_resolve_ready_promise = false;
        if self.playback().has_pending_pause_task && self.start_time().is_some() {
            // 1. Let the hold time be unresolved.
            // Note: Typically the hold time will already be unresolved except in the case when the animation was
            //       previously idle.
            self.set_hold_time(None);

            // 2. Cancel the pending pause task.
            self.set_has_pending_pause_task(false);

            // 3. Resolve the current ready promise of animation with animation.
            should_resolve_ready_promise = true;
        }

        // 7. If there is a pending play task and start time is resolved, cancel that task and resolve the current ready
        //    promise of animation with animation.
        if self.playback().has_pending_play_task && self.start_time().is_some() {
            self.set_has_pending_play_task(false);
            should_resolve_ready_promise = true;
        }

        if should_resolve_ready_promise {
            self.emit(HostEffectKind::ResolveReadyPromise);
        }

        // 8. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //    true, and the synchronously notify flag set to true.
        self.update_finished_state(DidSeek::Yes, SynchronouslyNotify::Yes, Invalidate::Yes, None);

        Ok(())
    }

    /// https://drafts.csswg.org/web-animations-1/#playing-an-animation-section
    /// https://drafts.csswg.org/web-animations-2/#play-an-animation
    pub(crate) fn play_an_animation(
        &mut self,
        auto_rewind: AutoRewind,
        invalidate: Invalidate,
    ) -> Result<(), AnimationError> {
        // 1. Let aborted pause be a boolean flag that is true if animation has a pending pause task, and false
        //    otherwise.
        let aborted_pause = self.playback().has_pending_pause_task;

        // 2. Let has pending ready promise be a boolean flag that is initially false.
        let mut has_pending_ready_promise = false;

        // 3. Let has finite timeline be true if animation has an associated timeline that is not monotonically
        //    increasing.
        let has_finite_timeline = self.has_finite_timeline();

        // 4. Let previous current time be the animation’s current time
        let previous_current_time = self.current_time();

        // 5. Let enable seek be true if the auto-rewind flag is true and has finite timeline is false. Otherwise,
        //    initialize to false.
        let enable_seek = auto_rewind == AutoRewind::Yes && !has_finite_timeline;

        // 6. Perform the steps corresponding to the first matching condition from the following, if any:
        let effective_playback_rate = self.playback().effective_playback_rate();
        let associated_effect_end = self.associated_effect_end();

        // -> If animation’s effective playback rate > 0, enable seek is true and either animation’s:
        //    - previous current time is unresolved, or
        //    - previous current time < zero, or
        //    - previous current time ≥ associated effect end,
        if effective_playback_rate > 0.0
            && enable_seek
            && previous_current_time.is_none_or(|previous| previous.value < 0.0 || previous >= associated_effect_end)
        {
            // Set the animation’s hold time to zero.
            self.set_hold_time(Some(self.zero()));
        }
        // -> If animation’s effective playback rate < 0, enable seek is true and either animation’s:
        //    - previous current time is unresolved, or
        //    - previous current time ≤ zero, or
        //    - previous current time > associated effect end,
        else if effective_playback_rate < 0.0
            && enable_seek
            && previous_current_time.is_none_or(|previous| previous.value <= 0.0 || previous > associated_effect_end)
        {
            // -> If associated effect end is positive infinity,
            if associated_effect_end.value == f64::INFINITY {
                // throw an "InvalidStateError" DOMException and abort these steps.
                return Err(AnimationError::InvalidState(
                    "Cannot rewind an animation with an infinite effect end",
                ));
            }

            // -> Otherwise,
            //    Set the animation’s hold time to the animation’s associated effect end.
            self.set_hold_time(Some(associated_effect_end));
        }
        // -> If animation’s effective playback rate = 0 and animation’s current time is unresolved,
        else if effective_playback_rate == 0.0 && previous_current_time.is_none() {
            // Set the animation’s hold time to zero.
            self.set_hold_time(Some(self.zero()));
        }

        // 7. If has finite timeline and auto-rewind is true:
        //    1. Set the flag auto align start time to true.
        //    2. Set the animation’s hold time to previous current time.
        // AD-HOC: The flag is set when previous current time is unresolved, whatever auto-rewind is, and the hold time
        //         is left as it is.
        if has_finite_timeline && previous_current_time.is_none() {
            self.set_auto_align_start_time(true);
        }

        // 8. If animation’s hold time is resolved, let its start time be unresolved.
        if self.hold_time().is_some() {
            self.set_start_time(None);
        }

        // 9. If animation has a pending play task or a pending pause task,
        if self.pending() {
            // 1. Cancel that task.
            self.set_has_pending_play_task(false);
            self.set_has_pending_pause_task(false);

            // 2. Set has pending ready promise to true.
            has_pending_ready_promise = true;
        }

        // 10. If the following three conditions are all satisfied:
        //     - animation’s hold time is unresolved, and
        //     - aborted pause is false, and
        //     - animation does not have a pending playback rate,
        // AD-HOC: We also don't abort if we have a pending auto-alignment of the start time, see
        //         https://github.com/w3c/csswg-drafts/issues/13236
        let pending_auto_aligned_start_time =
            self.animation.borrow().auto_align_start_time && self.start_time().is_none();
        if self.hold_time().is_none()
            && !aborted_pause
            && self.pending_playback_rate().is_none()
            && !pending_auto_aligned_start_time
        {
            // abort this procedure.
            return Ok(());
        }

        // 11. If has pending ready promise is false, let animation’s current ready promise be a new promise in the
        //     relevant Realm of animation.
        if !has_pending_ready_promise {
            self.emit(HostEffectKind::ReplaceReadyPromise);
        }

        // 12. Schedule a task to run as soon as animation is ready. The task shall perform the following steps:
        //
        //     NB: The steps of the task are in run_pending_play_task().
        //
        //     So long as the above task is scheduled but has yet to run, animation is described as having a pending
        //     play task. While the task is running, however, animation does not have a pending play task.
        //
        //     If a user agent determines that animation is immediately ready, it may schedule the above task as a
        //     microtask such that it runs at the next microtask checkpoint, but it must not perform the task
        //     synchronously.
        self.set_has_pending_play_task(true);

        // 13. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //     false, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, invalidate, None);

        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#pause-an-animation
    fn pause_an_animation(&mut self) -> Result<(), AnimationError> {
        // 1. If animation has a pending pause task, abort these steps.
        if self.playback().has_pending_pause_task {
            return Ok(());
        }

        // 2. If the play state of animation is paused, abort these steps.
        if self.play_state() == AnimationPlayState::Paused {
            return Ok(());
        }

        // 3. Let seek time be a time value that is initially unresolved.
        let mut seek_time = None;

        // 4. Let has finite timeline be true if animation has an associated timeline that is not monotonically
        //    increasing.
        let has_finite_timeline = self.has_finite_timeline();

        // 5. If the animation’s current time is unresolved, perform the steps according to the first matching
        //    condition below:
        if self.current_time().is_none() {
            // -> If animation’s playback rate is ≥ 0,
            if self.playback_rate() >= 0.0 {
                // Set seek time to zero.
                seek_time = Some(self.zero());
            }
            // -> Otherwise
            else {
                // If associated effect end for animation is positive infinity,
                let associated_effect_end = self.associated_effect_end();
                if associated_effect_end.value == f64::INFINITY {
                    // throw an "InvalidStateError" DOMException and abort these steps.
                    return Err(AnimationError::InvalidState(
                        "Cannot pause an animation with an infinite effect end",
                    ));
                }

                // Otherwise,
                //     Set seek time to animation’s associated effect end.
                seek_time = Some(associated_effect_end);
            }
        }

        // 6. If seek time is resolved,
        if let Some(seek_time) = seek_time {
            // If has finite timeline is true,
            if has_finite_timeline {
                // Set animation’s start time to seek time.
                self.set_start_time(Some(seek_time));
            }
            // Otherwise,
            else {
                // Set animation’s hold time to seek time.
                self.set_hold_time(Some(seek_time));
            }
        }

        // 7. Let has pending ready promise be a boolean flag that is initially false.
        let mut has_pending_ready_promise = false;

        // 8. If animation has a pending play task, cancel that task and let has pending ready promise be true.
        if self.playback().has_pending_play_task {
            self.set_has_pending_play_task(false);
            has_pending_ready_promise = true;
        }

        // 9. If has pending ready promise is false, set animation’s current ready promise to a new promise in the
        //    relevant Realm of animation.
        if !has_pending_ready_promise {
            self.emit(HostEffectKind::ReplaceReadyPromise);
        }

        // 10. Schedule a task to be executed at the first possible moment where both of the following conditions are
        //     true:
        // NB: is_ready_to_run_pending_pause_task() checks these conditions, and run_pending_pause_task() is the task.
        self.set_has_pending_pause_task(true);

        // 11. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //     false, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, Invalidate::Yes, None);

        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#seamlessly-update-the-playback-rate
    pub(crate) fn update_playback_rate(&mut self, new_playback_rate: f64) -> Result<(), AnimationError> {
        // 1. Let previous play state be animation’s play state.
        // Note: It is necessary to record the play state before updating animation’s effective playback rate since,
        //       in the following logic, we want to immediately apply the pending playback rate of animation if it is
        //       currently finished regardless of whether or not it will still be finished after we apply the pending
        //       playback rate.
        let previous_play_state = self.play_state();

        // 2. Let animation’s pending playback rate be new playback rate.
        self.set_pending_playback_rate(Some(new_playback_rate));

        // 3. Perform the steps corresponding to the first matching condition from below:

        // -> If animation has a pending play task or a pending pause task,
        if self.pending() {
            // Abort these steps.
            // Note: The different types of pending tasks will apply the pending playback rate when they run so there
            //       is no further action required in this case.
            return Ok(());
        }

        // -> If previous play state is idle or paused, or animation’s current time is unresolved,
        if matches!(
            previous_play_state,
            AnimationPlayState::Idle | AnimationPlayState::Paused
        ) || self.current_time().is_none()
        {
            // Apply any pending playback rate on animation.
            // Note: the second condition above is required so that if we have a running animation with an unresolved
            //       current time and no pending play task, we do not attempt to play it below.
            self.apply_any_pending_playback_rate();
        }
        // -> If previous play state is finished,
        else if previous_play_state == AnimationPlayState::Finished {
            // 1. Let the unconstrained current time be the result of calculating the current time of animation
            //    substituting an unresolved time value for the hold time.
            let unconstrained_current_time = self.current_time_without_hold_time();

            // 2. Let animation’s start time be the result of evaluating the following expression:
            //        timeline time - (unconstrained current time / pending playback rate)
            //    Where timeline time is the current time value of the timeline associated with animation.
            //    If pending playback rate is zero, let animation’s start time be timeline time.
            let timeline_time = self.timeline_time();
            if new_playback_rate == 0.0 {
                self.set_start_time(timeline_time);
            }
            // INTEROP: The timeline time is unresolved where the timeline became inactive after the animation
            //          finished, as a scroll timeline whose scroller stopped overflowing, while the hold time keeps
            //          the current time resolved. Like other browsers, leave the start time as it is then.
            else if let Some(timeline_time) = timeline_time {
                let unconstrained_current_time = unconstrained_current_time
                    .expect("a finished animation on an active timeline has a resolved unconstrained current time");
                self.set_start_time(Some(timeline_time - (unconstrained_current_time / new_playback_rate)));
            }

            // 3. Apply any pending playback rate on animation.
            self.apply_any_pending_playback_rate();

            // 4. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
            //    false, and the synchronously notify flag set to false.
            self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, Invalidate::Yes, None);
        }
        // -> Otherwise,
        else {
            // Run the procedure to play an animation for animation with the auto-rewind flag set to false.
            self.play_an_animation(AutoRewind::No, Invalidate::Yes)?;
        }

        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#reverse-an-animation
    fn reverse_an_animation(&mut self) -> Result<(), AnimationError> {
        // 1. If there is no timeline associated with animation, or the associated timeline is inactive throw an
        //    "InvalidStateError" DOMException and abort these steps.
        if !self.has_active_timeline() {
            return Err(AnimationError::InvalidState(
                "Cannot reverse an animation with an inactive timeline",
            ));
        }

        // 2. Let original pending playback rate be animation’s pending playback rate.
        let original_pending_playback_rate = self.pending_playback_rate();

        // 3. Let animation’s pending playback rate be the additive inverse of its effective playback rate (i.e.
        //    -effective playback rate).
        self.set_pending_playback_rate(Some(-self.playback().effective_playback_rate()));

        // 4. Run the steps to play an animation for animation with the auto-rewind flag set to true.
        //    If the steps to play an animation throw an exception, set animation’s pending playback rate to original
        //    pending playback rate and propagate the exception.
        if let Err(error) = self.play_an_animation(AutoRewind::Yes, Invalidate::Yes) {
            self.set_pending_playback_rate(original_pending_playback_rate);
            return Err(error);
        }

        Ok(())
    }

    /// https://drafts.csswg.org/web-animations-2/#ready
    fn is_ready(&self) -> bool {
        // An animation is ready at the first moment where all of the following conditions are true:

        // FIXME: - the user agent has completed any setup required to begin the playback of each inclusive descendant
        //          of the animation’s associated effect including rendering the first frame of any keyframe effect or
        //          executing any custom effects associated with an animation effect

        // - the animation is associated with a timeline that is not inactive.
        if !self.has_active_timeline() {
            return false;
        }

        // - the animation’s hold time or start time is resolved.
        self.hold_time().is_some() || self.start_time().is_some()
    }

    /// Runs the pending play task of the animation if it has one and is ready.
    pub(crate) fn run_pending_play_task_if_ready(&mut self) {
        if !self.playback().has_pending_play_task || !self.is_ready() {
            return;
        }
        self.set_has_pending_play_task(false);
        self.run_pending_play_task();
    }

    /// The time a new CSS transition became ready at, if it runs on the compositor from the time of its style change
    /// event.
    fn compositor_transition_ready_time(&self) -> Option<TimeValue> {
        let animation = self.animation.borrow();
        let AnimationKind::CssTransition(transition) = animation.kind() else {
            return None;
        };
        let may_run_on_the_compositor = animation.effect()?.borrow().may_run_on_the_compositor();
        if !may_run_on_the_compositor || transition.previous_phase != TransitionPhase::Idle {
            return None;
        }
        Some(TimeValue::milliseconds(
            animation.css_transition_style_change_event_time(),
        ))
    }

    /// Whether the animation is a CSS transition a compositor animation runs.
    fn is_compositor_driven_transition(&self) -> bool {
        let animation = self.animation.borrow();
        matches!(animation.kind(), AnimationKind::CssTransition(_))
            && animation
                .effect()
                .is_some_and(|effect| effect.borrow().has_flag(EffectFlag::CompositorDriven))
    }

    /// Step 12 of https://www.w3.org/TR/web-animations-1/#playing-an-animation-section
    fn run_pending_play_task(&mut self) {
        // 1. Assert that at least one of animation’s start time or hold time is resolved.
        assert!(self.start_time().is_some() || self.hold_time().is_some());

        // 2. Let ready time be the time value of the timeline associated with animation at the moment when animation
        //    became ready.
        // FIXME: We can get a more accurate time here if we record the actual instant we became ready rather than
        //        waiting to try and run this task
        // AD-HOC: A new CSS transition of properties the compositor may run stays pending past the rendering update of
        //         its style change event, which already rendered it, on the compositor or here. It became ready at the
        //         time of its style change event, from which the compositor runs it, so that a style change a frame
        //         later starts the next transition from the value on the screen rather than from this transition's
        //         start value.
        let ready_time = self.compositor_transition_ready_time().unwrap_or_else(|| {
            self.timeline_time()
                .expect("an animation is ready only on an active timeline")
        });

        // 3. Perform the steps corresponding to the first matching condition below, if any:

        // -> If animation’s hold time is resolved,
        if let Some(hold_time) = self.hold_time() {
            // 1. Apply any pending playback rate on animation.
            self.apply_any_pending_playback_rate();

            // 2. Let new start time be the result of evaluating ready time - hold time / playback rate for animation.
            //    If the playback rate is zero, let new start time be simply ready time.
            let playback_rate = self.playback_rate();
            let new_start_time = if playback_rate != 0.0 {
                ready_time - (hold_time / playback_rate)
            } else {
                ready_time
            };

            // 3. Set the start time of animation to new start time.
            self.set_start_time(Some(new_start_time));

            // 4. If animation’s playback rate is not 0, make animation’s hold time unresolved.
            if playback_rate != 0.0 {
                self.set_hold_time(None);
            }
        }
        // -> If animation’s start time is resolved and animation has a pending playback rate,
        else if let Some(start_time) = self.start_time()
            && self.pending_playback_rate().is_some()
        {
            // 1. Let current time to match be the result of evaluating (ready time - start time) × playback rate for
            //    animation.
            let current_time_to_match = (ready_time - start_time) * self.playback_rate();

            // 2. Apply any pending playback rate on animation.
            self.apply_any_pending_playback_rate();

            // 3. If animation’s playback rate is zero, let animation’s hold time be current time to match.
            let playback_rate = self.playback_rate();
            if playback_rate == 0.0 {
                self.set_hold_time(Some(current_time_to_match));
            }

            // 4. Let new start time be the result of evaluating ready time - current time to match / playback rate for
            //    animation. If the playback rate is zero, let new start time be simply ready time.
            let new_start_time = if playback_rate != 0.0 {
                ready_time - (current_time_to_match / playback_rate)
            } else {
                ready_time
            };

            // 5. Set the start time of animation to new start time.
            self.set_start_time(Some(new_start_time));
        }

        // 4. Resolve animation’s current ready promise with animation.
        self.emit(HostEffectKind::ResolveReadyPromise);

        // 5. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //    false, and the synchronously notify flag set to false.
        // OPTIMIZATION: A new CSS transition can already have a provisional compositor descriptor anchored to its style
        //               change event time. Keep that descriptor so resolving this task does not restart its visual
        //               time.
        let invalidate = if self.is_compositor_driven_transition() {
            Invalidate::No
        } else {
            Invalidate::Yes
        };
        self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, invalidate, None);

        // AD-HOC: A pending animation is not handed to the compositor, and the frame that hands it over once it is no
        //         longer pending is recorded only if something needs a repaint. Its output may not change, so ask for
        //         one.
        if self.effect_has_target() {
            self.emit(HostEffectKind::RequestRepaint);
        }
    }

    /// Whether the conditions hold at which step 10 of https://www.w3.org/TR/web-animations-1/#pause-an-animation
    /// schedules the pending pause task to run.
    fn is_ready_to_run_pending_pause_task(&self) -> bool {
        // https://www.w3.org/TR/web-animations-1/#pause-an-animation
        // https://drafts.csswg.org/web-animations-2/#pausing-an-animation-section
        // FIXME: - the user agent has performed any processing necessary to suspend the playback of animation’s
        //          associated effect, if any.

        // - the animation is associated with a timeline that is not inactive.
        if !self.has_active_timeline() {
            return false;
        }

        // - the animation has a resolved hold time or start time.
        self.hold_time().is_some() || self.start_time().is_some()
    }

    /// Step 10 of https://www.w3.org/TR/web-animations-1/#pause-an-animation
    fn run_pending_pause_task(&mut self) {
        // 1. Let ready time be the time value of the timeline associated with animation at the moment when the user
        //    agent completed processing necessary to suspend playback of animation’s associated effect.
        // FIXME: We can get a more accurate time here if we record the actual instant the above is true rather than
        //        waiting for this task to run
        let ready_time = self
            .timeline_time()
            .expect("a pending pause task runs only on an active timeline");

        // 2. If animation’s start time is resolved and its hold time is not resolved, let animation’s hold time be the
        //    result of evaluating (ready time - start time) × playback rate.
        // Note: The hold time might be already set if the animation is finished, or if the animation has a pending
        //       play task. In either case we want to preserve the hold time as we enter the paused state.
        if let Some(start_time) = self.start_time()
            && self.hold_time().is_none()
        {
            self.set_hold_time(Some((ready_time - start_time) * self.playback_rate()));
        }

        // 3. Apply any pending playback rate on animation.
        self.apply_any_pending_playback_rate();

        // 4. Make animation’s start time unresolved.
        self.set_start_time(None);

        // 5. Resolve animation’s current ready promise with animation.
        self.emit(HostEffectKind::ResolveReadyPromise);

        // 6. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //    false, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, Invalidate::Yes, None);
    }

    /// Normalizes the timing of the associated effect against the attachment interval where it moved.
    /// https://drafts.csswg.org/web-animations-2/#auto-aligning-start-time
    /// NB: The effect's timing is normalized against the attachment interval, which moves with the timeline's scroll
    ///     container and subject. That holds whether or not the start time is auto-aligned, e.g. for a paused
    ///     animation.
    fn normalize_timing_against_attachment_interval(&mut self) {
        let Some(interval) = self.timeline.as_ref().and_then(TimelineState::attachment_interval) else {
            return;
        };
        if self.animation.borrow().normalized_attachment_interval == Some(interval) {
            return;
        }
        self.normalize_effect_timing();
        self.invalidate_effect();
    }

    /// Notes the attachment interval of the timeline and normalizes the specified timing of the associated effect
    /// against it.
    fn normalize_effect_timing(&self) {
        let interval = self.timeline.as_ref().and_then(TimelineState::attachment_interval);
        let mut animation = self.animation.borrow_mut();
        animation.normalized_attachment_interval = interval;
        if let Some(effect) = animation.effect() {
            effect.borrow_mut().normalize_specified_timing(self.timeline.as_ref());
        }
    }

    /// https://drafts.csswg.org/web-animations-2/#auto-aligning-start-time
    fn calculate_auto_aligned_start_time(&mut self) {
        assert!(
            self.timeline
                .as_ref()
                .is_some_and(|timeline| timeline.is_progress_based)
        );

        // 1. If the auto-align start time flag is false, abort this procedure.
        if !self.animation.borrow().auto_align_start_time {
            return;
        }

        // 2. If the timeline is inactive, abort this procedure.
        if !self.has_active_timeline() {
            return;
        }

        // 3. If play state is idle, abort this procedure.
        let play_state = self.play_state();
        if play_state == AnimationPlayState::Idle {
            return;
        }

        // 4. If play state is paused, and hold time is resolved, abort this procedure.
        if play_state == AnimationPlayState::Paused && self.hold_time().is_some() {
            return;
        }

        self.normalize_timing_against_attachment_interval();
        let interval = self
            .timeline
            .as_ref()
            .and_then(TimelineState::attachment_interval)
            .expect("a progress-based timeline has an attachment interval");

        // 5. Let start offset be the resolved timeline time corresponding to the start of the animation attachment
        //    range. In the case of view timelines, it requires a calculation based on the proportion of the cover
        //    range.
        let start_offset = interval.start;

        // 6. Let end offset be the resolved timeline time corresponding to the end of the animation attachment range.
        //    In the case of view timelines, it requires a calculation based on the proportion of the cover range.
        let end_offset = interval.end;

        // 7. Set start time to start offset if effective playback rate ≥ 0, and end offset otherwise.
        if self.playback().effective_playback_rate() >= 0.0 {
            self.set_start_time(Some(start_offset));
        } else {
            self.set_start_time(Some(end_offset));
        }

        // 8. Clear hold time.
        self.set_hold_time(None);
    }

    /// Takes in a change of the attachment range of the animation on its progress-based timeline.
    /// https://drafts.csswg.org/scroll-animations-1/#animation-attachment-range
    pub(crate) fn attachment_range_changed(&mut self) {
        if !self
            .timeline
            .as_ref()
            .is_some_and(|timeline| timeline.is_progress_based)
        {
            return;
        }
        self.normalize_effect_timing();
        self.calculate_auto_aligned_start_time();
        self.invalidate_effect();
    }

    /// Updates the animation as its timeline's current time was updated.
    /// https://drafts.csswg.org/web-animations-1/#animation-frame-loop
    pub(crate) fn update(&mut self) {
        let timeline = self.timeline.expect("an animation updates as its timeline does");

        // https://drafts.csswg.org/scroll-animations-1/#event-loop
        // When updating timeline current time, the start time of any attached animation is conditionally updated. For
        // each attached animation, run the procedure for calculating an auto-aligned start time.
        if timeline.is_progress_based {
            self.normalize_timing_against_attachment_interval();
            self.calculate_auto_aligned_start_time();
        }

        let is_monotonically_increasing = timeline.sample.is_monotonically_increasing;

        // NB: Prevent unnecessary work if the animation is already finished and can't exit the finished state due to
        //     timeline changes.
        let was_finished = self.playback().is_finished;
        if !was_finished || !is_monotonically_increasing {
            self.update_finished_state(DidSeek::No, SynchronouslyNotify::Yes, Invalidate::No, None);
            if self.play_state() == AnimationPlayState::Running && self.animation.borrow().effect().is_some() {
                let output_is_constant_before_active_start = !self.pending()
                    && self.effect_phase() == Some(AnimationEffectPhase::Before)
                    && is_monotonically_increasing
                    && self.playback_rate() > 0.0;
                let invalidate = !self.pending() && !output_is_constant_before_active_start;
                self.emit(HostEffectKind::InvalidateUnlessThrottled { invalidate });
            } else if self.playback().is_finished != was_finished && self.animation.borrow().effect().is_some() {
                self.emit(HostEffectKind::InvalidateEffect { changes_plan: false });
            }
        }

        // NB: Act on the pending play or pause task.
        self.run_pending_play_task_if_ready();

        if self.playback().has_pending_pause_task && self.is_ready_to_run_pending_pause_task() {
            self.set_has_pending_pause_task(false);
            self.run_pending_pause_task();
        }

        if self.play_state() == AnimationPlayState::Running
            && !self.pending()
            && self.effect_phase() == Some(AnimationEffectPhase::Before)
            && is_monotonically_increasing
            && self.playback_rate() > 0.0
            && let Some(timing) = self.effect_timing()
            && timing.start_delay.r#type == TimeValueType::Milliseconds
            && let Some(current_time) = self.current_time()
            && current_time.r#type == TimeValueType::Milliseconds
        {
            let delay = (timing.start_delay.value - current_time.value) / self.playback_rate();
            if delay > 0.0 && self.effect_has_target() {
                self.emit(HostEffectKind::ScheduleWakeup { delay });
            }
        }
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-set-the-timeline-of-an-animation
    /// https://drafts.csswg.org/web-animations-2/#setting-the-timeline
    pub(crate) fn set_timeline(&mut self, new_timeline: Option<TimelineHandle>) {
        // 1. Let old timeline be the current timeline of animation, if any.
        let old_timeline = self.animation.borrow().timeline().cloned();

        // 2. If new timeline is the same object as old timeline, abort this procedure.
        let same = match (&new_timeline, &old_timeline) {
            (Some(new_timeline), Some(old_timeline)) => new_timeline.ptr_eq(old_timeline),
            (None, None) => true,
            _ => false,
        };
        if same {
            return;
        }

        // NB: The procedures below read the old timeline through `old` and the new one through `self`.
        let mut no_effects = HostEffects::default();
        let old = Procedures {
            animation: self.animation,
            timeline: self.timeline,
            effects: &mut no_effects,
        };
        let range = self.animation.borrow().range();
        self.timeline = new_timeline.as_ref().map(|timeline| timeline.state_for(&range));

        // 3. Let previous play state be animation’s play state.
        let previous_play_state = old.play_state();

        // 4. Let previous current time be the animation’s current time.
        let previous_current_time = old.current_time();

        // 5. Set previous progress based in the first condition that applies:
        // AD-HOC: The spec doesn't say what to do if we have no effect so we just assume an end time of 0
        let end_time = old.associated_effect_end();
        let previous_progress = previous_current_time.map(|previous_current_time| {
            // If previous current time is unresolved:
            // Set previous progress to unresolved.

            // If end time is zero:
            // Set previous progress to zero.
            if end_time.value == 0.0 {
                return 0.0;
            }

            // Otherwise
            // Set previous progress = previous current time / end time
            previous_current_time / end_time
        });

        // 6. Let from finite timeline be true if old timeline is not null and not monotonically increasing.
        let from_finite_timeline = old.has_finite_timeline();

        // 7. Let to finite timeline be true if timeline is not null and not monotonically increasing.
        let to_finite_timeline = self.has_finite_timeline();

        // 8. Let the timeline of animation be new timeline.
        if let Some(old_timeline) = &old_timeline {
            old_timeline.associated_animations().borrow_mut().remove(self.animation);
            // NB: Nothing the old timeline updates is left of the animation for it to update.
            old_timeline
                .animations_needing_update()
                .borrow_mut()
                .remove(self.animation);
        }
        if let Some(new_timeline) = &new_timeline {
            new_timeline.associated_animations().borrow_mut().insert(self.animation);
        }
        self.animation.borrow_mut().replace_timeline(new_timeline);
        let new_timeline = self.timeline;

        // AD-HOC: The normalization of the specified timing of the associated effect depends on the associated
        //         timeline. This must be done before setting the current time below to ensure consistent units.
        self.normalize_effect_timing();

        let previous_progress_multiplied_by_end_time = |procedures: &Self, previous_progress: f64| {
            // AD-HOC: The spec doesn't say what to do if we have no effect so we just assume an end time of 0
            match procedures.effect_timing() {
                None => zero_time(new_timeline.as_ref()),
                Some(timing) => TimeValue::new(
                    super::timeline_state::time_unit(new_timeline.as_ref()),
                    timing::end_time(&timing).value * previous_progress,
                ),
            }
        };

        // 9. Perform the steps corresponding to the first matching condition from the following, if any:
        // If to finite timeline,
        if to_finite_timeline {
            // 1. Apply any pending playback rate on animation
            self.apply_any_pending_playback_rate();

            // 2. set auto align start time to true.
            self.set_auto_align_start_time(true);

            // 3. Set start time to unresolved.
            self.set_start_time(None);

            // 4. Set hold time to unresolved.
            self.set_hold_time(None);

            // 5. If previous play state is "finished" or "running"
            if matches!(
                previous_play_state,
                AnimationPlayState::Finished | AnimationPlayState::Running
            ) {
                // 1. Schedule a pending play task
                self.set_has_pending_play_task(true);
            }

            // 6. If previous play state is "paused" and previous progress is resolved:
            if previous_play_state == AnimationPlayState::Paused
                && let Some(previous_progress) = previous_progress
            {
                // 1. Set hold time to previous progress * end time.
                self.set_hold_time(Some(previous_progress_multiplied_by_end_time(self, previous_progress)));
            }
            // NOTE: This step ensures that previous progress is preserved even in the case of a pause-pending
            //       animation with a resolved start time.
        }
        // If from finite timeline and previous progress is resolved,
        else if from_finite_timeline && let Some(previous_progress) = previous_progress {
            // Run the procedure to set the current time to previous progress * end time.
            // NB: The time is valid, so setting it cannot throw. This is the procedure itself, not script setting the
            //     current time of a CSS animation. Until it is done, the start time is still in the units of the old
            //     timeline.
            let time = previous_progress_multiplied_by_end_time(self, previous_progress);
            self.set_current_time(Some(time))
                .expect("setting the current time to a resolved time does not throw");
        }

        // 10. If the start time of animation is resolved, make animation’s hold time unresolved.
        if let Some(start_time) = self.start_time() {
            // AD-HOC: A finished animation holds a time beside its start time, and setting its current time as it
            //         leaves a finite timeline sets the hold time alone, which leaves the start time in the units of
            //         the old timeline. See https://github.com/w3c/csswg-drafts/issues/11761. Like WebKit, the start
            //         time takes the hold time.
            if let Some(hold_time) = self.hold_time()
                && hold_time.r#type != start_time.r#type
            {
                self.set_start_time(Some(hold_time));
            }
            // AD-HOC: Nor does anything convert a start time left from the old timeline, as one script set while the
            //         old timeline was inactive and the previous progress was unresolved. A time in units the new
            //         timeline does not have means nothing on it, so the start time becomes unresolved.
            else if start_time.r#type != super::timeline_state::time_unit(new_timeline.as_ref()) {
                self.set_start_time(None);
            }
            self.set_hold_time(None);
        }

        // 11. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //     false, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, Invalidate::Yes, None);
    }

    /// https://www.w3.org/TR/web-animations-1/#setting-the-associated-effect
    pub(crate) fn set_the_associated_effect(&mut self, new_effect: Option<&EffectHandle>, invalidate: Invalidate) {
        // 1. Let old effect be the current associated effect of animation, if any.
        let old_effect = self.animation.borrow().effect().cloned();

        // 2. If new effect is the same object as old effect, abort this procedure.
        if new_effect == old_effect.as_ref() {
            return;
        }

        // 3. If animation has a pending pause task, reschedule that task to run as soon as animation is ready.
        // 4. If animation has a pending play task, reschedule that task to run as soon as animation is ready to play
        //    new effect.
        // NB: There is no real difference between "pending" and "as soon as possible", so these steps are no-ops.

        // 5. If new effect is not null and if new effect is the associated effect of another animation, previous
        //    animation, run the procedure to set the associated effect of an animation (this procedure) on previous
        //    animation passing null as new effect.
        let previous_animation = new_effect.and_then(|effect| effect.borrow().associated_animation());
        if let Some(previous_animation) = previous_animation
            && !previous_animation.ptr_eq(self.animation)
        {
            let timeline = previous_animation.borrow().timeline_state();
            Procedures {
                animation: &previous_animation,
                timeline,
                effects: &mut *self.effects,
            }
            .set_the_associated_effect(None, Invalidate::Yes);
            note_animation_may_need_update(&previous_animation);
        }

        // 6. Let the associated effect of animation be new effect.
        let old_target = self.effect_target();
        let new_target = new_effect.and_then(|effect| effect.borrow().target_element());
        if old_target != new_target {
            if let (Some(element), Some(effect)) = (old_target, &old_effect) {
                self.emit(HostEffectKind::DisassociateFromTarget {
                    element,
                    effect: effect.clone(),
                });
            }
            if let Some(element) = new_target {
                self.emit(HostEffectKind::AssociateWithTarget { element });
            }
        }
        self.link_associated_effect(new_effect);

        // NB: Once the animated properties of old effect no longer apply, the style of its target is recomputed without
        //     them.
        if let Some(old_effect) = old_effect {
            self.emit(HostEffectKind::RecomputeEffectStyle { effect: old_effect });
        }

        // 7. Run the procedure to update an animation’s finished state for animation with the did seek flag set to
        //    false, and the synchronously notify flag set to false.
        self.update_finished_state(DidSeek::No, SynchronouslyNotify::No, invalidate, None);
    }

    /// Lets the associated effect of the animation be `new_effect`, which is associated with no other animation, and
    /// associates the old effect with none.
    fn link_associated_effect(&self, new_effect: Option<&EffectHandle>) {
        if let Some(new_effect) = new_effect {
            new_effect
                .borrow_mut()
                .set_associated_animation(Some(self.animation), self.timeline.as_ref());
        }
        if let Some(old_effect) = self.animation.borrow().effect() {
            old_effect.borrow_mut().set_associated_animation(None, None);
        }
        self.animation.borrow_mut().set_effect(new_effect.cloned());
    }

    /// Lets the associated effect of the animation, which has none, be `effect`, without associating the animation
    /// with the target of the effect, which must not observe it until a surrounding style stabilization epoch commits
    /// it.
    pub(crate) fn set_provisional_effect(&self, effect: &EffectHandle) {
        assert!(
            self.animation.borrow().effect().is_none(),
            "the animation has no associated effect"
        );
        assert!(
            effect.borrow().associated_animation().is_none(),
            "the effect is associated with no animation"
        );
        self.link_associated_effect(Some(effect));
    }

    /// Lets the animation, whose associated effect was set provisionally, have no associated effect again.
    pub(crate) fn discard_provisional_effect(&self) {
        let animation = self
            .animation
            .borrow()
            .effect()
            .and_then(|effect| effect.borrow().associated_animation());
        assert!(
            animation.is_some_and(|animation| animation.ptr_eq(self.animation)),
            "the animation has a provisional effect"
        );
        self.link_associated_effect(None);
    }

    /// Keeps the animation of an SVG animation element at the time of its time container: where the container is
    /// paused at `paused_time`, in milliseconds, the animation holds that time, and otherwise it starts at
    /// `zero_time`, the time of the document timeline at which the container's timeline began.
    /// https://drafts.csswg.org/web-animations-1/#setting-the-start-time-of-an-animation
    /// https://drafts.csswg.org/web-animations-1/#setting-the-current-time-of-an-animation
    /// NB: Both procedures take effect synchronously, and update the finished state of the animation as a seek does. A
    ///     backward seek therefore applies to an animation that already finished, too.
    pub(crate) fn synchronize_with_smil_time_container(&mut self, zero_time: f64, paused_time: Option<f64>) {
        let Some(paused_time) = paused_time else {
            self.set_the_start_time(Some(TimeValue::milliseconds(zero_time)));
            return;
        };
        // NB: Making the start time unresolved holds the animation at its current time, which setting the current time
        //     then moves to the time the timeline is paused at.
        self.set_the_start_time(None);
        self.set_current_time(Some(TimeValue::milliseconds(paused_time)))
            .expect("setting the current time to a resolved time does not throw");
    }

    /// Disassociates the animation from the target of its associated effect where it is inert, which a procedure
    /// scheduled.
    /// AD-HOC: The target's animation list is our own cache of the animations whose effect targets it, which an
    ///         animation that produces nothing leaves until it is revived.
    pub(crate) fn disassociate_from_target_if_inert(&mut self) {
        self.animation.borrow_mut().disassociation_from_target_pending = false;
        // NB: Script may have revived the animation since the disassociation was scheduled.
        let Some(effect) = self.animation.borrow().effect().cloned() else {
            return;
        };
        let Some(element) = effect.borrow().target_element() else {
            return;
        };
        if self.pending() || self.is_relevant() {
            return;
        }
        self.emit(HostEffectKind::DisassociateFromTarget {
            element,
            effect: effect.clone(),
        });
        let play_state = self.play_state();
        self.animation.borrow_mut().play_state_when_disassociated_from_target = Some(play_state);
        // NB: The target's animated style keeps the effect's values until they are recomputed without it.
        self.emit(HostEffectKind::RecomputeEffectStyle { effect });
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-replace-state
    /// NB: An animation whose replace state is removed is associated with neither the target of its associated effect
    ///     nor its timeline, until it is persisted.
    pub(crate) fn set_replace_state(&mut self, replace_state: AnimationReplaceState) {
        let previous_replace_state = self.playback().replace_state;
        let associated = match replace_state {
            AnimationReplaceState::Removed => Some(false),
            AnimationReplaceState::Persisted if previous_replace_state == AnimationReplaceState::Removed => Some(true),
            _ => None,
        };
        if let Some(associated) = associated {
            let effect = self.animation.borrow().effect().cloned();
            if let Some(effect) = effect
                && let Some(element) = effect.borrow().target_element()
            {
                self.emit(if associated {
                    HostEffectKind::AssociateWithTarget { element }
                } else {
                    HostEffectKind::DisassociateFromTarget {
                        element,
                        effect: effect.clone(),
                    }
                });
            }
            let timeline = self.animation.borrow().timeline().cloned();
            if let Some(timeline) = timeline {
                let mut associated_animations = timeline.associated_animations().borrow_mut();
                if associated {
                    associated_animations.insert(self.animation);
                } else {
                    associated_animations.remove(self.animation);
                }
            }
        }
        self.update_playback(|playback| playback.replace_state = replace_state);
    }

    /// https://www.w3.org/TR/web-animations-1/#replaceable-animation
    fn is_replaceable(&self) -> bool {
        // An animation is replaceable if all of the following conditions are true:

        // - The existence of the animation is not prescribed by markup. That is, it is not a CSS animation with an
        //   owning element, nor a CSS transition with an owning element.
        if is_prescribed_by_markup(&self.animation.borrow()) {
            return false;
        }

        // - The animation's play state is finished.
        if self.play_state() != AnimationPlayState::Finished {
            return false;
        }

        // - The animation's replace state is not removed.
        if self.playback().replace_state == AnimationReplaceState::Removed {
            return false;
        }

        // - The animation is associated with a monotonically increasing timeline.
        if !self.has_monotonically_increasing_timeline() {
            return false;
        }

        // - The animation has an associated effect.
        let Some(timing) = self.effect_timing() else {
            return false;
        };

        // - The animation's associated effect is in effect.
        // NB: The local time of the associated effect is the current time of the animation.
        let playback = self.playback();
        let resolved = timing::resolve_timing(
            &timing,
            self.current_time(),
            Some(&playback),
            self.timeline.as_ref().map(|timeline| &timeline.sample),
        );
        if !timing::is_in_effect(&resolved) {
            return false;
        }

        // - The animation's associated effect has an effect target.
        self.effect_has_target()
    }

    /// The steps the remove replaced animations procedure performs for an animation it removes.
    /// https://www.w3.org/TR/web-animations-1/#remove-replaced-animations
    fn remove_as_replaced(&mut self) {
        // - Set animation’s replace state to removed.
        self.set_replace_state(AnimationReplaceState::Removed);

        // - Create an AnimationPlaybackEvent, removeEvent.
        // - Set removeEvent’s type attribute to remove.
        // - Set removeEvent’s currentTime attribute to the current time of animation.
        // - Set removeEvent’s timelineTime attribute to the current time of the timeline with which animation is
        //   associated.
        // - If animation has a document for timing, then append removeEvent to its document for timing's pending
        //   animation event queue along with its target, animation. For the scheduled event time, use the result of
        //   applying the procedure to convert timeline time to origin-relative time to the current time of the
        //   timeline with which animation is associated.
        //   Otherwise, queue a task to dispatch removeEvent at animation. The task source for this task is the DOM
        //   manipulation task source.
        // NB: The host creates and queues the event.
        let timeline_time = self.timeline_time();
        self.emit(HostEffectKind::QueuePlaybackEvent {
            r#type: PlaybackEventType::Remove,
            current_time: self.current_time(),
            timeline_time,
            scheduled_timeline_time: timeline_time,
        });
    }

    // NB: The procedures below run as script asks for them through the Animation interface. Script that changes the
    //     play state of a CSS animation overrides its animation-play-state from then on, which the procedures the
    //     style computation runs for animation-play-state do not.

    /// Notes that script changed the play state of the animation, where it is a CSS animation.
    fn note_script_play_state_override(&self) {
        let mut animation = self.animation.borrow_mut();
        if let AnimationKind::CssAnimation(_) = animation.kind() {
            animation
                .css_animation_mut()
                .set_flag(CssAnimationFlag::ScriptOverrodePlayState);
        }
    }

    /// Runs `procedure`, noting that script changed the play state where it changes it.
    fn as_script_changing_the_play_state(
        &mut self,
        procedure: impl FnOnce(&mut Self) -> Result<(), AnimationError>,
    ) -> Result<(), AnimationError> {
        let previous_play_state = self.play_state();
        procedure(self)?;
        if self.play_state() != previous_play_state {
            self.note_script_play_state_override();
        }
        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-play
    pub(crate) fn play(&mut self, invalidate: Invalidate) -> Result<(), AnimationError> {
        // Begins or resumes playback of the animation by running the procedure to play an animation passing true as
        // the value of the auto-rewind flag.
        self.as_script_changing_the_play_state(|procedures| procedures.play_an_animation(AutoRewind::Yes, invalidate))
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-pause
    pub(crate) fn pause(&mut self) -> Result<(), AnimationError> {
        self.pause_an_animation()?;
        self.note_script_play_state_override();
        Ok(())
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-reverse
    pub(crate) fn reverse(&mut self) -> Result<(), AnimationError> {
        self.as_script_changing_the_play_state(Self::reverse_an_animation)
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-cancel
    pub(crate) fn cancel(&mut self, invalidate: Invalidate) {
        self.note_script_play_state_override();
        self.cancel_an_animation(invalidate);
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-starttime
    pub(crate) fn set_start_time_for_bindings(&mut self, new_start_time: Option<TimeValue>) {
        // Setting this attribute updates the start time using the procedure to set the start time of this object to
        // the new value.
        self.as_script_changing_the_play_state(|procedures| {
            procedures.set_the_start_time(new_start_time);
            Ok(())
        })
        .expect("setting the start time does not throw");
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-currenttime
    pub(crate) fn set_current_time_for_bindings(&mut self, seek_time: Option<TimeValue>) -> Result<(), AnimationError> {
        // Setting this attribute follows the procedure to set the current time of this object to the new value.
        self.as_script_changing_the_play_state(|procedures| procedures.set_current_time(seek_time))
    }

    /// Plays a CSS animation as its animation-play-state asks.
    /// https://drafts.csswg.org/css-animations-1/#animation-play-state
    pub(crate) fn play_from_css(&mut self) -> Result<(), AnimationError> {
        self.play_an_animation(AutoRewind::Yes, Invalidate::Yes)
    }

    /// Pauses a CSS animation as its animation-play-state asks.
    /// https://drafts.csswg.org/css-animations-1/#animation-play-state
    pub(crate) fn pause_from_css(&mut self) -> Result<(), AnimationError> {
        self.pause_an_animation()
    }
}

/// Sets the target element of `effect` to `element`, the host's `DOM::Element`, or to none, and hands back what that asks
/// the host to do: its associated animation, if any, is associated with the new target rather than the old.
/// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-target
pub(crate) fn set_effect_target(effect: &EffectHandle, element: Option<NonNull<c_void>>) -> HostEffects {
    let mut effects = HostEffects::default();
    let animation = effect.borrow().associated_animation();
    if let Some(animation) = animation {
        let old_target = effect.borrow().target_element();
        if let Some(old_target) = old_target {
            effects.push(
                &animation,
                HostEffectKind::DisassociateFromTarget {
                    element: old_target,
                    effect: effect.clone(),
                },
            );
        }
        if let Some(element) = element {
            effects.push(&animation, HostEffectKind::AssociateWithTarget { element });
        }
    }
    effect.borrow_mut().set_target_element(element);
    effects
}

/// Whether the existence of `animation` is prescribed by markup: it is a CSS animation or a CSS transition with an
/// owning element.
/// https://www.w3.org/TR/web-animations-1/#replaceable-animation
fn is_prescribed_by_markup(animation: &AnimationRecord) -> bool {
    matches!(
        animation.kind(),
        AnimationKind::CssAnimation(_) | AnimationKind::CssTransition(_)
    ) && animation.owning_element().is_some()
}

/// https://www.w3.org/TR/web-animations-1/#remove-replaced-animations
/// For the document whose associated timelines are `timelines`, where `is_descendant_of_document` answers whether the
/// host's `DOM::Element` is a descendant of it, and `tree_order` compares elements in tree order. Hands back what that
/// asks the host to do.
pub(crate) fn remove_replaced_animations<'a>(
    timelines: impl IntoIterator<Item = &'a TimelineCell>,
    is_descendant_of_document: impl Fn(NonNull<c_void>) -> bool,
    tree_order: &mut impl TreeOrder,
) -> HostEffects {
    // When asked to remove replaced animations for a Document, doc, then for every animation, animation, that:
    // - has an associated animation effect whose effect target is a descendant of doc, and
    // - is replaceable, and
    // - has a replace state of active, and
    // - for which there exists for each target property of every animation effect associated with animation, an
    //   animation effect associated with a replaceable animation with a higher composite order than animation that
    //   includes the same target property
    let mut replaceable_animations: Vec<AnimationHandle> = Vec::new();
    for timeline in timelines {
        let mut associated_animations = timeline.associated_animations().borrow_mut();
        associated_animations.prune();
        for index in 0..associated_animations.entry_count() {
            let Some(animation) = associated_animations.get(index) else {
                continue;
            };
            // AD-HOC: Like a CSS animation with an owning element, an SVG animation element prescribes its animation in
            //         markup, so it is not replaceable.
            if matches!(animation.borrow().kind(), AnimationKind::Smil { .. }) {
                continue;
            }
            // OPTIMIZATION: A CSS animation or CSS transition with an owning element is never replaceable. Rule those
            //               out before looking up the effect target and running the full replaceability check.
            if is_prescribed_by_markup(&animation.borrow()) {
                continue;
            }
            let target = animation
                .borrow()
                .effect()
                .and_then(|effect| effect.borrow().target_element());
            if !target.is_some_and(&is_descendant_of_document) {
                continue;
            }
            if !query(&animation, |procedures| {
                procedures.is_replaceable() && procedures.playback().replace_state == AnimationReplaceState::Active
            }) {
                continue;
            }
            // NB: Every animation effect is a keyframe effect, whose composite order is defined.
            replaceable_animations.push(animation);
        }
    }

    replaceable_animations.sort_by(|a, b| composite_order(&a.borrow(), &b.borrow(), tree_order));

    let mut effects = HostEffects::default();
    let mut properties_with_a_higher_animation = HashSet::new();
    for animation in replaceable_animations.iter().rev() {
        let mut has_any_highest_priority_property = false;
        let effect = animation.borrow().effect().cloned();
        if let Some(effect) = effect
            && let Some(keyframes) = effect.borrow().keyframes().cloned()
        {
            // NB: An animation is replaced by those whose effects animate the same target properties of the same
            //     effect target, as only those come above it in its effect stack.
            let effect_target = {
                let effect = effect.borrow();
                (
                    effect.target_element(),
                    effect.pseudo_element().map(|pseudo_element| pseudo_element.kind),
                )
            };
            let properties = keyframes
                .target_properties()
                .iter()
                .map(|property| TargetProperty::Longhand(*property))
                .chain(
                    keyframes
                        .target_custom_properties()
                        .iter()
                        .map(|name| TargetProperty::Custom(name.raw())),
                );
            for property in properties {
                has_any_highest_priority_property |=
                    properties_with_a_higher_animation.insert((effect_target, property));
            }
        }

        if !has_any_highest_priority_property {
            // perform the following steps:
            let ((), removal_effects) = run(animation, |procedures| {
                procedures.remove_as_replaced();
                Ok(())
            })
            .expect("removing an animation does not throw");
            effects.extend(removal_effects);
        }
    }
    effects
}

/// A target property of a keyframe effect: the host's `PropertyID` of a longhand, or the raw fly string of a custom
/// property's name.
/// https://www.w3.org/TR/web-animations-1/#target-property
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum TargetProperty {
    Longhand(u16),
    Custom(usize),
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::animations::animation::CssAnimationState;
    use crate::animations::easing::{CompactEasing, keyword_cubic_bezier};
    use crate::animations::effect::EffectHandle;
    use crate::animations::timeline::TimelineKind;
    use crate::css::retained_fly_string::RetainedUtf16FlyString;
    use timing::{OptionalTimeValue, SpecifiedEffectTiming, SpecifiedIterationDuration, TimelineSample};

    fn document_timeline_at(milliseconds: f64) -> TimelineState {
        TimelineState {
            sample: TimelineSample {
                current_time: Some(TimeValue::milliseconds(milliseconds)).into(),
                minimum_timeline_time: OptionalTimeValue::UNRESOLVED,
                maximum_timeline_time: OptionalTimeValue::UNRESOLVED,
                is_monotonically_increasing: true,
            },
            duration: None,
            attachment_interval: None,
            is_progress_based: false,
        }
    }

    /// An animation of `kind` with a one second effect, on `timeline`.
    fn one_second_animation(kind: AnimationKind, timeline: &TimelineState) -> AnimationHandle {
        let effect = EffectHandle::new();
        effect.borrow_mut().set_specified_timing(SpecifiedEffectTiming {
            iteration_duration: SpecifiedIterationDuration {
                is_auto: false,
                milliseconds: 1000.0,
            },
            ..SpecifiedEffectTiming::default()
        });
        effect.borrow_mut().normalize_specified_timing(Some(timeline));
        let animation = AnimationHandle::new(kind);
        animation.borrow_mut().set_effect(Some(effect));
        animation
    }

    fn css_animation() -> AnimationKind {
        AnimationKind::CssAnimation(CssAnimationState {
            name: RetainedUtf16FlyString::none(),
            default_easing: CompactEasing::of(keyword_cubic_bezier(0)),
            applied_definition: None,
            named_timeline: RetainedUtf16FlyString::none(),
            name_index: 0,
            last_css_play_state: None,
            flags: 0,
        })
    }

    fn kinds_of<T>(
        animation: &AnimationHandle,
        timeline: &TimelineState,
        procedure: impl FnOnce(&mut Procedures<'_>) -> Result<T, AnimationError>,
    ) -> Vec<HostEffectKind> {
        run_on(animation, Some(*timeline), procedure)
            .expect("the procedure does not throw")
            .1
            .kinds()
    }

    #[test]
    fn finishing_an_animation_at_rate_zero_throws() {
        let timeline = document_timeline_at(0.0);
        let animation = one_second_animation(AnimationKind::Script, &timeline);
        animation.borrow_mut().playback_mut().playback_rate = 0.0;
        let result = run_on(&animation, Some(timeline), |procedures| {
            procedures.finish_an_animation()
        });
        assert!(matches!(result, Err(AnimationError::InvalidState(_))));
    }

    #[test]
    fn reversing_without_an_active_timeline_throws() {
        let timeline = document_timeline_at(0.0);
        let animation = one_second_animation(AnimationKind::Script, &timeline);
        let result = run_on(&animation, None, |procedures| procedures.reverse());
        assert!(matches!(result, Err(AnimationError::InvalidState(_))));
    }

    fn script_overrode_play_state(animation: &AnimationHandle) -> bool {
        animation
            .borrow()
            .css_animation()
            .has_flag(CssAnimationFlag::ScriptOverrodePlayState)
    }

    // https://drafts.csswg.org/css-animations-2/#animations

    #[test]
    fn script_playing_a_running_css_animation_does_not_override_animation_play_state() {
        let timeline = document_timeline_at(0.0);
        let animation = one_second_animation(css_animation(), &timeline);

        kinds_of(&animation, &timeline, |procedures| procedures.play_from_css());
        kinds_of(&animation, &timeline, |procedures| procedures.play(Invalidate::No));
        assert!(!script_overrode_play_state(&animation));

        kinds_of(&animation, &timeline, |procedures| procedures.pause());
        assert!(script_overrode_play_state(&animation));
    }

    // https://www.w3.org/TR/web-animations-1/#setting-the-associated-effect
    #[test]
    fn setting_the_effect_of_another_animation_takes_it_from_that_one() {
        let timeline = document_timeline_at(0.0);
        let previous_animation = one_second_animation(AnimationKind::Script, &timeline);
        let effect = previous_animation.borrow().effect().cloned().unwrap();
        // NB: The test helper links the effect to the animation as the record alone; link it as the procedure does.
        effect
            .borrow_mut()
            .set_associated_animation(Some(&previous_animation), Some(&timeline));
        let animation = AnimationHandle::new(AnimationKind::Script);

        run_on(&animation, Some(timeline), |procedures| {
            procedures.set_the_associated_effect(Some(&effect), Invalidate::No);
            Ok(())
        })
        .unwrap();
        assert!(previous_animation.borrow().effect().is_none());
        assert!(animation.borrow().effect().is_some_and(|linked| linked.ptr_eq(&effect)));
        assert!(
            effect
                .borrow()
                .associated_animation()
                .is_some_and(|associated| associated.ptr_eq(&animation))
        );

        // Setting the same effect again does nothing.
        let kinds = kinds_of(&animation, &timeline, |procedures| {
            procedures.set_the_associated_effect(Some(&effect), Invalidate::Yes);
            Ok(())
        });
        assert!(kinds.is_empty());
    }

    #[test]
    fn removing_an_animation_disassociates_it_from_its_timeline_until_persisted() {
        let dangling = NonNull::<u8>::dangling().cast::<c_void>();
        let timeline = TimelineHandle::new(
            dangling,
            TimelineKind::Internal {
                time_for_observation: None,
            },
        );
        let animation = one_second_animation(AnimationKind::Script, &document_timeline_at(0.0));
        // NB: A set of animations holds only the animations whose host `Animation` is alive.
        animation.set_object_cell(Some(dangling));
        let associated = || timeline.associated_animations().borrow().contains(&animation);
        run(&animation, |procedures| {
            procedures.set_timeline(Some(timeline.clone()));
            Ok(())
        })
        .unwrap();
        assert!(associated());

        for (replace_state, is_associated) in [
            (AnimationReplaceState::Removed, false),
            (AnimationReplaceState::Persisted, true),
            // Persisting an animation that was not removed changes no association.
            (AnimationReplaceState::Persisted, true),
        ] {
            let kinds = run(&animation, |procedures| {
                procedures.set_replace_state(replace_state);
                Ok(())
            })
            .unwrap()
            .1
            .kinds();
            assert!(kinds.is_empty());
            assert_eq!(associated(), is_associated);
        }
    }

    /// A finished script animation on `timeline`, which runs from time zero, whose one second effect targets `target`,
    /// fills forwards and animates `properties`.
    fn finished_animation(timeline: &TimelineHandle, target: NonNull<c_void>, properties: &[u16]) -> AnimationHandle {
        use crate::animations::keyframes::{
            CompositeOperationOrAuto, KeyframeProperty, KeyframeSet, ProcessedKeyframe,
        };
        use crate::css::css_enums::keyword;
        use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
        use std::sync::Arc;

        let animation = one_second_animation(AnimationKind::Script, &document_timeline_at(0.0));
        let effect = animation.borrow().effect().cloned().unwrap();
        let specified = SpecifiedEffectTiming {
            fill_mode: FillMode::Forwards,
            ..*effect.borrow().specified_timing()
        };
        effect.borrow_mut().set_specified_timing(specified);
        effect
            .borrow_mut()
            .normalize_specified_timing(Some(&document_timeline_at(0.0)));
        effect.borrow_mut().set_target_element(Some(target));
        let keyframe = |key| ProcessedKeyframe {
            key,
            composite: CompositeOperationOrAuto::Auto,
            easing: None,
            declarations: properties
                .iter()
                .map(|property| {
                    (
                        KeyframeProperty::Id(*property),
                        RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: keyword::AUTO }),
                    )
                })
                .collect(),
        };
        effect
            .borrow_mut()
            .set_keyframes(Some(Arc::new(KeyframeSet::from_processed_keyframes(
                vec![keyframe(0), keyframe(crate::animations::keyframes::FINAL_KEY)],
                None,
            ))));
        // NB: A set of animations holds only the animations whose host `Animation` is alive.
        animation.set_object_cell(Some(NonNull::<u8>::dangling().cast()));
        animation.borrow_mut().playback_mut().start_time = Some(TimeValue::milliseconds(0.0)).into();
        run(&animation, |procedures| {
            procedures.set_timeline(Some(timeline.clone()));
            Ok(())
        })
        .unwrap();
        animation
    }

    fn removed_animations(effects: HostEffects) -> Vec<AnimationHandle> {
        effects
            .into_iter()
            .filter_map(|(animation, kind)| {
                matches!(
                    kind,
                    HostEffectKind::QueuePlaybackEvent {
                        r#type: PlaybackEventType::Remove,
                        ..
                    }
                )
                .then_some(animation)
            })
            .collect()
    }

    #[test]
    fn an_animation_whose_properties_later_animations_replace_is_removed() {
        use crate::css::property_metadata::property_id;
        let dangling = NonNull::<u8>::dangling().cast::<c_void>();
        let timeline = TimelineHandle::new(
            dangling,
            TimelineKind::Internal {
                time_for_observation: None,
            },
        );
        timeline.set_internal_time(Some(2000.0), false, 0, || {});
        let target = dangling;
        let in_document = |_| true;
        let replaced = finished_animation(&timeline, target, &[property_id::OPACITY]);
        let partly_replaced = finished_animation(&timeline, target, &[property_id::OPACITY, property_id::COLOR]);
        let replacing = finished_animation(&timeline, target, &[property_id::OPACITY]);

        let effects = remove_replaced_animations([&*timeline], in_document, &mut |_, _| std::cmp::Ordering::Equal);
        let removed = removed_animations(effects);
        assert_eq!(removed.len(), 1);
        assert!(removed[0].ptr_eq(&replaced));
        assert_eq!(
            replaced.borrow().playback().replace_state,
            AnimationReplaceState::Removed
        );
        assert!(!timeline.associated_animations().borrow().contains(&replaced));
        for animation in [&partly_replaced, &replacing] {
            assert_eq!(
                animation.borrow().playback().replace_state,
                AnimationReplaceState::Active
            );
        }

        // A removed animation is no longer replaceable, and the others replace nothing more.
        let effects = remove_replaced_animations([&*timeline], in_document, &mut |_, _| std::cmp::Ordering::Equal);
        assert!(removed_animations(effects).is_empty());
    }

    #[test]
    fn only_finished_animations_of_the_document_are_replaced() {
        use crate::css::property_metadata::property_id;
        let dangling = NonNull::<u8>::dangling().cast::<c_void>();
        let timeline = TimelineHandle::new(
            dangling,
            TimelineKind::Internal {
                time_for_observation: None,
            },
        );
        timeline.set_internal_time(Some(500.0), false, 0, || {});
        let running = finished_animation(&timeline, dangling, &[property_id::OPACITY]);
        let _replacing = finished_animation(&timeline, dangling, &[property_id::OPACITY]);

        // An animation that has not finished is not replaceable.
        let effects = remove_replaced_animations([&*timeline], |_| true, &mut |_, _| std::cmp::Ordering::Equal);
        assert!(removed_animations(effects).is_empty());

        // Nor is one whose target is not a descendant of the document.
        timeline.set_internal_time(Some(2000.0), false, 0, || {});
        let effects = remove_replaced_animations([&*timeline], |_| false, &mut |_, _| std::cmp::Ordering::Equal);
        assert!(removed_animations(effects).is_empty());

        let effects = remove_replaced_animations([&*timeline], |_| true, &mut |_, _| std::cmp::Ordering::Equal);
        let removed = removed_animations(effects);
        assert_eq!(removed.len(), 1);
        assert!(removed[0].ptr_eq(&running));
    }
}
