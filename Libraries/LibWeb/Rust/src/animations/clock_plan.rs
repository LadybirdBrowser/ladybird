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
        // NB: The compositor runs a transition whose play is still pending from the time of the style change event
        //     that started it, on the document timeline.
        if matches!(self.kind(), AnimationKind::CssTransition(_))
            && playback.pending()
            && playback.start_time.get().is_none()
            && effect.has_flag(EffectFlag::CompositorDriven)
            && clock.source == TimelineClockSource::Document
        {
            playback.start_time = Some(TimeValue::milliseconds(self.css_transition_style_change_event_time())).into();
            playback.hold_time = None.into();
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::animations::animation::AnimationHandle;
    use crate::animations::effect::EffectHandle;
    use timing::{SpecifiedEffectTiming, SpecifiedIterationDuration};

    /// A script animation of a one second effect on a document timeline, started at `start_time`, or pending.
    fn animation_started_at(start_time: Option<f64>) -> AnimationHandle {
        let effect = EffectHandle::new();
        effect.borrow_mut().set_specified_timing(SpecifiedEffectTiming {
            iteration_duration: SpecifiedIterationDuration {
                is_auto: false,
                milliseconds: 1000.0,
            },
            ..SpecifiedEffectTiming::default()
        });
        effect.borrow_mut().normalize_specified_timing(None);
        let animation = AnimationHandle::new(AnimationKind::Script);
        animation.borrow_mut().set_effect(Some(effect));
        let mut record = animation.borrow_mut();
        let playback = record.playback_mut();
        match start_time {
            Some(start_time) => playback.start_time = Some(TimeValue::milliseconds(start_time)).into(),
            None => {
                playback.hold_time = Some(TimeValue::milliseconds(0.0)).into();
                playback.has_pending_play_task = true;
            }
        }
        drop(record);
        animation
    }

    #[test]
    fn a_tick_moves_a_running_animation_until_its_active_interval_ends() {
        let animation = animation_started_at(Some(100.0));
        let plan = animation
            .borrow()
            .clock_plan(Some(TimeValue::milliseconds(600.0)), false);
        assert_eq!(
            plan,
            ClockPlanAnimation::Ticked {
                local_time: TimeValue::milliseconds(500.0),
                active_end: TimeValue::milliseconds(1000.0),
            }
        );
        let plan = animation
            .borrow()
            .clock_plan(Some(TimeValue::milliseconds(1100.0)), false);
        assert_eq!(plan, ClockPlanAnimation::NotTicked);
    }

    #[test]
    fn a_pending_animation_holds_still() {
        let animation = animation_started_at(None);
        let plan = animation.borrow().clock_plan(Some(TimeValue::milliseconds(0.0)), false);
        assert_eq!(plan, ClockPlanAnimation::Held);
    }
}
