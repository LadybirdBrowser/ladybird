/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The momentum of a touchpad flick: its velocity as the fingers lift, and how it decays.

// Velocities are in wheel delta units per second. Displacements are in wheel delta units.

use super::smooth_scroll_animation::{
    MAXIMUM_MOMENTUM_DURATION_IN_SECONDS, MOMENTUM_DISTANCE_SHARE_PER_FRAME, MOMENTUM_FRAME_DURATION_IN_SECONDS,
};
use super::{max, min};
use crate::time::{Duration, MonotonicTime};
use ak::math;
use libgfx_rust::FloatPoint;
use std::collections::VecDeque;

// Use only the most recent steps to find the velocity at the lift.
const VELOCITY_SAMPLING_WINDOW: Duration = Duration::from_milliseconds(100);

// If the fingers stop for longer than this before they lift, no fling starts.
const MAXIMUM_PAUSE_BEFORE_LIFT: Duration = Duration::from_milliseconds(100);

const MINIMUM_FLING_START_SPEED: f64 = 50.0;
const MINIMUM_FLING_SPEED: f64 = 20.0;
const MAXIMUM_FLING_SPEED: f64 = 20000.0;

// A boost occurs only if the stopped fling was fast and a fast flick in the same direction starts soon after.
const MINIMUM_BOOSTED_FLING_SPEED: f64 = 350.0;
const MINIMUM_BOOSTING_FLICK_SPEED: f64 = 150.0;
const MAXIMUM_BOOST_DELAY: Duration = Duration::from_milliseconds(500);

#[derive(Default)]
pub struct ScrollVelocityTracker {
    steps: VecDeque<Step>,
}

struct Step {
    delta: FloatPoint,
    time: MonotonicTime,
}

/// Continues to scroll after a touchpad flick, for platforms that do not send momentum scroll events.
/// The speed decreases by the same fraction per frame as a momentum smooth scroll.
/// MomentumFlingEstimator uses this decrease to find where the scroll stops.
pub struct ScrollFling {
    initial_velocity: FloatPoint,
    initial_speed: f64,
    started_at: MonotonicTime,
    last_step_elapsed_seconds: f64,
    last_step_decay: f64,
    finished: bool,
}

/// If the user flicks again while a fling is fast, the new fling gets the velocity of the old fling added.
#[derive(Default)]
pub struct ScrollFlingBooster {
    interrupted_fling: Option<InterruptedFling>,
}

#[derive(Clone, Copy)]
struct InterruptedFling {
    velocity: FloatPoint,
    interrupted_at: MonotonicTime,
}

fn speed_of(velocity: FloatPoint) -> f64 {
    math::hypot(velocity.x as f64, velocity.y as f64)
}

fn with_speed_capped(velocity: FloatPoint) -> FloatPoint {
    let speed = speed_of(velocity);
    if speed <= MAXIMUM_FLING_SPEED {
        return velocity;
    }
    velocity.scaled((MAXIMUM_FLING_SPEED / speed) as f32)
}

impl ScrollVelocityTracker {
    pub fn add_step(&mut self, delta: FloatPoint, time: MonotonicTime) {
        if self.steps.len() == 20 {
            self.steps.pop_front();
        }
        self.steps.push_back(Step { delta, time });
    }

    /// Returns no value if the fingers stopped for too long before the lift.
    pub fn velocity_at_lift(&self, lift_time: MonotonicTime) -> Option<FloatPoint> {
        let last_step_time = self.steps.back()?.time;
        if lift_time - last_step_time > MAXIMUM_PAUSE_BEFORE_LIFT {
            return None;
        }

        // Steps from before the window are not used. After a pause, they would make the velocity much too low.
        let mut first_index = self.steps.len() - 1;
        while first_index > 0 && last_step_time - self.steps[first_index - 1].time <= VELOCITY_SAMPLING_WINDOW {
            first_index -= 1;
        }
        let point_count = self.steps.len() - first_index;
        if point_count < 2 {
            return None;
        }

        // Fit a least-squares line to the total displacement over time. Thus, one late or early step has less effect on
        // the velocity.
        // Times are relative to the last step to keep the sums small.
        let (mut sum_t, mut sum_tt, mut sum_x, mut sum_y, mut sum_tx, mut sum_ty, mut x, mut y) =
            (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        for step in self.steps.iter().skip(first_index) {
            x += step.delta.x as f64;
            y += step.delta.y as f64;
            let t = (step.time - last_step_time).to_seconds_f64();
            sum_t += t;
            sum_tt += t * t;
            sum_x += x;
            sum_y += y;
            sum_tx += t * x;
            sum_ty += t * y;
        }
        let n = point_count as f64;
        let determinant = n * sum_tt - sum_t * sum_t;
        // If all steps have the same time, there is no velocity.
        if determinant <= 0.0 {
            return None;
        }
        Some(FloatPoint {
            x: ((n * sum_tx - sum_t * sum_x) / determinant) as f32,
            y: ((n * sum_ty - sum_t * sum_y) / determinant) as f32,
        })
    }

    pub fn reset(&mut self) {
        self.steps.clear();
    }
}

impl ScrollFling {
    pub fn start(velocity: FloatPoint, now: MonotonicTime) -> Option<Self> {
        if speed_of(velocity) < MINIMUM_FLING_START_SPEED {
            return None;
        }
        Some(Self::new(with_speed_capped(velocity), now))
    }

    fn new(velocity: FloatPoint, now: MonotonicTime) -> Self {
        Self {
            initial_velocity: velocity,
            initial_speed: speed_of(velocity),
            started_at: now,
            last_step_elapsed_seconds: 0.0,
            last_step_decay: 1.0,
            finished: false,
        }
    }

    /// The fraction of the initial speed that remains after the given time.
    fn decay_at(&self, elapsed_seconds: f64) -> f64 {
        math::pow(
            MOMENTUM_DISTANCE_SHARE_PER_FRAME,
            elapsed_seconds / MOMENTUM_FRAME_DURATION_IN_SECONDS,
        )
    }

    pub fn velocity_at(&self, time: MonotonicTime) -> FloatPoint {
        if self.finished {
            return FloatPoint::default();
        }
        let elapsed_seconds = max(0.0, (time - self.started_at).to_seconds_f64());
        self.initial_velocity.scaled(self.decay_at(elapsed_seconds) as f32)
    }

    /// Returns the displacement since the previous step, or no value after the fling stops.
    pub fn step(&mut self, now: MonotonicTime) -> Option<FloatPoint> {
        if self.finished {
            return None;
        }
        let elapsed_seconds = min(
            (now - self.started_at).to_seconds_f64(),
            MAXIMUM_MOMENTUM_DURATION_IN_SECONDS,
        );
        if elapsed_seconds <= self.last_step_elapsed_seconds {
            return Some(FloatPoint::default());
        }

        // If v(t) = v0 * decay(t), the distance at time t is v0 * time_constant * (1 - decay(t)).
        let time_constant = MOMENTUM_FRAME_DURATION_IN_SECONDS / -math::log(MOMENTUM_DISTANCE_SHARE_PER_FRAME);
        let decay = self.decay_at(elapsed_seconds);
        let displacement = self
            .initial_velocity
            .scaled((time_constant * (self.last_step_decay - decay)) as f32);
        self.last_step_elapsed_seconds = elapsed_seconds;
        self.last_step_decay = decay;
        if self.initial_speed * decay < MINIMUM_FLING_SPEED || elapsed_seconds >= MAXIMUM_MOMENTUM_DURATION_IN_SECONDS {
            self.finished = true;
        }
        Some(displacement)
    }
}

impl ScrollFlingBooster {
    pub fn fling_was_interrupted(&mut self, velocity: FloatPoint, now: MonotonicTime) {
        if speed_of(velocity) < MINIMUM_BOOSTED_FLING_SPEED {
            self.interrupted_fling = None;
            return;
        }
        self.interrupted_fling = Some(InterruptedFling {
            velocity,
            interrupted_at: now,
        });
    }

    pub fn boosted_velocity(&mut self, velocity: FloatPoint, now: MonotonicTime) -> FloatPoint {
        let Some(interrupted_fling) = self.interrupted_fling.take() else {
            return velocity;
        };
        if now - interrupted_fling.interrupted_at > MAXIMUM_BOOST_DELAY {
            return velocity;
        }
        if speed_of(velocity) < MINIMUM_BOOSTING_FLICK_SPEED {
            return velocity;
        }
        let direction_agreement = velocity.x * interrupted_fling.velocity.x + velocity.y * interrupted_fling.velocity.y;
        if direction_agreement <= 0.0 {
            return velocity;
        }
        with_speed_capped(velocity + interrupted_fling.velocity)
    }

    pub fn reset(&mut self) {
        self.interrupted_fling = None;
    }
}
