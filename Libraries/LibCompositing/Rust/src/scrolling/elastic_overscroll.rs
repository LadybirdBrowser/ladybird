/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How far a touchpad has pulled one scroller past the edges of its scroll range, and how the scroller springs back.
// A stretch has the sign of the scroll offset direction it extends: a negative y is a scroller pulled past its top.
// Deltas, stretches and the scrollport are in the same pixels. Velocities are in those pixels per second.

use super::max;
use crate::time::MonotonicTime;
use ak::math;
use libgfx_rust::{FloatPoint, FloatSize};

// A pull stretches by the overscroll divided by the stiffness, and a released stretch S0 with velocity V0 is
// (S0 + amplitude * V0 * t) * exp(-t * stiffness / period) after t seconds.
const RUBBER_BAND_STIFFNESS: f64 = 20.0;
const RUBBER_BAND_AMPLITUDE: f64 = 0.31;
const RUBBER_BAND_PERIOD: f64 = 1.6;
const REBOUND_DECAY_PER_SECOND: f64 = RUBBER_BAND_STIFFNESS / RUBBER_BAND_PERIOD;

const LARGEST_BOUNCE_AS_SHARE_OF_SCROLLPORT: f64 = 0.2;
const SCREEN_PIXELS_OF_STRETCH_THAT_COUNT_AS_SPRUNG_BACK: f32 = 1.0;

pub struct ElasticOverscroll {
    scrollport_size: FloatSize,
    screen_pixels_per_pixel: f32,
    held_stretch: FloatPoint,
    rebound: Option<Rebound>,
}

struct Rebound {
    started_at: MonotonicTime,
    initial_stretch: FloatPoint,
    initial_velocity: FloatPoint,
    current_stretch: FloatPoint,
}

// A scroller released at its edge with speed V peaks at amplitude * V / (decay * e). Wheel events carry no
// timestamps, so the speed measured for events that were handled in a burst can be far above any real one.
fn fastest_release_speed_for(scrollport_length: f32) -> f32 {
    (LARGEST_BOUNCE_AS_SHARE_OF_SCROLLPORT * scrollport_length as f64 * REBOUND_DECAY_PER_SECOND * std::f64::consts::E
        / RUBBER_BAND_AMPLITUDE) as f32
}

impl ElasticOverscroll {
    /// A scroller that is shown magnified takes up `screen_pixels_per_pixel` pixels of the screen with each of its own.
    pub fn new(scrollport_size: FloatSize, screen_pixels_per_pixel: f32) -> Self {
        Self {
            scrollport_size,
            screen_pixels_per_pixel,
            held_stretch: FloatPoint::default(),
            rebound: None,
        }
    }

    /// Whole pixels of the screen, like the scroll offsets the stretch is presented with.
    pub fn stretch(&self) -> FloatPoint {
        let unrounded_stretch = self
            .rebound
            .as_ref()
            .map_or(self.held_stretch, |rebound| rebound.current_stretch);
        FloatPoint {
            x: (unrounded_stretch.x * self.screen_pixels_per_pixel).trunc() / self.screen_pixels_per_pixel,
            y: (unrounded_stretch.y * self.screen_pixels_per_pixel).trunc() / self.screen_pixels_per_pixel,
        }
    }
}

fn relax_axis_by(stretch: &mut f32, delta: f32) -> f32 {
    let delta_points_back_to_scroll_range = (*stretch > 0.0 && delta < 0.0) || (*stretch < 0.0 && delta > 0.0);
    if !delta_points_back_to_scroll_range {
        return delta;
    }
    if delta.abs() <= stretch.abs() {
        *stretch += delta;
        return 0.0;
    }
    let delta_left_to_scroll_with = delta + *stretch;
    *stretch = 0.0;
    delta_left_to_scroll_with
}

impl ElasticOverscroll {
    /// The fingers moved by `delta`. The part of it that points back to the scroll range relaxes the stretch, and the
    /// rest is returned to scroll with. The fingers take a rebound over from where it has got to.
    pub fn relax_by(&mut self, delta: FloatPoint) -> FloatPoint {
        if let Some(rebound) = self.rebound.take() {
            self.held_stretch = rebound.current_stretch;
        }
        let mut held_stretch_x = self.held_stretch.x;
        let mut held_stretch_y = self.held_stretch.y;
        let delta_left_to_scroll_with = FloatPoint {
            x: relax_axis_by(&mut held_stretch_x, delta.x),
            y: relax_axis_by(&mut held_stretch_y, delta.y),
        };
        self.held_stretch = FloatPoint {
            x: held_stretch_x,
            y: held_stretch_y,
        };
        delta_left_to_scroll_with
    }

    /// The fingers moved by `overscroll_delta` further past an edge the scroller is already at.
    pub fn pull_by(&mut self, overscroll_delta: FloatPoint) {
        assert!(self.rebound.is_none());
        let stretch_delta = overscroll_delta.scaled((1.0 / RUBBER_BAND_STIFFNESS) as f32);
        self.held_stretch = FloatPoint {
            x: (self.held_stretch.x + stretch_delta.x).clamp(-self.scrollport_size.width, self.scrollport_size.width),
            y: (self.held_stretch.y + stretch_delta.y).clamp(-self.scrollport_size.height, self.scrollport_size.height),
        };
    }

    /// The fingers lifted, or momentum carried the scroller into its edge.
    pub fn release(&mut self, velocity: FloatPoint, now: MonotonicTime) {
        if self.rebound.is_some() {
            return;
        }
        let initial_stretch = self.held_stretch;
        self.held_stretch = FloatPoint::default();

        // Only an axis that is stretched is at its edge. The other one must not bounce off an edge it is not at.
        let release_velocity_for_axis = |stretch: f32, velocity: f32, scrollport_length: f32| -> f32 {
            if stretch == 0.0 {
                return 0.0;
            }
            let fastest_release_speed = fastest_release_speed_for(scrollport_length);
            velocity.clamp(-fastest_release_speed, fastest_release_speed)
        };
        self.rebound = Some(Rebound {
            started_at: now,
            initial_stretch,
            initial_velocity: FloatPoint {
                x: release_velocity_for_axis(initial_stretch.x, velocity.x, self.scrollport_size.width),
                y: release_velocity_for_axis(initial_stretch.y, velocity.y, self.scrollport_size.height),
            },
            current_stretch: initial_stretch,
        });
    }
}

struct ReboundAxisSample {
    stretch: f32,
    has_sprung_back: bool,
}

impl Default for ReboundAxisSample {
    fn default() -> Self {
        Self {
            stretch: 0.0,
            has_sprung_back: true,
        }
    }
}

fn sample_rebound_axis(
    initial_stretch: f32,
    initial_velocity: f32,
    scrollport_length: f32,
    stretch_that_counts_as_sprung_back: f32,
    elapsed_seconds: f64,
) -> ReboundAxisSample {
    if initial_stretch == 0.0 {
        return ReboundAxisSample::default();
    }
    let stretch_before_decay =
        initial_stretch as f64 + RUBBER_BAND_AMPLITUDE * initial_velocity as f64 * elapsed_seconds;
    // A release velocity that points back to the scroll range must not carry the content past the edge it left.
    let crossed_the_edge = (stretch_before_decay > 0.0) != (initial_stretch > 0.0);
    if crossed_the_edge {
        return ReboundAxisSample::default();
    }
    let mut stretch = (stretch_before_decay * math::exp(-elapsed_seconds * REBOUND_DECAY_PER_SECOND)) as f32;
    stretch = stretch.clamp(-scrollport_length, scrollport_length);

    // A bounce starts with almost no stretch, so a small stretch only means the rebound is over once it shrinks.
    let stretch_growth_before_decay =
        RUBBER_BAND_AMPLITUDE * initial_velocity as f64 - REBOUND_DECAY_PER_SECOND * stretch_before_decay;
    let is_still_stretching = stretch_before_decay * stretch_growth_before_decay > 0.0;
    ReboundAxisSample {
        stretch,
        has_sprung_back: !is_still_stretching && stretch.abs() < stretch_that_counts_as_sprung_back,
    }
}

impl ElasticOverscroll {
    /// Returns false once the scroller has sprung back.
    pub fn advance_rebound_to(&mut self, frame_time: MonotonicTime) -> bool {
        let Some(rebound) = self.rebound.as_mut() else {
            return false;
        };

        // The time of a display tick can be earlier than the event that released the stretch.
        let elapsed_seconds = max(0.0, (frame_time - rebound.started_at).to_seconds_f64());
        let stretch_that_counts_as_sprung_back =
            SCREEN_PIXELS_OF_STRETCH_THAT_COUNT_AS_SPRUNG_BACK / self.screen_pixels_per_pixel;
        let horizontal = sample_rebound_axis(
            rebound.initial_stretch.x,
            rebound.initial_velocity.x,
            self.scrollport_size.width,
            stretch_that_counts_as_sprung_back,
            elapsed_seconds,
        );
        let vertical = sample_rebound_axis(
            rebound.initial_stretch.y,
            rebound.initial_velocity.y,
            self.scrollport_size.height,
            stretch_that_counts_as_sprung_back,
            elapsed_seconds,
        );
        if horizontal.has_sprung_back && vertical.has_sprung_back {
            self.rebound = None;
            return false;
        }
        rebound.current_stretch = FloatPoint {
            x: if horizontal.has_sprung_back {
                0.0
            } else {
                horizontal.stretch
            },
            y: if vertical.has_sprung_back {
                0.0
            } else {
                vertical.stretch
            },
        };
        true
    }

    pub fn is_at_rest(&self) -> bool {
        self.rebound.is_none() && self.held_stretch.is_zero()
    }
    pub fn is_rebounding(&self) -> bool {
        self.rebound.is_some()
    }
}
