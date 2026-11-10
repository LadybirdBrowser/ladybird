/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A fade that turns back from whatever opacity it has reached.

use crate::time::{Duration, MonotonicTime};

#[derive(Clone, Copy, Default)]
pub struct OverlayFade {
    opacity: f32,
    initial_opacity: f32,
    target_opacity: f32,
    started_at: Option<MonotonicTime>,
}

impl OverlayFade {
    pub const DURATION: Duration = Duration::from_milliseconds(150);

    pub fn fade_to(&mut self, target_opacity: f32, now: MonotonicTime) {
        if target_opacity == self.target_opacity {
            return;
        }
        self.initial_opacity = self.opacity;
        self.target_opacity = target_opacity;
        self.started_at = Some(now);
    }

    /// Ends any fade, with the opacity at once where it would end.
    pub fn jump_to(&mut self, opacity: f32) {
        self.opacity = opacity;
        self.initial_opacity = opacity;
        self.target_opacity = opacity;
        self.started_at = None;
    }

    /// Returns whether the opacity changed.
    pub fn advance_to(&mut self, frame_time: MonotonicTime) -> bool {
        let opacity = self.opacity_at(frame_time);
        if opacity == self.opacity {
            return false;
        }
        self.opacity = opacity;
        true
    }

    pub fn opacity(&self) -> f32 {
        self.opacity
    }

    pub fn is_fading(&self) -> bool {
        self.opacity != self.target_opacity
    }

    fn opacity_at(&self, frame_time: MonotonicTime) -> f32 {
        let Some(started_at) = self.started_at else {
            return self.target_opacity;
        };
        let mut progress = (frame_time - started_at).to_seconds_f64() / Self::DURATION.to_seconds_f64();
        if progress >= 1.0 {
            return self.target_opacity;
        }
        progress = if progress < 0.0 { 0.0 } else { progress };

        // Smoothstep, so that the fade starts and ends gently.
        let eased_progress = (progress * progress * (3.0 - 2.0 * progress)) as f32;
        self.initial_opacity + (self.target_opacity - self.initial_opacity) * eased_progress
    }
}
