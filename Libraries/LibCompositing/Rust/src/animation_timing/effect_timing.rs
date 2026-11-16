/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The timing properties of an animation effect, as specified and as normalized.

use super::time_value::{TimeValue, TimeValueType};

/// https://www.w3.org/TR/web-animations-1/#enumdef-fillmode
/// In IDL order.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FillMode {
    None,
    Forwards,
    Backwards,
    Both,
    #[default]
    Auto,
}

/// https://www.w3.org/TR/web-animations-1/#enumdef-playbackdirection
/// In IDL order.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PlaybackDirection {
    #[default]
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
}

impl TryFrom<u8> for FillMode {
    type Error = u8;

    /// The fill mode at `index` in IDL order.
    fn try_from(index: u8) -> Result<Self, u8> {
        Ok(match index {
            0 => Self::None,
            1 => Self::Forwards,
            2 => Self::Backwards,
            3 => Self::Both,
            4 => Self::Auto,
            _ => return Err(index),
        })
    }
}

impl TryFrom<u8> for PlaybackDirection {
    type Error = u8;

    /// The playback direction at `index` in IDL order.
    fn try_from(index: u8) -> Result<Self, u8> {
        Ok(match index {
            0 => Self::Normal,
            1 => Self::Reverse,
            2 => Self::Alternate,
            3 => Self::AlternateReverse,
            _ => return Err(index),
        })
    }
}

/// https://www.w3.org/TR/web-animations-1/#animation-direction
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnimationDirection {
    Forwards,
    Backwards,
}

/// https://www.w3.org/TR/web-animations-1/#animation-effect-phases-and-states
/// The phase of an animation effect, with Idle for an unresolved local time.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AnimationEffectPhase {
    Before,
    Active,
    After,
    #[default]
    Idle,
}

/// https://drafts.csswg.org/web-animations-2/#specified-iteration-duration
/// The iteration duration as specified: auto, or a time in milliseconds.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpecifiedIterationDuration {
    pub is_auto: bool,
    pub milliseconds: f64,
}

impl SpecifiedIterationDuration {
    pub const AUTO: Self = Self {
        is_auto: true,
        milliseconds: 0.0,
    };

    #[inline]
    #[must_use]
    pub const fn milliseconds(milliseconds: f64) -> Self {
        Self {
            is_auto: false,
            milliseconds,
        }
    }

    /// The duration in milliseconds, or none for auto.
    #[inline]
    #[must_use]
    pub fn time(self) -> Option<f64> {
        (!self.is_auto).then_some(self.milliseconds)
    }
}

impl Default for SpecifiedIterationDuration {
    fn default() -> Self {
        Self::AUTO
    }
}

/// The timing properties of an animation effect as specified, which normalizing against the
/// effect's timeline turns into `AnimationEffectTiming`.
/// https://www.w3.org/TR/web-animations-1/#the-effecttiming-dictionaries
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpecifiedEffectTiming {
    /// https://drafts.csswg.org/web-animations-2/#specified-start-delay
    pub start_delay: f64,
    /// https://drafts.csswg.org/web-animations-2/#specified-end-delay
    pub end_delay: f64,
    /// https://drafts.csswg.org/web-animations-2/#specified-iteration-duration
    pub iteration_duration: SpecifiedIterationDuration,
    /// https://www.w3.org/TR/web-animations-1/#iteration-start
    pub iteration_start: f64,
    /// https://www.w3.org/TR/web-animations-1/#iteration-count
    pub iteration_count: f64,
    /// https://www.w3.org/TR/web-animations-1/#fill-mode
    pub fill_mode: FillMode,
    /// https://www.w3.org/TR/web-animations-1/#playback-direction
    pub playback_direction: PlaybackDirection,
}

impl Default for SpecifiedEffectTiming {
    fn default() -> Self {
        Self {
            start_delay: 0.0,
            end_delay: 0.0,
            iteration_duration: SpecifiedIterationDuration::AUTO,
            iteration_start: 0.0,
            iteration_count: 1.0,
            fill_mode: FillMode::Auto,
            playback_direction: PlaybackDirection::Normal,
        }
    }
}

/// The timing properties of an animation effect the timing calculations use: the specified
/// timing normalized against the effect's timeline, every time in the timeline's unit.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AnimationEffectTiming {
    /// https://www.w3.org/TR/web-animations-1/#start-delay
    pub start_delay: TimeValue,
    /// https://www.w3.org/TR/web-animations-1/#end-delay
    pub end_delay: TimeValue,
    /// https://www.w3.org/TR/web-animations-1/#iteration-duration
    pub iteration_duration: TimeValue,
    /// https://www.w3.org/TR/web-animations-1/#iteration-start
    pub iteration_start: f64,
    /// https://www.w3.org/TR/web-animations-1/#iteration-count
    pub iteration_count: f64,
    /// https://www.w3.org/TR/web-animations-1/#fill-mode
    pub fill_mode: FillMode,
    /// https://www.w3.org/TR/web-animations-1/#playback-direction
    pub playback_direction: PlaybackDirection,
}

impl AnimationEffectTiming {
    /// The unit of the effect's times, which is that of its timeline.
    #[inline]
    #[must_use]
    pub fn time_unit(&self) -> TimeValueType {
        self.iteration_duration.r#type
    }

    /// A zero time in the effect's unit.
    #[inline]
    #[must_use]
    pub fn zero(&self) -> TimeValue {
        TimeValue::zero(self.time_unit())
    }
}

impl Default for AnimationEffectTiming {
    /// The timing of an effect associated with no animation, whose iteration duration is the
    /// intrinsic iteration duration of 0ms.
    fn default() -> Self {
        Self {
            start_delay: TimeValue::milliseconds(0.0),
            end_delay: TimeValue::milliseconds(0.0),
            iteration_duration: TimeValue::milliseconds(0.0),
            iteration_start: 0.0,
            iteration_count: 1.0,
            fill_mode: FillMode::Auto,
            playback_direction: PlaybackDirection::Normal,
        }
    }
}
