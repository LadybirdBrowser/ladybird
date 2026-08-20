/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// CSSPixels fixed-point arithmetic, bit-exact with the C++ implementation in
// PixelUnits.h: an i32 with 6 fractional bits. The parity test on the C++
// side compares every operation against the original.
//
// Rounding notes, matching C++ exactly:
// - Conversion from floating point rounds half to even (the C++ path goes
//   through the x87 fistp instruction in the default rounding mode) and
//   saturates at the i32 bounds.
// - Multiplication rounds the cut-off fraction half away from zero when more
//   fraction bits follow, and half to even otherwise.
// - Fraction-to-value conversion truncates (plain integer division).

pub const FRACTIONAL_BITS: u32 = 6;
pub const FIXED_POINT_DENOMINATOR: i32 = 1 << FRACTIONAL_BITS;
pub const RADIX_MASK: i32 = FIXED_POINT_DENOMINATOR - 1;
pub const MAX_INTEGER_VALUE: i32 = i32::MAX >> FRACTIONAL_BITS;
pub const MIN_INTEGER_VALUE: i32 = i32::MIN >> FRACTIONAL_BITS;

#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Debug)]
#[repr(transparent)]
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

fn clamp_f32_to_i32(value: f32) -> i32 {
    if value >= i32::MAX as f32 {
        return i32::MAX;
    }
    if value <= i32::MIN as f32 {
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

    /// Matches `CSSPixels::nearest_value_for(float)`: the scaling and
    /// ties-to-even conversion both stay on the f32 path.
    pub fn nearest_value_for_f32(value: f32) -> Self {
        if value.is_nan() {
            return Self(0);
        }
        Self(clamp_f32_to_i32(value * FIXED_POINT_DENOMINATOR as f32))
    }

    pub fn round(self) -> Self {
        let half = Self(FIXED_POINT_DENOMINATOR >> 1);
        if self.0 > 0 {
            (self + half).floor()
        } else {
            (self - half).ceil()
        }
    }

    pub fn floor(self) -> Self {
        Self(self.0 & !RADIX_MASK)
    }

    pub fn ceil(self) -> Self {
        let floor = self.0 & !RADIX_MASK;
        let increment = if self.0 & RADIX_MASK != 0 {
            FIXED_POINT_DENOMINATOR
        } else {
            0
        };
        Self(floor.wrapping_add(increment))
    }

    pub fn to_int(self) -> i32 {
        self.0 / FIXED_POINT_DENOMINATOR
    }

    pub fn to_double(self) -> f64 {
        self.0 as f64 / FIXED_POINT_DENOMINATOR as f64
    }

    pub fn to_float(self) -> f32 {
        self.0 as f32 / FIXED_POINT_DENOMINATOR as f32
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

    pub fn mul_by_fraction(self, fraction: CssPixelFraction) -> Self {
        let mut wide_value = self.0 as i64;
        wide_value *= fraction.numerator.0 as i64;
        wide_value /= fraction.denominator.0 as i64;
        Self(clamp_i64_to_i32(wide_value))
    }

    pub fn div_by_fraction(self, fraction: CssPixelFraction) -> Self {
        let mut wide_value = self.0 as i64;
        wide_value *= fraction.denominator.0 as i64;
        wide_value /= fraction.numerator.0 as i64;
        Self(clamp_i64_to_i32(wide_value))
    }

    pub fn scaled(self, factor: f64) -> Self {
        Self::nearest_value_for(self.to_double() * factor)
    }

    /// Matches the floating-point `CSSPixelFraction(double, double)`
    /// constructor: a denominator that rounds to zero CSSPixels is rescued by
    /// folding it into the numerator before the fixed-point conversion.
    pub(crate) fn fraction_nearest_values_for(mut numerator: f64, mut denominator: f64) -> (CssPixels, CssPixels) {
        if Self::nearest_value_for(denominator).raw_value() == 0 {
            numerator /= denominator;
            denominator = 1.0;
        }
        (Self::nearest_value_for(numerator), Self::nearest_value_for(denominator))
    }
}

#[derive(Clone, Copy, Debug)]
pub struct CssPixelFraction {
    numerator: CssPixels,
    denominator: CssPixels,
}

impl CssPixelFraction {
    pub fn one() -> Self {
        Self {
            numerator: CssPixels::from_integer(1),
            denominator: CssPixels::from_integer(1),
        }
    }

    pub fn ratio_of(numerator: CssPixels, denominator: CssPixels) -> Self {
        assert!(denominator.raw_value() != 0);
        Self { numerator, denominator }
    }

    fn cross_products(self, other: Self) -> (i64, i64) {
        let left = self.numerator.raw_value() as i64 * other.denominator.raw_value() as i64;
        let right = other.numerator.raw_value() as i64 * self.denominator.raw_value() as i64;
        (left, right)
    }

    pub fn min(self, other: Self) -> Self {
        let (left, right) = self.cross_products(other);
        if left <= right { self } else { other }
    }

    pub fn is_at_least_one(self) -> bool {
        let (left, right) = self.cross_products(Self::one());
        left >= right
    }
}

const MAX_DIMENSION_RAW: i32 = 17_895_700 * 64;

pub(crate) fn max_dimension_value() -> CssPixels {
    CssPixels::from_raw(MAX_DIMENSION_RAW)
}

pub(crate) fn clamp_to_max_dimension_value(value: CssPixels) -> CssPixels {
    if matches!(value.raw_value(), i32::MIN | i32::MAX) {
        max_dimension_value()
    } else {
        value
    }
}

pub(crate) fn css_clamp(value: CssPixels, min: CssPixels, max: CssPixels) -> CssPixels {
    min.max(value.min(max))
}

impl std::ops::Add for CssPixels {
    type Output = Self;

    fn add(self, other: Self) -> Self {
        Self::from_raw(self.raw_value().saturating_add(other.raw_value()))
    }
}

impl std::ops::AddAssign for CssPixels {
    fn add_assign(&mut self, other: Self) {
        *self = *self + other;
    }
}

impl std::ops::Sub for CssPixels {
    type Output = Self;

    fn sub(self, other: Self) -> Self {
        Self::from_raw(self.raw_value().saturating_sub(other.raw_value()))
    }
}

impl std::ops::SubAssign for CssPixels {
    fn sub_assign(&mut self, other: Self) {
        *self = *self - other;
    }
}

impl std::ops::Neg for CssPixels {
    type Output = Self;

    fn neg(self) -> Self {
        Self::from_raw(0i32.saturating_sub(self.raw_value()))
    }
}

impl std::ops::Mul<usize> for CssPixels {
    type Output = Self;

    fn mul(self, other: usize) -> Self {
        let raw = (self.raw_value() as i64).saturating_mul(other as i64);
        Self::from_raw(raw.clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }
}

impl std::ops::Mul<CssPixels> for usize {
    type Output = CssPixels;

    fn mul(self, other: CssPixels) -> CssPixels {
        other * self
    }
}

impl std::ops::Div<usize> for CssPixels {
    type Output = Self;

    fn div(self, other: usize) -> Self {
        assert_ne!(other, 0);
        Self::from_raw((self.raw_value() as i64 / other as i64) as i32)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CssPixelPoint {
    pub x: CssPixels,
    pub y: CssPixels,
}

impl CssPixelPoint {
    pub const fn new(x: CssPixels, y: CssPixels) -> Self {
        Self { x, y }
    }
    pub fn translated(self, dx: CssPixels, dy: CssPixels) -> Self {
        Self {
            x: self.x + dx,
            y: self.y + dy,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CssPixelSize {
    pub width: CssPixels,
    pub height: CssPixels,
}

impl CssPixelSize {
    pub const fn new(width: CssPixels, height: CssPixels) -> Self {
        Self { width, height }
    }
    pub fn is_empty(self) -> bool {
        self.width <= CssPixels::from_raw(0) || self.height <= CssPixels::from_raw(0)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CssPixelRect {
    pub x: CssPixels,
    pub y: CssPixels,
    pub width: CssPixels,
    pub height: CssPixels,
}

impl CssPixelRect {
    pub const fn new(x: CssPixels, y: CssPixels, width: CssPixels, height: CssPixels) -> Self {
        Self { x, y, width, height }
    }
    pub const fn from_location_and_size(location: CssPixelPoint, size: CssPixelSize) -> Self {
        Self {
            x: location.x,
            y: location.y,
            width: size.width,
            height: size.height,
        }
    }
    pub const fn location(self) -> CssPixelPoint {
        CssPixelPoint { x: self.x, y: self.y }
    }
    pub const fn size(self) -> CssPixelSize {
        CssPixelSize {
            width: self.width,
            height: self.height,
        }
    }
    pub fn left(self) -> CssPixels {
        self.x
    }
    pub fn top(self) -> CssPixels {
        self.y
    }
    pub fn right(self) -> CssPixels {
        self.x + self.width
    }
    pub fn bottom(self) -> CssPixels {
        self.y + self.height
    }
    pub fn is_empty(self) -> bool {
        self.width <= CssPixels::from_raw(0) || self.height <= CssPixels::from_raw(0)
    }
    pub fn translated(self, dx: CssPixels, dy: CssPixels) -> Self {
        Self {
            x: self.x + dx,
            y: self.y + dy,
            ..self
        }
    }
    pub fn translated_by(self, offset: CssPixelPoint) -> Self {
        self.translated(offset.x, offset.y)
    }
    pub(crate) fn set_left(&mut self, left: CssPixels) {
        self.width = self.right() - left;
        self.x = left;
    }
    pub(crate) fn set_top(&mut self, top: CssPixels) {
        self.height = self.bottom() - top;
        self.y = top;
    }
    pub(crate) fn set_right(&mut self, right: CssPixels) {
        self.width = right - self.x;
    }
    pub(crate) fn set_bottom(&mut self, bottom: CssPixels) {
        self.height = bottom - self.y;
    }
    pub fn unite_horizontally(&mut self, other: Self) {
        let new_left = self.left().min(other.left());
        let new_right = self.right().max(other.right());
        self.set_left(new_left);
        self.set_right(new_right);
    }
    pub fn unite_vertically(&mut self, other: Self) {
        let new_top = self.top().min(other.top());
        let new_bottom = self.bottom().max(other.bottom());
        self.set_top(new_top);
        self.set_bottom(new_bottom);
    }
    pub fn intersected(self, other: Self) -> Self {
        let left = self.left().max(other.left());
        let right = self.right().min(other.right());
        let top = self.top().max(other.top());
        let bottom = self.bottom().min(other.bottom());
        if left > right || top > bottom {
            return Self::default();
        }
        Self {
            x: left,
            y: top,
            width: right - left,
            height: bottom - top,
        }
    }
    pub fn center(self) -> CssPixelPoint {
        let two = CssPixels::from_integer(2);
        CssPixelPoint {
            x: self.x + self.width.div_as_fraction(two),
            y: self.y + self.height.div_as_fraction(two),
        }
    }
    pub fn unite(&mut self, other: Self) {
        if self.is_empty() {
            *self = other;
            return;
        }
        if other.is_empty() {
            return;
        }
        self.unite_horizontally(other);
        self.unite_vertically(other);
    }
    pub fn shrink(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        self.x += left;
        self.width = self.width - left - right;
        self.y += top;
        self.height = self.height - top - bottom;
    }
    pub fn shrunken(mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) -> Self {
        self.shrink(top, right, bottom, left);
        self
    }
    pub fn inflate(&mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) {
        self.x -= left;
        self.width = self.width + left + right;
        self.y -= top;
        self.height = self.height + top + bottom;
    }
    pub fn inflated(mut self, top: CssPixels, right: CssPixels, bottom: CssPixels, left: CssPixels) -> Self {
        self.inflate(top, right, bottom, left);
        self
    }
    pub fn contains_rect(self, other: Self) -> bool {
        self.left() <= other.left()
            && self.right() >= other.right()
            && self.top() <= other.top()
            && self.bottom() >= other.bottom()
    }
    pub fn contains_point(self, point: CssPixelPoint) -> bool {
        point.x >= self.x && point.x < self.right() && point.y >= self.y && point.y < self.bottom()
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
    fn f32_nearest_value_keeps_the_float_instantiation() {
        assert_eq!(CssPixels::nearest_value_for_f32(0.0078125).raw_value(), 0);
        assert_eq!(CssPixels::nearest_value_for_f32(0.0234375).raw_value(), 2);
        assert_eq!(CssPixels::nearest_value_for_f32(f32::NAN).raw_value(), 0);
        assert_eq!(CssPixels::nearest_value_for_f32(f32::INFINITY).raw_value(), i32::MAX);
        assert_eq!(
            CssPixels::nearest_value_for_f32(f32::NEG_INFINITY).raw_value(),
            i32::MIN
        );
    }

    #[test]
    fn fixed_point_floor_and_ceil_match_cpp_bit_operations() {
        for (raw, floor, ceil) in [
            (0, 0, 0),
            (1, 0, 64),
            (63, 0, 64),
            (64, 64, 64),
            (65, 64, 128),
            (-1, -64, 0),
            (-63, -64, 0),
            (-64, -64, -64),
            (-65, -128, -64),
        ] {
            assert_eq!(CssPixels::from_raw(raw).floor().raw_value(), floor);
            assert_eq!(CssPixels::from_raw(raw).ceil().raw_value(), ceil);
        }
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
    crate::abort_on_panic(|| (CssPixels::from_raw(left_raw) * CssPixels::from_raw(right_raw)).raw_value())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_divide_as_fraction(numerator_raw: i32, denominator_raw: i32) -> i32 {
    crate::abort_on_panic(|| {
        CssPixels::from_raw(numerator_raw)
            .div_as_fraction(CssPixels::from_raw(denominator_raw))
            .raw_value()
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_nearest_value_for(value: f64) -> i32 {
    crate::abort_on_panic(|| CssPixels::nearest_value_for(value).raw_value())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_pixels_scaled(raw: i32, factor: f64) -> i32 {
    crate::abort_on_panic(|| CssPixels::from_raw(raw).scaled(factor).raw_value())
}
