/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A wheel gesture keeps scrolling the scroller its first event was routed to, so the compositor has to tell whether a wheel event continues the gesture it latched or starts another one.

use super::{ScrollGesturePhase, USER_SCROLL_SETTLE_DELAY};
use crate::time::{Duration, MonotonicTime};
use libgfx_rust::FloatPoint;

pub const WHEEL_GESTURE_MOMENTUM_GRACE_AFTER_END: Duration = Duration::from_milliseconds(100);
pub const WHEEL_GESTURE_POSITION_SLOP_IN_CSS_PIXELS: f32 = 10.0;

/// When a touchpad gesture ends: once its steps stop arriving for a while, or once the platform reports that the fingers
/// lifted, however long they rest on the touchpad before that.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TouchpadGestureEnd {
    AfterIdleDelay,
    WhenFingersLift,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum Kind {
    PhaseLess,
    Phased,
}

pub struct WheelGestureIdentity {
    kind: Kind,
    first_position: FloatPoint,
    modifiers: u32,
    last_event_time: MonotonicTime,
    ended_at: Option<MonotonicTime>,
    momentum_has_started: bool,
}

impl WheelGestureIdentity {
    fn new(kind: Kind, first_position: FloatPoint, modifiers: u32, now: MonotonicTime) -> Self {
        Self {
            kind,
            first_position,
            modifiers,
            last_event_time: now,
            ended_at: None,
            momentum_has_started: false,
        }
    }

    fn kind_for(phase: ScrollGesturePhase) -> Kind {
        if phase == ScrollGesturePhase::None {
            Kind::PhaseLess
        } else {
            Kind::Phased
        }
    }

    pub fn started_by(position: FloatPoint, phase: ScrollGesturePhase, modifiers: u32, now: MonotonicTime) -> Self {
        let mut gesture = Self::new(Self::kind_for(phase), position, modifiers, now);
        gesture.advance_to(phase, now);
        gesture
    }

    /// Whether the fingers of a touchpad gesture are still on the touchpad, before it ends or hands over to momentum.
    pub fn is_held_by_fingers(&self) -> bool {
        self.kind == Kind::Phased && self.ended_at.is_none() && !self.momentum_has_started
    }

    /// Positions and the slop are in whatever unit the caller routes in.
    pub fn is_continued_by(
        &self,
        position: FloatPoint,
        phase: ScrollGesturePhase,
        modifiers: u32,
        now: MonotonicTime,
        position_slop: f32,
        touchpad_gesture_end: TouchpadGestureEnd,
    ) -> bool {
        let lasts_while_fingers_rest =
            touchpad_gesture_end == TouchpadGestureEnd::WhenFingersLift && self.is_held_by_fingers();
        if !lasts_while_fingers_rest && now - self.last_event_time > USER_SCROLL_SETTLE_DELAY {
            return false;
        }
        if Self::kind_for(phase) != self.kind {
            return false;
        }
        if self.kind == Kind::PhaseLess {
            return modifiers == self.modifiers && position.distance_from(self.first_position) <= position_slop;
        }
        // Fingers that touch down while the momentum of the gesture runs start another gesture, whether or not the end of
        // that momentum arrived first.
        if phase == ScrollGesturePhase::Ongoing && self.momentum_has_started {
            return false;
        }
        let Some(ended_at) = self.ended_at else {
            return true;
        };
        let phase_may_follow_an_ended_gesture =
            phase == ScrollGesturePhase::Momentum || phase == ScrollGesturePhase::Ended;
        phase_may_follow_an_ended_gesture && now - ended_at <= WHEEL_GESTURE_MOMENTUM_GRACE_AFTER_END
    }

    pub fn advance_to(&mut self, phase: ScrollGesturePhase, now: MonotonicTime) {
        self.last_event_time = now;
        if phase == ScrollGesturePhase::Momentum {
            self.momentum_has_started = true;
        }
        if phase != ScrollGesturePhase::Ended {
            self.ended_at = None;
            return;
        }
        if self.ended_at.is_none() {
            self.ended_at = Some(now);
        }
    }
}
