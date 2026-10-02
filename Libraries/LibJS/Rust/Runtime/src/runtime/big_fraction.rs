/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Crypto::BigFraction, the arbitrary-precision fractions Temporal computes totals and fractional durations with,
//! over num_bigint, rounding and converting exactly as LibCrypto does.

use core::ops::{Add, Div, Mul, Neg, Sub};

use num_bigint::Sign;
use num_integer::Integer;
use num_traits::{One, Signed, Zero};

use ak::Utf16String;

use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::big_int_algorithms;
use crate::utf16::Utf16StringBuilder;

/// A fraction whose denominator is positive. Arithmetic reduces it, but the conversion from a double does not.
#[derive(Clone, Debug)]
pub struct BigFraction {
    numerator: SignedBigInteger,
    denominator: SignedBigInteger,
}

impl Default for BigFraction {
    fn default() -> Self {
        Self {
            numerator: SignedBigInteger::zero(),
            denominator: SignedBigInteger::one(),
        }
    }
}

/// AK::pow() of a double raised to an integral power, which multiplies the base by itself rather than calling the
/// C library, and inverts the product for negative powers.
fn ak_pow_integral(base: f64, power: i32) -> f64 {
    if power == 0 {
        return 1.0;
    }
    if base == 0.0 {
        return 0.0;
    }
    if power == 1 {
        return base;
    }
    let mut result = base;
    let mut i = 0;
    while f64::from(i) < f64::from(power).abs() - 1.0 {
        result *= base;
        i += 1;
    }
    if power < 0 {
        result = 1.0 / result;
    }
    result
}

impl BigFraction {
    /// BigFraction(SignedBigInteger numerator, UnsignedBigInteger denominator).
    pub fn new(numerator: SignedBigInteger, denominator: SignedBigInteger) -> Self {
        assert!(!denominator.is_zero() && denominator.sign() != Sign::Minus);
        let mut fraction = Self { numerator, denominator };
        fraction.reduce();
        fraction
    }

    /// BigFraction(SignedBigInteger value).
    pub fn from_integer(value: SignedBigInteger) -> Self {
        Self::new(value, SignedBigInteger::one())
    }

    /// BigFraction(double), which reads the decimal digits of the double one at a time with AK::pow(), and so is
    /// only exact for small values. Its exponent is an i8, which wraps for values of 10^127 and above, where the
    /// C++ never finishes reading digits; this replicates that.
    pub fn from_double(mut value: f64) -> Self {
        let mut numerator = SignedBigInteger::zero();
        let mut denominator = SignedBigInteger::one();

        let mut negative = false;
        if value < 0.0 {
            negative = true;
            value = -value;
        }
        let mut current_power: i8 = 0;
        while ak_pow_integral(10.0, i32::from(current_power)) <= value {
            current_power = current_power.wrapping_add(1);
        }
        current_power = current_power.wrapping_sub(1);
        let mut decimal_places: u32 = 0;
        while value >= f64::EPSILON || current_power >= 0 {
            numerator *= 10;
            let digit = ((value * ak_pow_integral(0.1, i32::from(current_power))) as u64 % 10) as i8;
            numerator += digit;
            value -= f64::from(digit) * ak_pow_integral(10.0, i32::from(current_power));
            if current_power < 0 {
                decimal_places += 1;
                denominator = SignedBigInteger::from(10).pow(decimal_places);
            }
            current_power = current_power.wrapping_sub(1);
        }
        if negative {
            numerator = -numerator;
        }
        Self { numerator, denominator }
    }

    pub fn numerator(&self) -> &SignedBigInteger {
        &self.numerator
    }

    pub fn denominator(&self) -> &SignedBigInteger {
        &self.denominator
    }

    pub fn is_zero(&self) -> bool {
        self.numerator.is_zero()
    }

    /// BigFraction::rounded(): the fraction over 10^rounding_threshold, rounding half away from zero for positive
    /// fractions and truncating negative ones, as LibCrypto's truncating division does.
    pub fn rounded(&self, rounding_threshold: u32) -> BigFraction {
        let get_last_digit = |integer: &SignedBigInteger| integer % 10;

        let quotient = &self.numerator / &self.denominator;
        let remainder = &self.numerator % &self.denominator;
        let mut result = BigFraction::from_integer(quotient);

        let needed_power = SignedBigInteger::from(10).pow(rounding_threshold);
        // We get one more digit to do proper rounding
        let fractional_value = (remainder * (&needed_power * 10)) / &self.denominator;

        result.numerator = &result.numerator * &needed_power + &fractional_value / 10;
        if get_last_digit(&fractional_value) > SignedBigInteger::from(4) {
            result.numerator += 1;
        }

        result.denominator = &result.denominator * &needed_power;

        result
    }

    /// BigFraction::to_utf16_string(): the decimal digits of the fraction rounded to `rounding_threshold` fractional
    /// digits, without trailing zeros.
    pub fn to_utf16_string(&self, rounding_threshold: u32) -> Utf16String {
        let mut builder = Utf16StringBuilder::new();
        if self.numerator.sign() == Sign::Minus {
            builder.append_ascii("-");
        }

        let number_of_digits = |integer: &SignedBigInteger| -> usize {
            let mut size = 1;
            let ten = SignedBigInteger::from(10);

            let (mut quotient, mut remainder) = integer.div_rem(&ten);

            while remainder.is_zero() && !quotient.is_zero() {
                (quotient, remainder) = quotient.div_rem(&ten);
                size += 1;
            }

            size
        };

        let rounded_fraction = self.rounded(rounding_threshold);

        // We take the unsigned value as we already manage the '-'
        let full_value = rounded_fraction.numerator.magnitude().to_string();
        let full_value_length = full_value.len();
        let split =
            (full_value_length as i64 - (number_of_digits(&rounded_fraction.denominator) as i64 - 1)).max(0) as usize;

        let remove_trailing_zeros = |value: &str| -> usize {
            assert!(!value.is_empty());
            value.trim_end_matches('0').len()
        };

        let raw_fractional_value = &full_value[split..];

        let integer_value = if split != 0 { &full_value[..split] } else { "0" };

        let fractional_value = if rounding_threshold != 0 {
            &raw_fractional_value[..remove_trailing_zeros(raw_fractional_value)]
        } else {
            "0"
        };

        builder.append_ascii(integer_value);

        let has_decimal_part = !fractional_value.is_empty() && fractional_value != "0";

        if has_decimal_part {
            builder.append_ascii(".");

            let mut number_pre_zeros = number_of_digits(&rounded_fraction.denominator)
                .wrapping_sub(full_value_length)
                .wrapping_sub(1);
            if number_pre_zeros > rounding_threshold as usize || fractional_value == "0" {
                number_pre_zeros = 0;
            }

            for _ in 0..number_pre_zeros {
                builder.append_ascii("0");
            }

            if fractional_value != "0" {
                builder.append_ascii(fractional_value);
            }
        }

        builder.to_utf16_string()
    }

    fn reduce(&mut self) {
        let gcd = self.numerator.abs().gcd(&self.denominator);

        if gcd.is_one() {
            return;
        }

        self.numerator /= &gcd;
        self.denominator /= &gcd;
    }

    /// BigFraction::to_double(): the quotient to 63 bits or more, with a sticky bit for any remainder, converted
    /// with the rounding of UnsignedBigInteger::to_double(), and then scaled back by adjusting the exponent field of
    /// the double directly.
    pub fn to_double(&self) -> f64 {
        let sign = self.numerator.sign() == Sign::Minus;
        if self.numerator.is_zero() {
            return if sign { -0.0 } else { 0.0 };
        }

        let mut numerator = self.numerator.magnitude().clone();
        let denominator = self.denominator.magnitude();

        let top_bit_numerator = numerator.bits();
        let top_bit_denominator = denominator.bits();
        let mut shift_left_numerator = 0;

        // 1. Shift numerator so that its most significant bit is exaclty 64 bits left tha than that of the denominator.
        // NOTE: the precision of the result will be 63 bits (more than 53 bits necessary for the mantissa of a double).
        if top_bit_numerator < top_bit_denominator + 64 {
            shift_left_numerator = top_bit_denominator + 64 - top_bit_numerator;
            numerator <<= shift_left_numerator;
        }
        // NOTE: Do nothing if numerator already has more than 64 bits more than denominator.

        // 2. Divide [potentially shifted] numerator by the denominator.
        let (mut quotient, remainder) = numerator.div_rem(denominator);
        if !remainder.is_zero() {
            // Extend the quotient with a "fake 1".
            quotient = (quotient << 1u32) + 1u32;
            // NOTE: Since the quotient has at least 63 bits, this will only affect the mantissa
            //       on rounding, and have the same effect on rounding as any fractional digits (from the remainder).
            shift_left_numerator += 1;
        }

        // 3. Convert the quotient to_double using UnsignedBigInteger::to_double.
        let bits = big_int_algorithms::unsigned_to_double(&quotient).to_bits();

        // 4. Shift the result back by the same number of bits as the numerator.
        const EXPONENT_MASK: u64 = 0x7ff;
        let exponent = (bits >> 52) & EXPONENT_MASK;
        let exponent = exponent.wrapping_sub(shift_left_numerator) & EXPONENT_MASK;
        let bits = (bits & !(EXPONENT_MASK << 52) & !(1 << 63)) | (exponent << 52) | (u64::from(sign) << 63);
        f64::from_bits(bits)
    }
}

impl Add for &BigFraction {
    type Output = BigFraction;

    fn add(self, rhs: &BigFraction) -> BigFraction {
        if rhs.numerator.is_zero() {
            return self.clone();
        }

        let mut result = BigFraction {
            numerator: &self.numerator * &rhs.denominator + &rhs.numerator * &self.denominator,
            denominator: &self.denominator * &rhs.denominator,
        };
        result.reduce();
        result
    }
}

impl Neg for &BigFraction {
    type Output = BigFraction;

    fn neg(self) -> BigFraction {
        BigFraction::new(-&self.numerator, self.denominator.clone())
    }
}

impl Sub for &BigFraction {
    type Output = BigFraction;

    fn sub(self, rhs: &BigFraction) -> BigFraction {
        self + &(-rhs)
    }
}

impl Mul for &BigFraction {
    type Output = BigFraction;

    fn mul(self, rhs: &BigFraction) -> BigFraction {
        let mut result = BigFraction {
            numerator: &self.numerator * &rhs.numerator,
            denominator: &self.denominator * &rhs.denominator,
        };
        result.reduce();
        result
    }
}

impl Div for &BigFraction {
    type Output = BigFraction;

    fn div(self, rhs: &BigFraction) -> BigFraction {
        assert!(!rhs.numerator.is_zero());

        let mut result = BigFraction {
            numerator: &self.numerator * &rhs.denominator,
            denominator: &self.denominator * rhs.numerator.abs(),
        };
        if rhs.numerator.sign() == Sign::Minus {
            result.numerator = -result.numerator;
        }
        result.reduce();
        result
    }
}
