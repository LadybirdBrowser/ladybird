/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Monotonic time as the compositor's scroll and input logic counts it: in nanoseconds, handed in by whoever calls.

use std::ops::{Add, Sub};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Duration(i64);

impl Duration {
    pub const fn zero() -> Self {
        Self(0)
    }

    pub const fn from_nanoseconds(nanoseconds: i64) -> Self {
        Self(nanoseconds)
    }

    pub const fn from_milliseconds(milliseconds: i64) -> Self {
        Self(milliseconds.saturating_mul(1_000_000))
    }

    pub fn from_seconds_f64(seconds: f64) -> Self {
        assert!(!seconds.is_nan());
        if seconds >= i64::MAX as f64 {
            return Self(i64::MAX);
        }
        if seconds <= i64::MIN as f64 {
            return Self(i64::MIN);
        }
        let mut integer_seconds = seconds as i64;
        if integer_seconds as f64 > seconds {
            integer_seconds = integer_seconds.saturating_sub(1);
        }
        let fractional_nanoseconds = (seconds - integer_seconds as f64) * 1_000_000_000.0;
        let mut integer_nanoseconds = fractional_nanoseconds as u32;
        if fractional_nanoseconds - integer_nanoseconds as f64 >= 0.5 {
            integer_nanoseconds += 1;
        }
        let nanoseconds = integer_seconds as i128 * 1_000_000_000 + integer_nanoseconds as i128;
        Self(nanoseconds.clamp(i64::MIN as i128, i64::MAX as i128) as i64)
    }

    pub const fn nanoseconds(self) -> i64 {
        self.0
    }

    pub fn to_seconds_f64(self) -> f64 {
        self.0.div_euclid(1_000_000_000) as f64 + self.0.rem_euclid(1_000_000_000) as f64 / 1_000_000_000.0
    }

    pub const fn is_zero(self) -> bool {
        self.0 == 0
    }
}

impl Add for Duration {
    type Output = Self;

    fn add(self, other: Self) -> Self {
        Self(self.0.saturating_add(other.0))
    }
}

impl Sub for Duration {
    type Output = Self;

    fn sub(self, other: Self) -> Self {
        Self(self.0.saturating_sub(other.0))
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MonotonicTime(i64);

impl MonotonicTime {
    pub const fn from_nanoseconds(nanoseconds: i64) -> Self {
        Self(nanoseconds)
    }

    pub const fn nanoseconds(self) -> i64 {
        self.0
    }
}

impl Add<Duration> for MonotonicTime {
    type Output = Self;

    fn add(self, duration: Duration) -> Self {
        Self(self.0.saturating_add(duration.0))
    }
}

impl Sub<Duration> for MonotonicTime {
    type Output = Self;

    fn sub(self, duration: Duration) -> Self {
        Self(self.0.saturating_sub(duration.0))
    }
}

impl Sub for MonotonicTime {
    type Output = Duration;

    fn sub(self, other: Self) -> Duration {
        Duration(self.0.saturating_sub(other.0))
    }
}
