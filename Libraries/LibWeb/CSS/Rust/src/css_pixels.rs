/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSPixels fixed-point arithmetic, bit-exact with the C++ implementation in
//! PixelUnits.h: an i32 with 6 fractional bits. The parity test on the C++
//! side compares every operation against the original.
//!
//! Rounding notes, matching C++ exactly:
//! - Conversion from floating point rounds half to even (the C++ path goes
//!   through the x87 fistp instruction in the default rounding mode) and
//!   saturates at the i32 bounds.
//! - Multiplication rounds the cut-off fraction half away from zero when more
//!   fraction bits follow, and half to even otherwise.
//! - Fraction-to-value conversion truncates (plain integer division).

pub const FRACTIONAL_BITS: u32 = 6;
pub const FIXED_POINT_DENOMINATOR: i32 = 1 << FRACTIONAL_BITS;
pub const RADIX_MASK: i32 = FIXED_POINT_DENOMINATOR - 1;
pub const MAX_INTEGER_VALUE: i32 = i32::MAX >> FRACTIONAL_BITS;
pub const MIN_INTEGER_VALUE: i32 = i32::MIN >> FRACTIONAL_BITS;

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct CssPixels(i32);

impl std::ops::Mul for CssPixels {
    type Output = CssPixels;
    fn mul(self, other: CssPixels) -> CssPixels {
        self.fixed_point_multiply(other)
    }
}

fn clamp_i64_to_i32(value: i64) -> i32 {
    value.clamp(i32::MIN as i64, i32::MAX as i64) as i32
}

fn clamp_f64_to_i32(value: f64) -> i32 {
    if value >= i32::MAX as f64 {
        return i32::MAX;
    }
    if value <= i32::MIN as f64 {
        return i32::MIN;
    }
    value.round_ties_even() as i32
}

impl CssPixels {
    pub fn from_raw(raw: i32) -> Self {
        Self(raw)
    }

    pub fn raw_value(self) -> i32 {
        self.0
    }

    pub fn from_integer(value: i64) -> Self {
        if value > MAX_INTEGER_VALUE as i64 {
            Self(i32::MAX)
        } else if value < MIN_INTEGER_VALUE as i64 {
            Self(i32::MIN)
        } else {
            Self((value as i32) << FRACTIONAL_BITS)
        }
    }

    pub fn nearest_value_for(value: f64) -> Self {
        if value.is_nan() {
            return Self(0);
        }
        Self(clamp_f64_to_i32(value * FIXED_POINT_DENOMINATOR as f64))
    }

    pub fn to_double(self) -> f64 {
        self.0 as f64 / FIXED_POINT_DENOMINATOR as f64
    }

    fn fixed_point_multiply(self, other: CssPixels) -> Self {
        let value = self.0 as i64 * other.0 as i64;
        let mut int_value = clamp_i64_to_i32(value >> FRACTIONAL_BITS);

        // Rounding: if the last bit cut off was 1:
        if value & (1i64 << (FRACTIONAL_BITS - 1)) != 0 {
            // If any bit after was 1 as well, round away from zero.
            if value & (RADIX_MASK as i64 >> 1) != 0 {
                int_value = int_value.saturating_add(1);
            } else {
                // Otherwise round to the next even value, adding the least
                // significant bit of the raw integer value.
                int_value = int_value.saturating_add(int_value & 1);
            }
        }
        Self(int_value)
    }

    /// Division through CSSPixelFraction: the conversion back to a value is a
    /// truncating integer division of the widened numerator.
    pub fn div_as_fraction(self, denominator: CssPixels) -> Self {
        let wide_value = (self.0 as i64) << FRACTIONAL_BITS;
        Self(clamp_i64_to_i32(wide_value / denominator.0 as i64))
    }

    pub fn scaled(self, factor: f64) -> Self {
        Self::nearest_value_for(self.to_double() * factor)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn integer_construction_shifts_and_saturates() {
        assert_eq!(CssPixels::from_integer(3).raw_value(), 3 << 6);
        assert_eq!(CssPixels::from_integer(1 << 40).raw_value(), i32::MAX);
        assert_eq!(CssPixels::from_integer(-(1 << 40)).raw_value(), i32::MIN);
    }

    #[test]
    fn nearest_value_rounds_half_to_even() {
        // 0.0078125 * 64 = 0.5: ties to even -> 0.
        assert_eq!(CssPixels::nearest_value_for(0.0078125).raw_value(), 0);
        // 0.0234375 * 64 = 1.5: ties to even -> 2.
        assert_eq!(CssPixels::nearest_value_for(0.0234375).raw_value(), 2);
        assert_eq!(CssPixels::nearest_value_for(f64::NAN).raw_value(), 0);
    }

    #[test]
    fn multiplication_rounding_modes() {
        // 0.5 * 0.5 = 0.25: exact, no rounding.
        let half = CssPixels::from_raw(32);
        assert_eq!((half * half).raw_value(), 16);
        // Half-bit set with trailing bits rounds away from zero;
        // half-bit alone rounds to even.
        let x = CssPixels::from_raw(33); // 33 * 32 = 1056; 1056 >> 6 = 16 r 32 (half, no trailing) -> even
        assert_eq!((x * half).raw_value(), 16);
        let y = CssPixels::from_raw(35); // 35 * 33 = 1155 = 18 * 64 + 3; below half -> truncate
        assert_eq!((y * CssPixels::from_raw(33)).raw_value(), 18);
    }

    #[test]
    fn fraction_division_truncates() {
        // (10 << 6) << 6 / (3 << 6) = 640 / 3 * ... : 10/3 in fixed point = 213.33 -> 213.
        let ten = CssPixels::from_integer(10);
        let three = CssPixels::from_integer(3);
        assert_eq!(ten.div_as_fraction(three).raw_value(), 213);
        let minus_ten = CssPixels::from_integer(-10);
        assert_eq!(minus_ten.div_as_fraction(three).raw_value(), -213);
    }
}

/// FFI hooks for the C++ parity test.
#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_multiply(left_raw: i32, right_raw: i32) -> i32 {
    (CssPixels::from_raw(left_raw) * CssPixels::from_raw(right_raw)).raw_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_divide_as_fraction(numerator_raw: i32, denominator_raw: i32) -> i32 {
    CssPixels::from_raw(numerator_raw)
        .div_as_fraction(CssPixels::from_raw(denominator_raw))
        .raw_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_nearest_value_for(value: f64) -> i32 {
    CssPixels::nearest_value_for(value).raw_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_scaled(raw: i32, factor: f64) -> i32 {
    CssPixels::from_raw(raw).scaled(factor).raw_value()
}
