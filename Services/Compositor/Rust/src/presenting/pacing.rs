/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Display ticks paced to a maximum frame rate.

use libcompositing_rust::time::MonotonicTime;

/// A display tick that was asked for and not delivered yet, paced to the rate it was last asked for at.
pub struct PacedTickRequest {
    requested: bool,
    maximum_frames_per_second: f64,
    last_frame_time: Option<MonotonicTime>,
}

impl Default for PacedTickRequest {
    fn default() -> Self {
        Self {
            requested: false,
            maximum_frames_per_second: 60.0,
            last_frame_time: None,
        }
    }
}

impl PacedTickRequest {
    /// Returns whether the tick had not been asked for already.
    pub fn request(&mut self, maximum_frames_per_second: f64) -> bool {
        assert!(maximum_frames_per_second.is_finite() && maximum_frames_per_second > 0.0);
        self.maximum_frames_per_second = maximum_frames_per_second;
        !std::mem::replace(&mut self.requested, true)
    }

    pub fn is_requested(&self) -> bool {
        self.requested
    }

    /// The whole number of display ticks between two frames, in milliseconds.
    pub fn frame_interval(&self, display_refresh_rate: f64) -> f64 {
        let ticks_per_frame = (display_refresh_rate / self.maximum_frames_per_second).ceil().max(1.0);
        ticks_per_frame * 1000.0 / display_refresh_rate
    }

    pub fn is_due(&self, frame_time: MonotonicTime, display_refresh_rate: f64) -> bool {
        let Some(last_frame_time) = self.last_frame_time else {
            return true;
        };
        let interval = self.frame_interval(display_refresh_rate);
        let display_interval = 1000.0 / display_refresh_rate;
        let elapsed = (frame_time.nanoseconds() - last_frame_time.nanoseconds()) as f64 / 1_000_000.0;
        elapsed + display_interval / 2.0 >= interval
    }

    pub fn did_deliver(&mut self, frame_time: MonotonicTime) {
        assert!(self.requested);
        self.requested = false;
        self.last_frame_time = Some(frame_time);
    }
}
