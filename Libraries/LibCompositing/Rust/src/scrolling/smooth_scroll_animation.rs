/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{ScrollAnimationKind, min};
use crate::time::Duration;
use ak::math;
use libgfx_rust::FloatPoint;

// During a momentum scroll, each frame moves this fraction of the distance of the frame before it.
pub const MOMENTUM_DISTANCE_SHARE_PER_FRAME: f64 = 0.92;
pub const MOMENTUM_FRAME_DURATION_IN_SECONDS: f64 = 0.016;
pub const MAXIMUM_MOMENTUM_DURATION_IN_SECONDS: f64 = 5.0;

const SCROLL_SPEED_IN_PIXELS_PER_SECOND: f64 = 1000.0;
const MAXIMUM_SCROLL_DURATION_IN_SECONDS: f64 = 0.2;

// A wheel step takes longer the shorter its distance, within these bounds.
const MINIMUM_WHEEL_SCROLL_DURATION_IN_SECONDS: f64 = 0.1;
const MAXIMUM_WHEEL_SCROLL_DURATION_IN_SECONDS: f64 = 0.2;
const WHEEL_SCROLL_DISTANCE_COVERED_AT_MAXIMUM_DURATION: f64 = 120.0;
const WHEEL_SCROLL_DURATION_REDUCTION_PER_PIXEL: f64 = 1.0 / 3600.0;

// INTEROP: The ease-in-out curve WebKit uses for programmatic smooth scrolling, cubic-bezier(0.42, 0, 0.58, 1). A retargeted
// scroll changes y1 so that it keeps the speed it had.
const EASING_CONTROL_POINT_X1: f64 = 0.42;
const EASING_CONTROL_POINT_X2: f64 = 0.58;
const EASING_CONTROL_POINT_Y2: f64 = 1.0;

const MAXIMUM_EASING_SLOPE: f64 = 1000.0;
const RETARGET_EASE_OUT_ALLOWANCE: f64 = 2.5;

#[derive(Default)]
pub struct Sample {
    pub offset: FloatPoint,
    pub complete: bool,
}

pub struct SmoothScrollAnimation {
    start_offset: FloatPoint,
    destination_offset: FloatPoint,
    pixels_per_css_pixel: f64,
    start_time: Duration,
    duration: Duration,
    kind: ScrollAnimationKind,
    easing_control_point_y1: f64,
}

// https://drafts.csswg.org/css-easing/#cubic-bezier-algo
fn cubic_bezier(t: f64, p1: f64, p2: f64) -> f64 {
    3.0 * (1.0 - t) * (1.0 - t) * t * p1 + 3.0 * (1.0 - t) * t * t * p2 + t * t * t
}

fn cubic_bezier_derivative(t: f64, p1: f64, p2: f64) -> f64 {
    3.0 * (1.0 - t) * (1.0 - t) * p1 + 6.0 * (1.0 - t) * t * (p2 - p1) + 3.0 * t * t * (1.0 - p2)
}

fn curve_parameter_for_progress(progress: f64) -> f64 {
    // Find the curve parameter whose horizontal position is the progress: a few Newton steps usually land on it, and
    // bisection finishes the job when the slope gets too flat for them.
    let mut t = progress;
    for _ in 0..8 {
        let x = cubic_bezier(t, EASING_CONTROL_POINT_X1, EASING_CONTROL_POINT_X2) - progress;
        if x.abs() < 1e-7 {
            return t;
        }
        let slope = cubic_bezier_derivative(t, EASING_CONTROL_POINT_X1, EASING_CONTROL_POINT_X2);
        if slope.abs() < 1e-6 {
            break;
        }
        t -= x / slope;
    }
    let mut low = 0.0;
    let mut high = 1.0;
    while high - low > 1e-7 {
        t = (low + high) / 2.0;
        if cubic_bezier(t, EASING_CONTROL_POINT_X1, EASING_CONTROL_POINT_X2) < progress {
            low = t;
        } else {
            high = t;
        }
    }
    t
}

fn momentum_frames_for_distance(distance: f64) -> f64 {
    let frames = (-math::log(1.0 - distance * (1.0 - 1.0 / MOMENTUM_DISTANCE_SHARE_PER_FRAME))
        / math::log(MOMENTUM_DISTANCE_SHARE_PER_FRAME))
    .ceil();
    min(
        frames,
        MAXIMUM_MOMENTUM_DURATION_IN_SECONDS / MOMENTUM_FRAME_DURATION_IN_SECONDS,
    )
}

impl SmoothScrollAnimation {
    pub fn new(
        start_offset: FloatPoint,
        destination_offset: FloatPoint,
        pixels_per_css_pixel: f64,
        kind: ScrollAnimationKind,
    ) -> Self {
        assert!(pixels_per_css_pixel > 0.0);
        let mut animation = Self {
            start_offset,
            destination_offset,
            pixels_per_css_pixel,
            start_time: Duration::zero(),
            duration: Duration::zero(),
            kind,
            easing_control_point_y1: 0.0,
        };
        animation.duration = animation.duration_for_distance(destination_offset - start_offset);
        animation
    }

    fn duration_for_distance(&self, distance: FloatPoint) -> Duration {
        let horizontal_distance = distance.x as f64 / self.pixels_per_css_pixel;
        let vertical_distance = distance.y as f64 / self.pixels_per_css_pixel;
        let length = (horizontal_distance * horizontal_distance + vertical_distance * vertical_distance).sqrt();
        if length == 0.0 {
            return Duration::zero();
        }
        let duration_in_seconds = match self.kind {
            ScrollAnimationKind::SmoothScroll => min(
                length / SCROLL_SPEED_IN_PIXELS_PER_SECOND,
                MAXIMUM_SCROLL_DURATION_IN_SECONDS,
            ),
            ScrollAnimationKind::Wheel => (MAXIMUM_WHEEL_SCROLL_DURATION_IN_SECONDS
                - (length - WHEEL_SCROLL_DISTANCE_COVERED_AT_MAXIMUM_DURATION)
                    * WHEEL_SCROLL_DURATION_REDUCTION_PER_PIXEL)
                .clamp(
                    MINIMUM_WHEEL_SCROLL_DURATION_IN_SECONDS,
                    MAXIMUM_WHEEL_SCROLL_DURATION_IN_SECONDS,
                ),
            ScrollAnimationKind::Momentum => momentum_frames_for_distance(length) * MOMENTUM_FRAME_DURATION_IN_SECONDS,
        };
        Duration::from_seconds_f64(duration_in_seconds)
    }

    fn eased_progress_at(&self, progress: f64) -> f64 {
        if self.kind == ScrollAnimationKind::Momentum {
            let frames = self.duration.to_seconds_f64() / MOMENTUM_FRAME_DURATION_IN_SECONDS;
            let frames_elapsed = progress * frames;
            return (1.0 - math::pow(MOMENTUM_DISTANCE_SHARE_PER_FRAME, frames_elapsed))
                / (1.0 - math::pow(MOMENTUM_DISTANCE_SHARE_PER_FRAME, frames));
        }

        // https://drafts.csswg.org/cssom-view/#smooth-scroll
        // A smooth scroll follows a user-agent-defined timing function.
        cubic_bezier(
            curve_parameter_for_progress(progress),
            self.easing_control_point_y1,
            EASING_CONTROL_POINT_Y2,
        )
    }

    fn easing_slope_at(&self, progress: f64) -> f64 {
        if self.kind == ScrollAnimationKind::Momentum {
            let frames = self.duration.to_seconds_f64() / MOMENTUM_FRAME_DURATION_IN_SECONDS;
            let frames_elapsed = progress * frames;
            let decay = math::log(MOMENTUM_DISTANCE_SHARE_PER_FRAME);
            return -math::pow(MOMENTUM_DISTANCE_SHARE_PER_FRAME, frames_elapsed) * decay * frames
                / (1.0 - math::pow(MOMENTUM_DISTANCE_SHARE_PER_FRAME, frames));
        }
        let t = curve_parameter_for_progress(progress);
        let horizontal_slope = cubic_bezier_derivative(t, EASING_CONTROL_POINT_X1, EASING_CONTROL_POINT_X2);
        if horizontal_slope == 0.0 {
            return 0.0;
        }
        cubic_bezier_derivative(t, self.easing_control_point_y1, EASING_CONTROL_POINT_Y2) / horizontal_slope
    }

    fn velocity_at(&self, elapsed: Duration) -> FloatPoint {
        if self.duration.is_zero() {
            return FloatPoint::default();
        }
        let duration_in_seconds = self.duration.to_seconds_f64();
        let progress = ((elapsed - self.start_time).to_seconds_f64() / duration_in_seconds).clamp(0.0, 1.0);
        let speed_per_pixel = self.easing_slope_at(progress) / duration_in_seconds;
        let distance = self.destination_offset - self.start_offset;
        FloatPoint {
            x: (distance.x as f64 * speed_per_pixel) as f32,
            y: (distance.y as f64 * speed_per_pixel) as f32,
        }
    }

    pub fn sample(&self, elapsed: Duration) -> Sample {
        let elapsed_since_start = elapsed - self.start_time;
        if self.duration.is_zero() || elapsed_since_start >= self.duration {
            return Sample {
                offset: self.destination_offset,
                complete: true,
            };
        }
        let progress = (elapsed_since_start.to_seconds_f64() / self.duration.to_seconds_f64()).clamp(0.0, 1.0);
        let eased_progress = self.eased_progress_at(progress);
        Sample {
            offset: FloatPoint {
                x: (self.start_offset.x as f64
                    + (self.destination_offset.x - self.start_offset.x) as f64 * eased_progress)
                    as f32,
                y: (self.start_offset.y as f64
                    + (self.destination_offset.y - self.start_offset.y) as f64 * eased_progress)
                    as f32,
            },
            complete: false,
        }
    }

    pub fn retarget(&mut self, destination_offset: FloatPoint, elapsed: Duration) {
        if destination_offset == self.destination_offset {
            return;
        }
        let current_offset = self.sample(elapsed).offset;
        let remaining_distance = destination_offset - current_offset;
        let remaining_length = math::hypot(remaining_distance.x as f64, remaining_distance.y as f64);

        // Only the speed along the new path carries over.
        let mut velocity = 0.0;
        if remaining_length != 0.0 {
            let velocity_vector = self.velocity_at(elapsed);
            velocity = (velocity_vector.x as f64 * remaining_distance.x as f64
                + velocity_vector.y as f64 * remaining_distance.y as f64)
                / remaining_length;
        }
        let mut duration = self.duration_for_distance(remaining_distance);

        // An animation moving faster than the remaining distance needs would swing past the destination and back.
        if velocity > 0.0 {
            duration = min(
                duration,
                Duration::from_seconds_f64(remaining_length / velocity * RETARGET_EASE_OUT_ALLOWANCE),
            );
        }
        self.start_offset = current_offset;
        self.destination_offset = destination_offset;
        self.start_time = elapsed;
        self.duration = duration;
        self.easing_control_point_y1 = 0.0;
        if duration.is_zero() || remaining_length == 0.0 {
            return;
        }

        // The curve's starting slope is y1 / x1.
        let slope = velocity * duration.to_seconds_f64() / remaining_length;
        self.easing_control_point_y1 =
            EASING_CONTROL_POINT_X1 * slope.clamp(-MAXIMUM_EASING_SLOPE, MAXIMUM_EASING_SLOPE);
    }

    pub fn duration(&self) -> Duration {
        self.duration
    }
    pub fn destination_offset(&self) -> FloatPoint {
        self.destination_offset
    }
    pub fn kind(&self) -> ScrollAnimationKind {
        self.kind
    }

    /// Moves where the scroll comes from and goes to by the same distance, which keeps its pace.
    pub fn shift_by(&mut self, delta: FloatPoint) {
        self.start_offset.translate_by(delta);
        self.destination_offset.translate_by(delta);
    }
}
