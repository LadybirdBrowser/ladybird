/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Time values of the Web Animations timing model: a time in milliseconds, or a percentage of a
//! progress-based timeline.

use libipc_rust::wire::{Decode, Encode};
use std::cmp::Ordering;
use std::ops::{Add, Div, Mul, Neg, Sub};

/// The unit of a time value.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Encode, Decode)]
pub enum TimeValueType {
    #[default]
    Milliseconds,
    Percentage,
}

/// A time value. Arithmetic and ordering between values of different units are meaningless and
/// panic; equality between them is false.
///
/// cbindgen:field-names=[type, value]
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Encode, Decode)]
pub struct TimeValue {
    pub r#type: TimeValueType,
    pub value: f64,
}

impl TimeValue {
    #[inline]
    #[must_use]
    pub const fn new(r#type: TimeValueType, value: f64) -> Self {
        Self { r#type, value }
    }

    #[inline]
    #[must_use]
    pub const fn milliseconds(value: f64) -> Self {
        Self::new(TimeValueType::Milliseconds, value)
    }

    #[inline]
    #[must_use]
    pub const fn percentage(value: f64) -> Self {
        Self::new(TimeValueType::Percentage, value)
    }

    /// A zero time in `r#type`.
    #[inline]
    #[must_use]
    pub const fn zero(r#type: TimeValueType) -> Self {
        Self::new(r#type, 0.0)
    }

    #[inline]
    fn assert_same_type(self, other: Self) {
        assert!(
            self.r#type == other.r#type,
            "time values of different units: {self:?} and {other:?}"
        );
    }
}

impl Neg for TimeValue {
    type Output = Self;
    #[inline]
    fn neg(self) -> Self {
        Self::new(self.r#type, -self.value)
    }
}

impl Add for TimeValue {
    type Output = Self;
    #[inline]
    fn add(self, other: Self) -> Self {
        self.assert_same_type(other);
        Self::new(self.r#type, self.value + other.value)
    }
}

impl Sub for TimeValue {
    type Output = Self;
    #[inline]
    fn sub(self, other: Self) -> Self {
        self.assert_same_type(other);
        Self::new(self.r#type, self.value - other.value)
    }
}

impl Mul<f64> for TimeValue {
    type Output = Self;
    #[inline]
    fn mul(self, factor: f64) -> Self {
        Self::new(self.r#type, self.value * factor)
    }
}

impl Div<f64> for TimeValue {
    type Output = Self;
    #[inline]
    fn div(self, divisor: f64) -> Self {
        Self::new(self.r#type, self.value / divisor)
    }
}

/// The ratio of two time values.
impl Div for TimeValue {
    type Output = f64;
    #[inline]
    fn div(self, other: Self) -> f64 {
        self.assert_same_type(other);
        self.value / other.value
    }
}

impl PartialOrd for TimeValue {
    /// Values neither less nor greater than each other, which includes a NaN against anything,
    /// order equal.
    #[inline]
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        self.assert_same_type(*other);
        Some(if self.value < other.value {
            Ordering::Less
        } else if self.value > other.value {
            Ordering::Greater
        } else {
            Ordering::Equal
        })
    }
}

/// The larger of two values, keeping `a` where neither is less.
#[inline]
#[must_use]
pub fn max<T: PartialOrd>(a: T, b: T) -> T {
    if a < b { b } else { a }
}

/// The smaller of two values, keeping `a` where neither is less.
#[inline]
#[must_use]
pub fn min<T: PartialOrd>(a: T, b: T) -> T {
    if b < a { b } else { a }
}

/// A time value that may be unresolved.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OptionalTimeValue {
    pub value: TimeValue,
    pub has_value: bool,
}

impl OptionalTimeValue {
    pub const UNRESOLVED: Self = Self {
        value: TimeValue::milliseconds(0.0),
        has_value: false,
    };

    #[inline]
    #[must_use]
    pub fn get(self) -> Option<TimeValue> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<TimeValue>> for OptionalTimeValue {
    #[inline]
    fn from(value: Option<TimeValue>) -> Self {
        match value {
            Some(value) => Self { value, has_value: true },
            None => Self::UNRESOLVED,
        }
    }
}

impl From<OptionalTimeValue> for Option<TimeValue> {
    #[inline]
    fn from(value: OptionalTimeValue) -> Self {
        value.get()
    }
}

/// A number that may be unresolved.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OptionalNumber {
    pub value: f64,
    pub has_value: bool,
}

impl OptionalNumber {
    #[inline]
    #[must_use]
    pub fn get(self) -> Option<f64> {
        self.has_value.then_some(self.value)
    }
}

impl From<Option<f64>> for OptionalNumber {
    #[inline]
    fn from(value: Option<f64>) -> Self {
        Self {
            value: value.unwrap_or(0.0),
            has_value: value.is_some(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[should_panic(expected = "different units")]
    fn mixed_unit_arithmetic_panics() {
        let _ = TimeValue::percentage(30.0) + TimeValue::milliseconds(10.0);
    }

    #[test]
    #[should_panic(expected = "different units")]
    fn mixed_unit_ordering_panics() {
        let _ = TimeValue::percentage(30.0) < TimeValue::milliseconds(10.0);
    }

    #[test]
    fn nan_orders_equal_and_max_keeps_the_first() {
        let nan = TimeValue::milliseconds(f64::NAN);
        let zero = TimeValue::milliseconds(0.0);
        assert!(nan <= zero && nan >= zero);
        assert!(max(nan, zero).value.is_nan());
        assert_eq!(max(zero, nan), zero);
        assert!(max(f64::NAN, 0.0).is_nan());
        assert_eq!(min(1.0, 2.0), 1.0);
    }
}
