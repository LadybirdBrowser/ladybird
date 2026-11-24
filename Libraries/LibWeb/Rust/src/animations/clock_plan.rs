/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the render clock's plan for a rendering update makes of an animation, as far as its record decides: whether a
//! tick holds its element still, cannot move it, or moves it, and the timing a hover of the clock's lane runs a
//! transition with.

use super::animation::{AnimationKind, AnimationRecord};
use super::effect::{AnimationEffectRecord, EffectFlag};
use super::snapshot::{EffectTimingSnapshot, TimelineClock, TimelineClockSource};
use super::timing::{self, TimeValue, TimeValueType, TimelineSample};

/// What the render clock's plan makes of an animation of a keyframe effect with a target, as far as its record
/// decides.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum ClockPlanAnimation {
    /// The animation's play is pending and the compositor does not run it: it holds still until the host makes it
    /// ready, so the ticks sample nothing of its element.
    Held,
    /// A tick cannot move the animation.
    NotTicked,
    /// A tick moves the animation, whose effect is at `local_time` and, on a document timeline, ends its active
    /// interval at `active_end`.
    Ticked {
        local_time: TimeValue,
        active_end: TimeValue,
    },
}

/// Whether a compositor animation runs `effect`, or what it animates in its place, which the render clock then does not
/// sample.
pub(crate) fn runs_on_compositor(effect: &AnimationEffectRecord) -> bool {
    effect.has_flag(EffectFlag::CompositorDriven) || effect.has_flag(EffectFlag::CompositorReplaced)
}

impl AnimationRecord {
    /// What the render clock's plan makes of the animation, whose timeline is at `timeline_time` and is a scroll
    /// timeline where `on_scroll_timeline`.
    pub(crate) fn clock_plan(&self, timeline_time: Option<TimeValue>, on_scroll_timeline: bool) -> ClockPlanAnimation {
        let effect = self
            .effect()
            .expect("the plan runs on animations of keyframe effects")
            .borrow();
        let playback = self.playback();
        // NB: A tick moves the document's timeline, at the rate it runs, and the scroll timelines, to where the
        //     compositor has scrolled. What the compositor runs already runs there while its play is pending. Anything
        //     else holds still while its play is pending, until the host makes it ready, as it does in the boxes of
        //     the host's frame.
        if playback.pending() && !runs_on_compositor(&effect) {
            return ClockPlanAnimation::Held;
        }
        if playback.playback_rate.is_nan() || playback.playback_rate <= 0.0 {
            return ClockPlanAnimation::NotTicked;
        }
        // NB: Times on a document timeline are in milliseconds, and those on a scroll timeline percentages of its
        //     progress, throughout.
        let unit = if on_scroll_timeline {
            TimeValueType::Percentage
        } else {
            TimeValueType::Milliseconds
        };
        let timing = effect.timing();
        let Some(local_time) = timing::current_time_at(playback, timeline_time) else {
            return ClockPlanAnimation::NotTicked;
        };
        if local_time.r#type != unit
            || timing.start_delay.r#type != unit
            || timing.iteration_duration.r#type != unit
            || timing::active_duration(&timing).r#type != unit
        {
            return ClockPlanAnimation::NotTicked;
        }
        let active_end = timing.start_delay + timing::active_duration(&timing);
        // NB: An animation of a scroll timeline that finished runs again as the scroll goes back.
        if !on_scroll_timeline && local_time.value >= active_end.value {
            return ClockPlanAnimation::NotTicked;
        }
        ClockPlanAnimation::Ticked { local_time, active_end }
    }

    /// The timing a hover of the render clock's lane runs the animation with, on its timeline at `timeline`, where
    /// `has_timeline`, which `clock` moves.
    pub(crate) fn hover_timing(
        &self,
        timeline: TimelineSample,
        has_timeline: bool,
        clock: TimelineClock,
    ) -> EffectTimingSnapshot {
        let effect = self
            .effect()
            .expect("the hover runs animations of keyframe effects")
            .borrow();
        let mut playback = *self.playback();
        // NB: The compositor runs a transition whose play is still pending as the pending play task would start it, from
        //     the time the play becomes ready at on the document timeline.
        if matches!(self.kind(), AnimationKind::CssTransition(_))
            && playback.pending()
            && playback.start_time.get().is_none()
            && effect.has_flag(EffectFlag::CompositorDriven)
            && clock.source == TimelineClockSource::Document
            && let Some(ready_time) = self.css_transition_initial_ready_time()
        {
            let ready_time = TimeValue::milliseconds(ready_time);
            let playback_rate = playback.effective_playback_rate();
            playback.playback_rate = playback_rate;
            playback.pending_playback_rate = None.into();
            match playback.hold_time.get() {
                Some(hold_time) if playback_rate != 0.0 => {
                    playback.start_time = Some(ready_time - hold_time / playback_rate).into();
                    playback.hold_time = None.into();
                }
                _ => playback.start_time = Some(ready_time).into(),
            }
        }
        EffectTimingSnapshot {
            effect: effect.timing(),
            playback,
            timeline,
            has_timeline,
            clock,
        }
    }
}
