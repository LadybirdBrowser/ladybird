/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The BigInt arithmetic that needs no cells: the BigInt operations of the spec, the conversions between BigInts,
//! Numbers and strings, and the Crypto::SignedBigInteger operations they rely on, over num_bigint::BigInt.

use core::cmp::Ordering;

use num_bigint::{BigInt, BigUint, Sign};
use num_traits::{One, ToPrimitive, Zero};

use crate::runtime::string_conversions::{BinaryDecomposition, decompose_double};
use crate::runtime::value_conversions::NumericOperationError;

/// Crypto::UnsignedBigInteger::CompareResult.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompareResult {
    DoubleEqualsBigInt,
    DoubleLessThanBigInt,
    DoubleGreaterThanBigInt,
}

/// The number of bits in a libtommath digit on 64-bit targets. Crypto's mod_power_of_two() returns positive numbers
/// unchanged when they occupy no more digits than the modulus has bits.
const TOMMATH_DIGIT_BIT: u64 = 60;

const DOUBLE_MANTISSA_BITS: u32 = 52;
const DOUBLE_EXPONENT_BIAS: u64 = 1023;

/// Crypto's pow() passes its u32 exponent to mp_expt_n(), which takes an int. Exponents of 2^31 and above become
/// negative there, and mp_expt_n() then returns 1 without multiplying, so this returns the exponent actually applied.
fn exponent_applied_by_crypto_pow(exponent: u32) -> u32 {
    if i32::try_from(exponent).is_ok() { exponent } else { 0 }
}

fn power_of_two_double(exponent: u64) -> f64 {
    debug_assert!(exponent < 1024);
    f64::from_bits((DOUBLE_EXPONENT_BIAS + exponent) << DOUBLE_MANTISSA_BITS)
}

/// floor(abs(value)) for a finite double, exactly.
fn floor_of_magnitude(value: f64) -> BigUint {
    let BinaryDecomposition { significand, exponent } = decompose_double(value);
    if exponent >= 0 {
        BigUint::from(significand) << exponent.unsigned_abs()
    } else {
        BigUint::from(significand) >> exponent.unsigned_abs()
    }
}

/// Crypto::UnsignedBigInteger::to_double(RoundingMode::ECMAScriptNumberValueFor): the Number value for the integer,
/// rounding to nearest with ties to an even significand.
pub fn unsigned_to_double(value: &BigUint) -> f64 {
    // Check if we need to truncate
    let bitlen = value.bits();
    if bitlen <= 53 {
        return value.to_u64().expect("53 bits fit in a u64") as f64;
    }

    // Get top 53 bits (truncated)
    let shift = bitlen - 53;
    let mut shifted = (value >> shift).to_u64().expect("53 bits fit in a u64");

    // Compare the remainder with 2^(shift - 1).
    let remainder_has_half_bit = value.bit(shift - 1);
    let remainder_exceeds_half = value.trailing_zeros().is_some_and(|zeros| zeros < shift - 1);
    if remainder_has_half_bit && (remainder_exceeds_half || shifted % 2 == 1) {
        // Round up, or round an exact half to even.
        shifted += 1;
    }

    // Convert to double. The significand is at most 2^53, so scaling it is exact until it overflows to infinity.
    if shift >= 1024 {
        return f64::INFINITY;
    }
    shifted as f64 * power_of_two_double(shift)
}

/// Crypto::SignedBigInteger::to_double(RoundingMode::ECMAScriptNumberValueFor).
pub fn to_double(value: &BigInt) -> f64 {
    let sign = if value.sign() == Sign::Minus { -1.0 } else { 1.0 };
    unsigned_to_double(value.magnitude()) * sign
}

/// Crypto::UnsignedBigInteger::compare_to_double: an exact comparison of the integer with a non-NaN double.
pub fn unsigned_compare_to_double(value: &BigUint, double: f64) -> CompareResult {
    assert!(!double.is_nan());

    if double.is_infinite() {
        return if double > 0.0 {
            CompareResult::DoubleGreaterThanBigInt
        } else {
            CompareResult::DoubleLessThanBigInt
        };
    }

    if double < 0.0 {
        return CompareResult::DoubleLessThanBigInt;
    }

    // Value is zero.
    if double == 0.0 {
        // Either we are also zero or value is certainly less than us.
        return if value.is_zero() {
            CompareResult::DoubleEqualsBigInt
        } else {
            CompareResult::DoubleLessThanBigInt
        };
    }

    // If value is not zero but we are, value must be greater.
    if value.is_zero() {
        return CompareResult::DoubleGreaterThanBigInt;
    }

    let floor = floor_of_magnitude(double);
    let double_is_integral = double.trunc() == double;
    match value.cmp(&floor) {
        Ordering::Less => CompareResult::DoubleGreaterThanBigInt,
        Ordering::Equal if double_is_integral => CompareResult::DoubleEqualsBigInt,
        Ordering::Equal => CompareResult::DoubleGreaterThanBigInt,
        Ordering::Greater => CompareResult::DoubleLessThanBigInt,
    }
}

/// Crypto::SignedBigInteger::compare_to_double.
pub fn compare_to_double(value: &BigInt, double: f64) -> CompareResult {
    let bigint_is_negative = value.sign() == Sign::Minus;

    let value_is_negative = double < 0.0;

    if value_is_negative != bigint_is_negative {
        return if bigint_is_negative {
            CompareResult::DoubleGreaterThanBigInt
        } else {
            CompareResult::DoubleLessThanBigInt
        };
    }

    // Now both bigint and value have the same sign, so let's compare our magnitudes.
    let magnitudes_compare_result = unsigned_compare_to_double(value.magnitude(), double.abs());

    // If our magnitudes are equal, then we're equal.
    if magnitudes_compare_result == CompareResult::DoubleEqualsBigInt {
        return CompareResult::DoubleEqualsBigInt;
    }

    // If we're negative, revert the comparison result, otherwise return the same result.
    if value_is_negative {
        if magnitudes_compare_result == CompareResult::DoubleLessThanBigInt {
            CompareResult::DoubleGreaterThanBigInt
        } else {
            CompareResult::DoubleLessThanBigInt
        }
    } else {
        magnitudes_compare_result
    }
}

/// 21.2.1.1.1 NumberToBigInt ( number ), https://tc39.es/ecma262/#sec-numbertobigint
pub fn number_to_bigint(number: f64) -> Result<BigInt, NumericOperationError> {
    // 1. If IsIntegralNumber(number) is false, throw a RangeError exception.
    if !number.is_finite() || number.trunc() != number {
        return Err(NumericOperationError::BigIntFromNonIntegral);
    }

    // 2. Return the BigInt value that represents ℝ(number).
    let sign = if number < 0.0 { Sign::Minus } else { Sign::Plus };
    Ok(BigInt::from_biguint(sign, floor_of_magnitude(number)))
}

/// Crypto::SignedBigInteger::to_u64, which is libtommath's mp_get_u64: the low 64 bits of the magnitude, negated
/// modulo 2^64 for negative values, which is ℝ(value) modulo 2^64.
pub fn to_u64(value: &BigInt) -> u64 {
    let low_bits = value.magnitude().iter_u64_digits().next().unwrap_or(0);
    if value.sign() == Sign::Minus {
        low_bits.wrapping_neg()
    } else {
        low_bits
    }
}

/// Crypto::SignedBigInteger::to_base: the digits in the radix with lowercase letters, after a minus sign for negative
/// values.
pub fn to_base(value: &BigInt, radix: u32) -> String {
    assert!((2..=36).contains(&radix));
    value.to_str_radix(radix)
}

/// The low bits of a magnitude, as libtommath's mp_mod_2d() keeps them.
fn low_bits(value: &BigUint, bit_count: u64) -> BigUint {
    if value.bits() <= bit_count {
        return value.clone();
    }
    let mut digits = value.to_u32_digits();
    let kept_digit_count = bit_count.div_ceil(32) as usize;
    digits.truncate(kept_digit_count);
    if let Some(top_digit) = digits.last_mut()
        && !bit_count.is_multiple_of(32)
    {
        *top_digit &= (1u32 << (bit_count % 32)) - 1;
    }
    BigUint::new(digits)
}

/// Crypto::SignedBigInteger::mod_power_of_two: ℝ(value) modulo 2^power_of_two.
pub fn mod_power_of_two(value: &BigInt, power_of_two: u64) -> Result<BigInt, NumericOperationError> {
    if power_of_two == 0 {
        return Ok(BigInt::zero());
    }

    let is_negative = value.sign() == Sign::Minus;

    // If the number is positive and smaller than the modulus, we can just return it.
    let tommath_digit_count = value.magnitude().bits().div_ceil(TOMMATH_DIGIT_BIT);
    if !is_negative && tommath_digit_count * TOMMATH_DIGIT_BIT <= power_of_two {
        return Ok(value.clone());
    }

    // If the power of two overflows the int type, we don't have enough memory to compute it.
    if i32::try_from(power_of_two).is_err() {
        return Err(NumericOperationError::OutOfMemory);
    }

    let magnitude_modulo = low_bits(value.magnitude(), power_of_two);
    if !is_negative || magnitude_modulo.is_zero() {
        return Ok(BigInt::from(magnitude_modulo));
    }

    // If the result is negative, we need to add the modulus to it.
    Ok(BigInt::from((BigUint::one() << power_of_two) - magnitude_modulo))
}

/// Crypto::UnsignedBigInteger::bitwise_not_fill_to_one_based_index for a value below 2^index.
fn bitwise_not_fill_to_one_based_index(value: &BigUint, index: u64) -> Result<BigUint, NumericOperationError> {
    if index == 0 {
        return Ok(BigUint::zero());
    }
    if i32::try_from(index).is_err() {
        return Err(NumericOperationError::OutOfMemory);
    }
    debug_assert!(value.bits() <= index);
    Ok((BigUint::one() << index) - 1u32 - value)
}

/// 21.2.2.1 BigInt.asIntN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asintn
/// Steps 3-5, given the results of ToIndex(bits) and ToBigInt(bigint).
pub fn as_int_n(bits: u64, bigint: &BigInt) -> Result<BigInt, NumericOperationError> {
    // OPTIMIZATION: mod = bigint (mod 2^0) = 0 < 2^(0-1) = 0.5
    if bits == 0 {
        return Ok(BigInt::zero());
    }

    // OPTIMIZATION: This condition guarantees bigint is within the signed bits-bit range, so steps 3-5 return bigint.
    if bigint.sign() == Sign::Minus && bigint.magnitude().bits() < bits {
        return Ok(bigint.clone());
    }

    // 3. Let mod be ℝ(bigint) modulo 2^bits.
    let modulo = mod_power_of_two(bigint, bits)?;

    // OPTIMIZATION: mod < 2^(bits-1)
    if modulo.is_zero() {
        return Ok(BigInt::zero());
    }

    // 4. If mod ≥ 2^(bits-1), return ℤ(mod - 2^bits); ...
    if modulo.magnitude().bits() >= bits {
        // twos complement decode
        let decoded = bitwise_not_fill_to_one_based_index(modulo.magnitude(), bits)? + 1u32;
        return Ok(BigInt::from_biguint(Sign::Minus, decoded));
    }

    // ... otherwise, return ℤ(mod).
    Ok(modulo)
}

/// 21.2.2.2 BigInt.asUintN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asuintn
/// Step 3, given the results of ToIndex(bits) and ToBigInt(bigint).
pub fn as_uint_n(bits: u64, bigint: &BigInt) -> Result<BigInt, NumericOperationError> {
    // 3. Return the BigInt value that represents ℝ(bigint) modulo 2^bits.
    mod_power_of_two(bigint, bits)
}

/// 6.1.6.2.9 BigInt::leftShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-leftShift
pub fn left_shift(x: &BigInt, y: &BigInt) -> Result<BigInt, NumericOperationError> {
    // AD-HOC: Prevent allocating huge amounts of memory.
    let rhs_bigint = y.magnitude();
    if rhs_bigint.bits() > 32 {
        return Err(NumericOperationError::BigIntSizeExceeded);
    }

    // x is multiplied or divided by 2^|y| as Crypto's pow() computes it.
    let shift = u64::from(exponent_applied_by_crypto_pow(
        rhs_bigint.to_u32().expect("at most 32 bits"),
    ));

    // 1. If y < 0ℤ, then
    if y.sign() == Sign::Minus {
        // a. Return the BigInt value that represents ℝ(x) / 2^-y, rounding down to the nearest integer, including for negative numbers.
        // NOTE: Since y is negative we can just do ℝ(x) / 2^|y|
        let quotient = BigInt::from_biguint(x.sign(), x.magnitude() >> shift);
        let remainder_is_zero = x.magnitude().trailing_zeros().is_none_or(|zeros| zeros >= shift);

        // For positive initial values and no remainder just return quotient
        if remainder_is_zero || x.sign() != Sign::Minus {
            return Ok(quotient);
        }
        // For negative round "down" to the next negative number
        return Ok(quotient - 1);
    }

    // 2. Return the BigInt value that represents ℝ(x) × 2^y.
    Ok(x << shift)
}

/// 6.1.6.2.10 BigInt::signedRightShift ( x, y ), https://tc39.es/ecma262/#sec-numeric-types-bigint-signedRightShift
pub fn signed_right_shift(x: &BigInt, y: &BigInt) -> Result<BigInt, NumericOperationError> {
    // 1. Return BigInt::leftShift(x, -y).
    left_shift(x, &-y)
}

/// 6.1.6.2.3 BigInt::exponentiate ( base, exponent ), https://tc39.es/ecma262/#sec-numeric-types-bigint-exponentiate
pub fn exponentiate(base: &BigInt, exponent: &BigInt) -> Result<BigInt, NumericOperationError> {
    // 1. If exponent < 0ℤ, throw a RangeError exception.
    if exponent.sign() == Sign::Minus {
        return Err(NumericOperationError::NegativeExponent);
    }

    // AD-HOC: Prevent allocating huge amounts of memory.
    if exponent.magnitude().bits() > 32 {
        return Err(NumericOperationError::BigIntSizeExceeded);
    }

    // 2. If base is 0ℤ and exponent is 0ℤ, return 1ℤ.
    // 3. Return the BigInt value that represents ℝ(base) raised to the power ℝ(exponent).
    let exponent = exponent_applied_by_crypto_pow(exponent.magnitude().to_u32().expect("at most 32 bits"));
    Ok(base.pow(exponent))
}
