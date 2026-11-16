/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the timing of an animation effect reads of its animation and the animation's timeline.

use super::time_value::{OptionalNumber, OptionalTimeValue};

/// https://www.w3.org/TR/web-animations-1/#enumdef-animationplaystate
/// In IDL order.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnimationPlayState {
    Idle,
    Running,
    Paused,
    Finished,
}

/// https://www.w3.org/TR/web-animations-1/#enumdef-animationreplacestate
/// In IDL order.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AnimationReplaceState {
    #[default]
    Active,
    Removed,
    Persisted,
}

/// The playback state of an animation.
/// https://www.w3.org/TR/web-animations-1/#animations
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AnimationPlayback {
    /// https://www.w3.org/TR/web-animations-1/#animation-start-time
    pub start_time: OptionalTimeValue,
    /// https://www.w3.org/TR/web-animations-1/#animation-hold-time
    pub hold_time: OptionalTimeValue,
    /// https://www.w3.org/TR/web-animations-1/#playback-rate
    pub playback_rate: f64,
    /// https://www.w3.org/TR/web-animations-1/#pending-playback-rate
    pub pending_playback_rate: OptionalNumber,
    /// https://www.w3.org/TR/web-animations-1/#pending-play-task
    pub has_pending_play_task: bool,
    /// https://www.w3.org/TR/web-animations-1/#pending-pause-task
    pub has_pending_pause_task: bool,
    /// Whether the animation was last found finished when its finished state was updated.
    /// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
    pub is_finished: bool,
    /// https://www.w3.org/TR/web-animations-1/#animation-replace-state
    pub replace_state: AnimationReplaceState,
}

impl AnimationPlayback {
    /// https://www.w3.org/TR/web-animations-1/#pending
    #[inline]
    #[must_use]
    pub fn pending(&self) -> bool {
        self.has_pending_play_task || self.has_pending_pause_task
    }

    /// https://www.w3.org/TR/web-animations-1/#effective-playback-rate
    #[inline]
    #[must_use]
    pub fn effective_playback_rate(&self) -> f64 {
        // The effective playback rate of an animation is its pending playback rate, if set, otherwise it is the
        // animation’s playback rate.
        self.pending_playback_rate.get().unwrap_or(self.playback_rate)
    }
}

impl Default for AnimationPlayback {
    /// An idle animation.
    fn default() -> Self {
        Self {
            start_time: OptionalTimeValue::UNRESOLVED,
            hold_time: OptionalTimeValue::UNRESOLVED,
            playback_rate: 1.0,
            pending_playback_rate: OptionalNumber::default(),
            has_pending_play_task: false,
            has_pending_pause_task: false,
            is_finished: false,
            replace_state: AnimationReplaceState::Active,
        }
    }
}

/// What the timing calculations read of an animation's timeline at one moment.
/// https://www.w3.org/TR/web-animations-1/#timelines
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TimelineSample {
    /// https://www.w3.org/TR/web-animations-1/#timeline-current-time
    pub current_time: OptionalTimeValue,
    /// https://drafts.csswg.org/web-animations-2/#minimum-timeline-time
    pub minimum_timeline_time: OptionalTimeValue,
    /// https://drafts.csswg.org/web-animations-2/#maximum-timeline-time
    pub maximum_timeline_time: OptionalTimeValue,
    /// https://www.w3.org/TR/web-animations-1/#monotonically-increasing-timeline
    pub is_monotonically_increasing: bool,
}
