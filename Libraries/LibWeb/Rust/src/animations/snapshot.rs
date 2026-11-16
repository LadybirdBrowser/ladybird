/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The timing of an animation effect as the host publishes it to the style engine, which samples the
//! effect from it wherever the engine runs.

use super::timeline::AnimationTimelineId;
use super::timing::{
    AnimationEffectTiming, AnimationPlayback, AnimationReplaceState, FillMode, OptionalNumber, OptionalTimeValue,
    PlaybackDirection, TimeValue, TimeValueType, TimelineSample,
};

/// What moves an effect's timeline as a clock tick samples it.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TimelineClockSource {
    /// Nothing a tick moves: the effect's animation has no timeline, or one no tick moves.
    #[default]
    Unticked,
    /// A document timeline, whose time is a timestamp less the clock's origin time, in milliseconds.
    Document,
    /// A scroll timeline, whose time is the progress, in percent, of the scroller the clock's scroll
    /// timeline follows.
    Scroll,
}

/// What moves an effect's timeline as a clock tick samples it, and where from.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TimelineClock {
    pub source: TimelineClockSource,
    /// The origin time of a document timeline.
    pub origin_time: f64,
    /// The identity of a scroll timeline.
    pub scroll_timeline: AnimationTimelineId,
}

/// The progress, in percent, of a scroll timeline, which is its time.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScrollProgress {
    pub timeline: AnimationTimelineId,
    pub progress: f64,
}

impl TimelineClock {
    /// The time of the timeline at a tick at `timestamp`, which moves every scroll timeline in
    /// `scroll_progress`, sorted by timeline, to its progress, or `None` where the tick does not move
    /// the timeline.
    #[inline]
    #[must_use]
    pub fn time_at_tick(&self, timestamp: f64, scroll_progress: &[ScrollProgress]) -> Option<TimeValue> {
        match self.source {
            TimelineClockSource::Unticked => None,
            TimelineClockSource::Document => Some(TimeValue::new(
                TimeValueType::Milliseconds,
                timestamp - self.origin_time,
            )),
            TimelineClockSource::Scroll => scroll_progress
                .binary_search_by_key(&self.scroll_timeline, |scroll| scroll.timeline)
                .ok()
                .map(|index| TimeValue::new(TimeValueType::Percentage, scroll_progress[index].progress)),
        }
    }
}

/// The timing of an animation effect as the host holds it when it samples the effect: the effect's own
/// normalized timing, the playback state of its animation, its animation's timeline as it stood, and
/// what moves that timeline as a clock tick samples it.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EffectTimingSnapshot {
    pub effect: AnimationEffectTiming,
    pub playback: AnimationPlayback,
    /// The animation's timeline, where `has_timeline`.
    pub timeline: TimelineSample,
    pub has_timeline: bool,
    pub clock: TimelineClock,
}

/// Bits of `CompactEffectTiming`, one for each time it holds, which mark the time a percentage in `units` and the
/// time resolved in `resolved`.
mod time_bit {
    pub(super) const START_DELAY: u8 = 1 << 0;
    pub(super) const END_DELAY: u8 = 1 << 1;
    pub(super) const ITERATION_DURATION: u8 = 1 << 2;
    pub(super) const START_TIME: u8 = 1 << 3;
    pub(super) const HOLD_TIME: u8 = 1 << 4;
    pub(super) const TIMELINE_TIME: u8 = 1 << 5;
    pub(super) const MINIMUM_TIMELINE_TIME: u8 = 1 << 6;
    pub(super) const MAXIMUM_TIMELINE_TIME: u8 = 1 << 7;
}

/// Bits of `CompactEffectTiming::flags`.
mod flag {
    pub(super) const HAS_PENDING_PLAYBACK_RATE: u8 = 1 << 0;
    pub(super) const HAS_PENDING_PLAY_TASK: u8 = 1 << 1;
    pub(super) const HAS_PENDING_PAUSE_TASK: u8 = 1 << 2;
    pub(super) const IS_FINISHED: u8 = 1 << 3;
    pub(super) const HAS_TIMELINE: u8 = 1 << 4;
    pub(super) const IS_MONOTONICALLY_INCREASING: u8 = 1 << 5;
}

/// An `EffectTimingSnapshot` as the style engine keeps it for every effect it describes: each time a bare number,
/// with its unit and whether it is resolved in a bit each, and the origin time of a document timeline and the
/// identity of a scroll timeline in one place, as a timeline has at most one of them. It expands to the timing
/// model's types wherever the engine samples the effect.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct CompactEffectTiming {
    start_delay: f64,
    end_delay: f64,
    iteration_duration: f64,
    iteration_start: f64,
    iteration_count: f64,
    start_time: f64,
    hold_time: f64,
    playback_rate: f64,
    pending_playback_rate: f64,
    timeline_time: f64,
    minimum_timeline_time: f64,
    maximum_timeline_time: f64,
    /// The origin time of a document timeline, as its bits, or the identity of a scroll timeline.
    clock_reference: u64,
    /// The times that are percentages, by `time_bit`; the others are milliseconds.
    units: u8,
    /// The optional times that are resolved, by `time_bit`.
    resolved: u8,
    flags: u8,
    fill_mode: FillMode,
    playback_direction: PlaybackDirection,
    replace_state: AnimationReplaceState,
    clock_source: TimelineClockSource,
}

impl CompactEffectTiming {
    #[inline]
    fn time(&self, value: f64, bit: u8) -> TimeValue {
        let unit = match self.units & bit {
            0 => TimeValueType::Milliseconds,
            _ => TimeValueType::Percentage,
        };
        TimeValue::new(unit, value)
    }

    #[inline]
    fn optional_time(&self, value: f64, bit: u8) -> OptionalTimeValue {
        OptionalTimeValue {
            value: self.time(value, bit),
            has_value: self.resolved & bit != 0,
        }
    }

    #[inline]
    fn has(&self, flag: u8) -> bool {
        self.flags & flag != 0
    }

    /// The effect's own normalized timing.
    #[inline]
    #[must_use]
    pub(crate) fn effect(&self) -> AnimationEffectTiming {
        AnimationEffectTiming {
            start_delay: self.time(self.start_delay, time_bit::START_DELAY),
            end_delay: self.time(self.end_delay, time_bit::END_DELAY),
            iteration_duration: self.time(self.iteration_duration, time_bit::ITERATION_DURATION),
            iteration_start: self.iteration_start,
            iteration_count: self.iteration_count,
            fill_mode: self.fill_mode,
            playback_direction: self.playback_direction,
        }
    }

    /// The playback state of the effect's animation.
    #[inline]
    #[must_use]
    pub(crate) fn playback(&self) -> AnimationPlayback {
        AnimationPlayback {
            start_time: self.optional_time(self.start_time, time_bit::START_TIME),
            hold_time: self.optional_time(self.hold_time, time_bit::HOLD_TIME),
            playback_rate: self.playback_rate,
            pending_playback_rate: OptionalNumber {
                value: self.pending_playback_rate,
                has_value: self.has(flag::HAS_PENDING_PLAYBACK_RATE),
            },
            has_pending_play_task: self.has(flag::HAS_PENDING_PLAY_TASK),
            has_pending_pause_task: self.has(flag::HAS_PENDING_PAUSE_TASK),
            is_finished: self.has(flag::IS_FINISHED),
            replace_state: self.replace_state,
        }
    }

    /// The animation's timeline as the host sampled it, if it has one.
    #[inline]
    #[must_use]
    pub(crate) fn timeline(&self) -> Option<TimelineSample> {
        self.has(flag::HAS_TIMELINE).then(|| self.timeline_sample())
    }

    #[inline]
    fn timeline_sample(&self) -> TimelineSample {
        TimelineSample {
            current_time: self.optional_time(self.timeline_time, time_bit::TIMELINE_TIME),
            minimum_timeline_time: self.optional_time(self.minimum_timeline_time, time_bit::MINIMUM_TIMELINE_TIME),
            maximum_timeline_time: self.optional_time(self.maximum_timeline_time, time_bit::MAXIMUM_TIMELINE_TIME),
            is_monotonically_increasing: self.has(flag::IS_MONOTONICALLY_INCREASING),
        }
    }

    /// The current time of the animation's timeline as the host sampled it, which is unresolved without a timeline.
    #[inline]
    #[must_use]
    pub(crate) fn timeline_time(&self) -> Option<TimeValue> {
        if !self.has(flag::HAS_TIMELINE) {
            return None;
        }
        self.optional_time(self.timeline_time, time_bit::TIMELINE_TIME).get()
    }

    /// What moves the animation's timeline as a clock tick samples it, and where from.
    #[inline]
    #[must_use]
    pub(crate) fn clock(&self) -> TimelineClock {
        match self.clock_source {
            TimelineClockSource::Unticked => TimelineClock::default(),
            TimelineClockSource::Document => TimelineClock {
                source: TimelineClockSource::Document,
                origin_time: f64::from_bits(self.clock_reference),
                scroll_timeline: AnimationTimelineId::default(),
            },
            TimelineClockSource::Scroll => TimelineClock {
                source: TimelineClockSource::Scroll,
                origin_time: 0.0,
                scroll_timeline: AnimationTimelineId(self.clock_reference),
            },
        }
    }

    /// Sets the hold time of the effect's animation.
    #[inline]
    pub(crate) fn set_hold_time(&mut self, hold_time: Option<TimeValue>) {
        let hold_time = OptionalTimeValue::from(hold_time);
        self.set_time(&hold_time.value, time_bit::HOLD_TIME);
        self.hold_time = hold_time.value.value;
        self.set_resolved(hold_time.has_value, time_bit::HOLD_TIME);
    }

    #[inline]
    fn set_time(&mut self, time: &TimeValue, bit: u8) {
        match time.r#type {
            TimeValueType::Milliseconds => self.units &= !bit,
            TimeValueType::Percentage => self.units |= bit,
        }
    }

    #[inline]
    fn set_resolved(&mut self, resolved: bool, bit: u8) {
        match resolved {
            true => self.resolved |= bit,
            false => self.resolved &= !bit,
        }
    }

    /// The timing as the host published it.
    #[cfg(test)]
    #[must_use]
    pub(crate) fn snapshot(&self) -> EffectTimingSnapshot {
        EffectTimingSnapshot {
            effect: self.effect(),
            playback: self.playback(),
            timeline: self.timeline_sample(),
            has_timeline: self.has(flag::HAS_TIMELINE),
            clock: self.clock(),
        }
    }
}

impl From<&EffectTimingSnapshot> for CompactEffectTiming {
    fn from(snapshot: &EffectTimingSnapshot) -> Self {
        let EffectTimingSnapshot {
            effect,
            playback,
            timeline,
            has_timeline,
            clock,
        } = snapshot;
        let mut compact = Self {
            start_delay: effect.start_delay.value,
            end_delay: effect.end_delay.value,
            iteration_duration: effect.iteration_duration.value,
            iteration_start: effect.iteration_start,
            iteration_count: effect.iteration_count,
            start_time: playback.start_time.value.value,
            hold_time: playback.hold_time.value.value,
            playback_rate: playback.playback_rate,
            pending_playback_rate: playback.pending_playback_rate.value,
            timeline_time: timeline.current_time.value.value,
            minimum_timeline_time: timeline.minimum_timeline_time.value.value,
            maximum_timeline_time: timeline.maximum_timeline_time.value.value,
            clock_reference: match clock.source {
                TimelineClockSource::Unticked => 0,
                TimelineClockSource::Document => clock.origin_time.to_bits(),
                TimelineClockSource::Scroll => clock.scroll_timeline.0,
            },
            units: 0,
            resolved: 0,
            flags: 0,
            fill_mode: effect.fill_mode,
            playback_direction: effect.playback_direction,
            replace_state: playback.replace_state,
            clock_source: clock.source,
        };
        compact.set_time(&effect.start_delay, time_bit::START_DELAY);
        compact.set_time(&effect.end_delay, time_bit::END_DELAY);
        compact.set_time(&effect.iteration_duration, time_bit::ITERATION_DURATION);
        for (time, bit) in [
            (&playback.start_time, time_bit::START_TIME),
            (&playback.hold_time, time_bit::HOLD_TIME),
            (&timeline.current_time, time_bit::TIMELINE_TIME),
            (&timeline.minimum_timeline_time, time_bit::MINIMUM_TIMELINE_TIME),
            (&timeline.maximum_timeline_time, time_bit::MAXIMUM_TIMELINE_TIME),
        ] {
            compact.set_time(&time.value, bit);
            compact.set_resolved(time.has_value, bit);
        }
        for (set, flag) in [
            (
                playback.pending_playback_rate.has_value,
                flag::HAS_PENDING_PLAYBACK_RATE,
            ),
            (playback.has_pending_play_task, flag::HAS_PENDING_PLAY_TASK),
            (playback.has_pending_pause_task, flag::HAS_PENDING_PAUSE_TASK),
            (playback.is_finished, flag::IS_FINISHED),
            (*has_timeline, flag::HAS_TIMELINE),
            (timeline.is_monotonically_increasing, flag::IS_MONOTONICALLY_INCREASING),
        ] {
            if set {
                compact.flags |= flag;
            }
        }
        compact
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ms(time: f64) -> OptionalTimeValue {
        Some(TimeValue::milliseconds(time)).into()
    }

    fn percent(time: f64) -> OptionalTimeValue {
        Some(TimeValue::percentage(time)).into()
    }

    #[test]
    fn the_packed_timing_round_trips() {
        let document_timeline = EffectTimingSnapshot {
            effect: AnimationEffectTiming {
                start_delay: TimeValue::milliseconds(-250.0),
                end_delay: TimeValue::milliseconds(125.0),
                iteration_duration: TimeValue::milliseconds(1000.0),
                iteration_start: 0.5,
                iteration_count: f64::INFINITY,
                fill_mode: FillMode::Both,
                playback_direction: PlaybackDirection::AlternateReverse,
            },
            playback: AnimationPlayback {
                start_time: ms(40.0),
                hold_time: ms(700.0),
                playback_rate: -2.0,
                pending_playback_rate: Some(0.5).into(),
                has_pending_play_task: true,
                has_pending_pause_task: false,
                is_finished: true,
                replace_state: AnimationReplaceState::Persisted,
            },
            timeline: TimelineSample {
                current_time: ms(1234.5),
                is_monotonically_increasing: true,
                ..TimelineSample::default()
            },
            has_timeline: true,
            clock: TimelineClock {
                source: TimelineClockSource::Document,
                origin_time: 16.75,
                scroll_timeline: AnimationTimelineId::default(),
            },
        };
        let scroll_timeline = EffectTimingSnapshot {
            effect: AnimationEffectTiming {
                start_delay: TimeValue::percentage(10.0),
                end_delay: TimeValue::percentage(5.0),
                iteration_duration: TimeValue::percentage(85.0),
                fill_mode: FillMode::Backwards,
                ..AnimationEffectTiming::default()
            },
            playback: AnimationPlayback {
                start_time: percent(20.0),
                has_pending_pause_task: true,
                replace_state: AnimationReplaceState::Removed,
                ..AnimationPlayback::default()
            },
            timeline: TimelineSample {
                current_time: OptionalTimeValue::from(None),
                minimum_timeline_time: percent(0.0),
                maximum_timeline_time: percent(100.0),
                is_monotonically_increasing: false,
            },
            has_timeline: true,
            clock: TimelineClock {
                source: TimelineClockSource::Scroll,
                origin_time: 0.0,
                scroll_timeline: AnimationTimelineId(u64::MAX - 3),
            },
        };
        for snapshot in [document_timeline, scroll_timeline, EffectTimingSnapshot::default()] {
            let compact = CompactEffectTiming::from(&snapshot);
            assert_eq!(compact.snapshot(), snapshot);
            assert_eq!(compact.timeline(), snapshot.has_timeline.then_some(snapshot.timeline));
        }
    }
}
