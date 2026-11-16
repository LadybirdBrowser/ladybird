/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the procedures of an animation read of its timeline.

use super::animation::AttachmentInterval;
use super::timing::time_value::max;
use super::timing::{TimeValue, TimeValueType, TimelineSample};

/// An animation's timeline as the animation sees it at one moment.
/// https://www.w3.org/TR/web-animations-1/#timelines
#[derive(Clone, Copy, Debug)]
pub(crate) struct TimelineState {
    /// What the timing calculations read of the timeline. Its current time is unresolved where the timeline is
    /// inactive.
    pub(crate) sample: TimelineSample,
    /// https://drafts.csswg.org/web-animations-2/#timeline-duration
    pub(crate) duration: Option<TimeValue>,
    /// The timeline times of the start and the end of the animation's attachment range, on a progress-based timeline.
    /// https://drafts.csswg.org/scroll-animations-1/#animation-attachment-range
    pub(crate) attachment_interval: Option<AttachmentInterval>,
    /// https://drafts.csswg.org/web-animations-2/#progress-based-timeline
    pub(crate) is_progress_based: bool,
}

impl TimelineState {
    /// https://www.w3.org/TR/web-animations-1/#timeline-current-time
    #[must_use]
    pub(crate) fn current_time(&self) -> Option<TimeValue> {
        self.sample.current_time.get()
    }

    /// https://www.w3.org/TR/web-animations-1/#inactive-timeline
    #[must_use]
    pub(crate) fn is_inactive(&self) -> bool {
        // A timeline is considered to be inactive when its time value is unresolved, and active otherwise.
        self.current_time().is_none()
    }

    #[must_use]
    pub(crate) fn attachment_interval(&self) -> Option<AttachmentInterval> {
        self.attachment_interval
    }

    /// The duration of the timeline as an animation attached to its attachment range sees it.
    #[must_use]
    pub(crate) fn duration_for_attachment_range(&self) -> Option<TimeValue> {
        // AD-HOC: The duration of a progress-based timeline, as an animation attached to a range of it sees it, is the
        //         length of its attachment range. This matches WebKit.
        //         An attachment range whose end comes before its start is empty.
        if let Some(interval) = self.attachment_interval() {
            return Some(max(interval.end - interval.start, TimeValue::percentage(0.0)));
        }
        self.duration
    }
}

/// The unit of the times of `timeline`, which is milliseconds without one.
#[must_use]
pub(crate) fn time_unit(timeline: Option<&TimelineState>) -> TimeValueType {
    time_unit_of(timeline.is_some_and(|timeline| timeline.is_progress_based))
}

/// The unit of the times of a timeline that is progress-based or not.
#[must_use]
pub(crate) fn time_unit_of(is_progress_based: bool) -> TimeValueType {
    if is_progress_based {
        return TimeValueType::Percentage;
    }
    TimeValueType::Milliseconds
}

/// A zero time in the unit of the times of `timeline`.
#[must_use]
pub(crate) fn zero_time(timeline: Option<&TimelineState>) -> TimeValue {
    TimeValue::zero(time_unit(timeline))
}
