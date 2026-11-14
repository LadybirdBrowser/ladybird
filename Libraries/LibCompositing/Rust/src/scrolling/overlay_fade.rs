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

#[cfg(test)]
mod tests {
    use super::*;

    const START_NANOSECONDS: i64 = 1_000_000_000;

    fn after(milliseconds: i64) -> MonotonicTime {
        MonotonicTime::from_nanoseconds(START_NANOSECONDS + milliseconds * 1_000_000)
    }

    fn assert_approximately(actual: f32, expected: f32, error: f32) {
        assert!(
            (actual - expected).abs() <= error,
            "{actual} is not within {error} of {expected}"
        );
    }

    #[test]
    fn a_fade_starts_transparent_and_at_rest() {
        let fade = OverlayFade::default();
        assert_eq!(fade.opacity(), 0.0);
        assert!(!fade.is_fading());
    }

    #[test]
    fn a_fade_eases_in_and_out_to_its_target() {
        let mut fade = OverlayFade::default();
        fade.fade_to(1.0, after(0));
        assert!(fade.is_fading());

        assert!(!fade.advance_to(after(0)));
        assert_eq!(fade.opacity(), 0.0);

        assert!(fade.advance_to(after(30)));
        assert_approximately(fade.opacity(), 0.104, 0.0001);

        fade.advance_to(after(75));
        assert_approximately(fade.opacity(), 0.5, 0.000001);

        fade.advance_to(after(150));
        assert_eq!(fade.opacity(), 1.0);
        assert!(!fade.is_fading());

        assert!(!fade.advance_to(after(300)));
        assert_eq!(fade.opacity(), 1.0);
    }

    #[test]
    fn a_fade_does_not_go_back_in_time() {
        let mut fade = OverlayFade::default();
        fade.fade_to(1.0, after(0));

        assert!(!fade.advance_to(after(-10)));
        assert_eq!(fade.opacity(), 0.0);
    }

    #[test]
    fn a_fade_turns_back_from_where_it_has_reached() {
        let mut fade = OverlayFade::default();
        fade.fade_to(1.0, after(0));
        fade.advance_to(after(75));

        fade.fade_to(0.0, after(75));
        assert!(!fade.advance_to(after(75)));
        assert_approximately(fade.opacity(), 0.5, 0.000001);

        fade.advance_to(after(150));
        assert_approximately(fade.opacity(), 0.25, 0.000001);

        fade.advance_to(after(225));
        assert_eq!(fade.opacity(), 0.0);
        assert!(!fade.is_fading());
    }

    #[test]
    fn fading_to_the_same_target_keeps_the_fade_going() {
        let mut fade = OverlayFade::default();
        fade.fade_to(1.0, after(0));
        fade.advance_to(after(75));

        fade.fade_to(1.0, after(75));
        fade.advance_to(after(150));
        assert_eq!(fade.opacity(), 1.0);
    }
}
