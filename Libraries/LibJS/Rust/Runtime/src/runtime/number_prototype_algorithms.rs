/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The digit generation of Number.prototype: the parts of toExponential, toFixed, toPrecision and toString(radix)
//! that run once the arguments are validated and x is known to be finite, as functions returning ASCII strings.

use num_bigint::BigUint;
use num_integer::Integer;
use num_traits::{One, ToPrimitive};

use crate::runtime::string_conversions::{BinaryDecomposition, convert_to_decimal_exponential_form, decompose_double};

const DIGITS: [u8; 36] = *b"0123456789abcdefghijklmnopqrstuvwxyz";

const fn count_digits(mut number: u64) -> u8 {
    let mut digits = 0;

    loop {
        number /= 10;
        digits += 1;
        if number == 0 {
            break;
        }
    }

    digits
}

/// Crypto::UnsignedBigInteger::count_digits_in_base(10).
fn count_decimal_digits(value: &BigUint) -> usize {
    value.to_str_radix(10).len()
}

fn power_of_ten(exponent: i32) -> BigUint {
    BigUint::from(10u32).pow(exponent.unsigned_abs())
}

struct SignificandAndExponent {
    significand: BigUint,
    exponent: i32,
}

fn compute_significand_and_exponent_with_precision(number: f64, precision: i32) -> SignificandAndExponent {
    let result = convert_to_decimal_exponential_form(number);
    let mut exponent = result.exponent + i32::from(count_digits(result.fraction)) - 1;

    // Decompose the number into its exact binary representation. An IEEE-754 double is exactly equal to:
    //
    //     binary_significand * 2 ^ binary_exponent
    let decomposition = decompose_double(number);
    let binary_significand = BigUint::from(decomposition.significand);
    let binary_exponent = decomposition.exponent;

    // Compute the significand from the binary representation using exact arithmetic. We are effectively after:
    //
    //    significand = round(number * 10 ^ (precision - exponent - 1))
    //
    // Using the binary representation of the number, that becomes:
    //
    //    significand = round(binary_significand * (2 ^ binary_exponent) * (10 ^ (precision - exponent - 1)))
    //
    // Below, we arrange this as a fraction, placing any negative values into the denominator to ensure that the math
    // involves only unsigned integers.
    let compute_significand = |exponent: i32| {
        let mut numerator = binary_significand.clone();
        let mut denominator = BigUint::one();

        // 2 ^ binary_exponent
        if binary_exponent > 0 {
            numerator <<= binary_exponent.unsigned_abs();
        } else if binary_exponent < 0 {
            denominator <<= binary_exponent.unsigned_abs();
        }

        // 10 ^ (precision - exponent - 1)
        let scale = precision - exponent - 1;
        if scale > 0 {
            numerator *= power_of_ten(scale);
        } else if scale < 0 {
            denominator *= power_of_ten(scale);
        }

        let (mut quotient, remainder) = numerator.div_rem(&denominator);

        // Round half-up to distinguish between equally valid candidates.
        if (remainder << 1u32) >= denominator {
            quotient += 1u32;
        }

        quotient
    };

    // The exponent computed from Ryu can be off-by-one at boundaries (e.g. when rounding 9.9999... up to 10.0). If the
    // resulting digit count is incorrect, we adjust the exponent and recompute the significand.
    let mut significand = compute_significand(exponent);

    let precision_digit_count = precision.unsigned_abs() as usize;
    let digit_count = count_decimal_digits(&significand);
    if digit_count > precision_digit_count {
        exponent += 1;
        significand = compute_significand(exponent);
    } else if digit_count < precision_digit_count {
        exponent -= 1;
        significand = compute_significand(exponent);
    }

    // When the computed significand is exactly (10 ^ (precision - 1)), then we have two candidate representations of
    // the original number:
    //
    //    candidate_a = significand * (10 ^ (exponent - precision + 1))
    //    candidate_b = alternate * (10 ^ (exponent - precision))
    //
    // Where alternate = compute_significand(exponent - 1).
    //
    // We want to know which candidate is closest to the original number (x). Tie breaks go to the larger value
    // (candidate_a), so we only pick candidate_b if:
    //
    //    candidate_a - x > x - candidate_b
    //    candidate_a + candidate_b > 2 * x
    //
    // Substituting the candidates and simplifying the left-hand side of this comparison, we have:
    //
    //    lhs = significand * (10 ^ (exponent - precision + 1)) + alternate * (10 ^ (exponent - precision))
    //    lhs = (significand * 10 + alternate) * (10 ^ (exponent - precision))
    //
    // And substituting the binary decomposition for the right-hand side of this comparison, we have:
    //
    //    rhs = 2 * binary_significand * (2 ^ binary_exponent)
    //
    // Similar to `compute_significand` above, we take care to clear any negative exponents to ensure that the math
    // involves only unsigned integers.
    if significand == power_of_ten(precision - 1) {
        let alternate = compute_significand(exponent - 1);

        if count_decimal_digits(&alternate) == precision_digit_count {
            let mut lhs = &significand * 10u32 + &alternate;
            let mut rhs = &binary_significand * 2u32;

            // 10 ^ (exponent - precision)
            let scale = exponent - precision;
            if scale > 0 {
                lhs *= power_of_ten(scale);
            } else if scale < 0 {
                rhs *= power_of_ten(scale);
            }

            // 2 ^ binary_exponent
            if binary_exponent > 0 {
                rhs <<= binary_exponent.unsigned_abs();
            } else if binary_exponent < 0 {
                lhs <<= binary_exponent.unsigned_abs();
            }

            if lhs > rhs {
                significand = alternate;
                exponent -= 1;
            }
        }
    }

    SignificandAndExponent { significand, exponent }
}

fn compute_to_fixed_scaled_integer(number: f64, fraction_digits: u32) -> BigUint {
    let decomposition = decompose_double(number);
    let binary_significand = BigUint::from(decomposition.significand);
    let binary_exponent = decomposition.exponent;

    let numerator = binary_significand * BigUint::from(5u32).pow(fraction_digits);
    let binary_scale = binary_exponent + fraction_digits as i32;
    if binary_scale >= 0 {
        return numerator << binary_scale.unsigned_abs();
    }

    let denominator = BigUint::one() << binary_scale.unsigned_abs();
    let (mut quotient, remainder) = numerator.div_rem(&denominator);

    // Pick the larger integer if x * 10^f is exactly between two candidates.
    if (remainder << 1u32) >= denominator {
        quotient += 1u32;
    }

    quotient
}

// OPTIMIZATION: For f ≤ 27, 5^f fits in a u64, so binary_significand * 5^f fits in 116 bits, and n can be computed exactly
//               with 128-bit arithmetic instead of arbitrary-precision integers. Returns None when n doesn't
//               fit in a u64, or when the double is large enough to need a left shift; the caller falls back to
//               compute_to_fixed_scaled_integer() in those cases.
fn try_compute_to_fixed_scaled_integer_in_u64(number: f64, fraction_digits: u32) -> Option<u64> {
    const POWERS_OF_FIVE: [u64; 28] = {
        let mut powers = [0u64; 28];
        powers[0] = 1;
        let mut i = 1;
        while i < powers.len() {
            powers[i] = powers[i - 1] * 5;
            i += 1;
        }
        powers
    };

    let power_of_five = *POWERS_OF_FIVE.get(fraction_digits as usize)?;

    let BinaryDecomposition {
        significand: binary_significand,
        exponent: binary_exponent,
    } = decompose_double(number);

    let binary_scale = binary_exponent + fraction_digits as i32;
    if binary_scale >= 0 {
        return None;
    }

    // n = round(numerator / 2^shift), picking the larger integer on a tie. Since numerator < 2^116, n is 0 whenever
    // shift ≥ 117. Otherwise, adding one at the bit below the cut and then dropping it rounds ties upward.
    let shift = binary_scale.unsigned_abs();
    if shift >= 117 {
        return Some(0);
    }

    let numerator = u128::from(binary_significand) * u128::from(power_of_five);
    let n = ((numerator >> (shift - 1)) + 1) >> 1;
    u64::try_from(n).ok()
}

fn exponent_sign_and_digits(exponent: i32) -> (char, u32) {
    if exponent < 0 {
        ('-', exponent.unsigned_abs())
    } else {
        ('+', exponent.unsigned_abs())
    }
}

/// 21.1.3.2 Number.prototype.toExponential ( fractionDigits ), https://tc39.es/ecma262/#sec-number.prototype.toexponential
/// Steps 6-15, for a finite x. fraction_digits is f, or None when fractionDigits is undefined; f is at most 100.
pub fn to_exponential(number: f64, fraction_digits: Option<u32>) -> String {
    debug_assert!(number.is_finite());
    debug_assert!(fraction_digits.is_none_or(|digits| digits <= 100));

    let mut fraction_digit_count = fraction_digits.unwrap_or(0);

    // 6. Set x to ℝ(x).
    let mut number = number;

    // 7. Let s be the empty String.
    let mut sign = "";

    let mut number_string;
    let exponent;

    // 8. If x < 0, then
    if number < 0.0 {
        // a. Set s to "-".
        sign = "-";

        // b. Set x to -x.
        number = -number;
    }

    // 9. If x = 0, then
    if number == 0.0 {
        // a. Let m be the String value consisting of f + 1 occurrences of the code unit 0x0030 (DIGIT ZERO).
        number_string = "0".repeat(fraction_digit_count as usize + 1);

        // b. Let e be 0.
        exponent = 0;
    }
    // 10. Else,
    else {
        let significand;

        // a. If fractionDigits is not undefined, then
        if fraction_digits.is_some() {
            // i. Let e and n be integers such that 10^f ≤ n < 10^(f+1) and for which n × 10^(e-f) - x is as close to
            //    zero as possible. If there are two such sets of e and n, pick the e and n for which n × 10^(e-f) is
            //    larger.
            let result = compute_significand_and_exponent_with_precision(number, fraction_digit_count as i32 + 1);

            significand = result.significand;
            exponent = result.exponent;
        }
        // b. Else,
        else {
            // i. Let e, n, and f be integers such that f ≥ 0, 10^f ≤ n < 10^(f+1), 𝔽(n × 10^(e-f)) is 𝔽(x), and f is
            //    as small as possible. Note that the decimal representation of n has f + 1 digits, n is not divisible
            //    by 10, and the least significant digit of n is not necessarily uniquely determined by these criteria.
            let result = convert_to_decimal_exponential_form(number);

            significand = BigUint::from(result.fraction);
            fraction_digit_count = u32::from(count_digits(result.fraction)) - 1;
            exponent = result.exponent + fraction_digit_count as i32;
        }

        // c. Let m be the String value consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
        number_string = significand.to_str_radix(10);
    }

    // 11. If f ≠ 0, then
    if fraction_digit_count != 0 {
        // a. Let a be the first code unit of m.
        // b. Let b be the other f code units of m.
        // c. Set m to the string-concatenation of a, ".", and b.
        number_string.insert(1, '.');
    }

    // 12. If e = 0, then
    //     a. Let c be "+".
    //     b. Let d be "0".
    // 13. Else,
    //     a. If e > 0, let c be "+".
    //     b. Else,
    //         i. Assert: e < 0.
    //         ii. Let c be "-".
    //         iii. Set e to -e.
    //     c. Let d be the String value consisting of the digits of the decimal representation of e (in order, with no leading zeroes).
    let (exponent_sign, exponent_digits) = exponent_sign_and_digits(exponent);

    // 14. Set m to the string-concatenation of m, "e", c, and d.
    // 15. Return the string-concatenation of s and m.
    format!("{sign}{number_string}e{exponent_sign}{exponent_digits}")
}

/// 21.1.3.3 Number.prototype.toFixed ( fractionDigits ), https://tc39.es/ecma262/#sec-number.prototype.tofixed
/// Steps 7-12, for a finite x and an f of at most 100. Returns None for step 10, where x ≥ 10^21 and the result is
/// ToString of the original x.
pub fn to_fixed(number: f64, fraction_digits: u32) -> Option<FixedPointString> {
    debug_assert!(number.is_finite());
    debug_assert!(fraction_digits <= 100);

    // 7. Set x to ℝ(x).
    let mut number = number;

    // 8. Let s be the empty String.
    // 9. If x < 0, then
    //    a. Set s to "-".
    let s = if number < 0.0 { "-" } else { "" };
    //    b. Set x to -x.
    if number < 0.0 {
        number = -number;
    }

    // 10. If x ≥ 10^21, then
    //     a. Let m be ! ToString(𝔽(x)).
    if number >= 1e+21 {
        return None;
    }

    // 11. Else,
    //     a. Let n be an integer for which n / (10^f) - x is as close to zero as possible. If there are two such n, pick the larger n.
    //     b. If n = 0, let m be the String "0". Otherwise, let m be the String value consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
    //     c. If f ≠ 0, then
    //         i. Let k be the length of m.
    //         ii. If k ≤ f, then
    //             1. Let z be the String value consisting of f + 1 - k occurrences of the code unit 0x0030 (DIGIT ZERO).
    //             2. Set m to the string-concatenation of z and m.
    //             3. Set k to f + 1.
    //         iii. Let a be the first k - f code units of m.
    //         iv. Let b be the other f code units of m.
    //         v. Set m to the string-concatenation of a, ".", and b.
    // 12. Return the string-concatenation of s and m.

    let fraction_digit_count = fraction_digits as usize;

    let mut u64_digit_buffer = [0; 20];
    let big_integer_digits;
    let digits: &[u8] = match try_compute_to_fixed_scaled_integer_in_u64(number, fraction_digits) {
        Some(n) => write_decimal_digits(n, &mut u64_digit_buffer),
        None => {
            big_integer_digits = compute_to_fixed_scaled_integer(number, fraction_digits).to_str_radix(10);
            big_integer_digits.as_bytes()
        }
    };

    // NB: Build the result in a single pass: the sign, then m zero-padded to at least f + 1 digits (if f ≠ 0), with "."
    //     inserted before the last f digits.
    let k = digits.len();
    let padded_length = if fraction_digit_count != 0 {
        k.max(fraction_digit_count + 1)
    } else {
        k
    };
    let zero_count = padded_length - k;
    let whole_length = padded_length - fraction_digit_count;

    let mut result = FixedPointString::default();
    result.push_bytes(s.as_bytes());
    for i in 0..padded_length {
        if i == whole_length {
            result.push_bytes(b".");
        }
        result.push_bytes(if i < zero_count {
            b"0"
        } else {
            &digits[i - zero_count..=i - zero_count]
        });
    }

    Some(result)
}

/// What to_fixed() makes of an x below 10^21 with at most 100 fraction digits: a sign, at most 121 digits and a
/// decimal point, kept inline.
pub struct FixedPointString {
    bytes: [u8; 128],
    length: usize,
}

impl Default for FixedPointString {
    fn default() -> Self {
        Self {
            bytes: [0; 128],
            length: 0,
        }
    }
}

impl FixedPointString {
    fn push_bytes(&mut self, ascii: &[u8]) {
        self.bytes[self.length..self.length + ascii.len()].copy_from_slice(ascii);
        self.length += ascii.len();
    }
}

impl core::ops::Deref for FixedPointString {
    type Target = str;

    fn deref(&self) -> &str {
        core::str::from_utf8(&self.bytes[..self.length]).expect("to_fixed() writes ASCII")
    }
}

/// Writes the decimal digits of `number` to the end of `buffer`, and returns them.
fn write_decimal_digits(mut number: u64, buffer: &mut [u8; 20]) -> &[u8] {
    let mut start = buffer.len();
    loop {
        start -= 1;
        buffer[start] = DIGITS[(number % 10) as usize];
        number /= 10;
        if number == 0 {
            break;
        }
    }
    &buffer[start..]
}

/// 21.1.3.5 Number.prototype.toPrecision ( precision ), https://tc39.es/ecma262/#sec-number.prototype.toprecision
/// Steps 6-14, for a finite x and a p from 1 to 100.
pub fn to_precision(number: f64, precision: u32) -> String {
    debug_assert!(number.is_finite());
    debug_assert!((1..=100).contains(&precision));

    let precision_as_exponent = precision as i32;

    // 6. Set x to ℝ(x).
    let mut number = number;

    // 7. Let s be the empty String.
    let mut sign = "";

    let mut number_string;
    let exponent;

    // 8. If x < 0, then
    if number < 0.0 {
        // a. Set s to the code unit 0x002D (HYPHEN-MINUS).
        sign = "-";

        // b. Set x to -x.
        number = -number;
    }

    // 9. If x = 0, then
    if number == 0.0 {
        // a. Let m be the String value consisting of p occurrences of the code unit 0x0030 (DIGIT ZERO).
        number_string = "0".repeat(precision as usize);

        // b. Let e be 0.
        exponent = 0;
    }
    // 10. Else,
    else {
        // a. Let e and n be integers such that 10^(p-1) ≤ n < 10^p and for which n × 10^(e-p+1) - x is as close to zero
        //    as possible. If there are two such sets of e and n, pick the e and n for which n × 10^(e-p+1) is larger.
        let result = compute_significand_and_exponent_with_precision(number, precision_as_exponent);
        exponent = result.exponent;

        // b. Let m be the String value consisting of the digits of the decimal representation of n (in order, with no
        //    leading zeroes).
        number_string = result.significand.to_str_radix(10);

        // c. If e < -6 or e ≥ p, then
        if exponent < -6 || exponent >= precision_as_exponent {
            // i. Assert: e ≠ 0.
            assert!(exponent != 0);

            // ii. If p ≠ 1, then
            if precision != 1 {
                // 1. Let a be the first code unit of m.
                // 2. Let b be the other p - 1 code units of m.
                // 3. Set m to the string-concatenation of a, ".", and b.
                number_string.insert(1, '.');
            }

            // iii. If e > 0, then
            //      1. Let c be the code unit 0x002B (PLUS SIGN).
            // iv. Else,
            //     1. Assert: e < 0.
            //     2. Let c be the code unit 0x002D (HYPHEN-MINUS).
            //     3. Set e to -e.
            let (exponent_sign, exponent_digits) = exponent_sign_and_digits(exponent);

            // v. Let d be the String value consisting of the digits of the decimal representation of e (in order, with no leading zeroes).
            // vi. Return the string-concatenation of s, m, the code unit 0x0065 (LATIN SMALL LETTER E), c, and d.
            return format!("{sign}{number_string}e{exponent_sign}{exponent_digits}");
        }
    }

    // 11. If e = p - 1, return the string-concatenation of s and m.
    if exponent == precision_as_exponent - 1 {
        return format!("{sign}{number_string}");
    }

    // 12. If e ≥ 0, then
    if exponent >= 0 {
        // a. Set m to the string-concatenation of the first e + 1 code units of m, the code unit 0x002E (FULL STOP), and the remaining p - (e + 1) code units of m.
        number_string.insert(exponent.unsigned_abs() as usize + 1, '.');
    }
    // 13. Else,
    else {
        // a. Set m to the string-concatenation of the code unit 0x0030 (DIGIT ZERO), the code unit 0x002E (FULL STOP), -(e + 1) occurrences of the code unit 0x0030 (DIGIT ZERO), and the String m.
        let zero_count = (exponent + 1).unsigned_abs() as usize;
        number_string = format!("0.{}{number_string}", "0".repeat(zero_count));
    }

    // 14. Return the string-concatenation of s and m.
    format!("{sign}{number_string}")
}

/// 21.1.3.6 Number.prototype.toString ( [ radix ] ), https://tc39.es/ecma262/#sec-number.prototype.tostring
/// Step 6, for a radixMV from 2 to 36 other than 10, which step 5 handles with ToString.
pub fn to_string_with_radix(number: f64, radix: u32) -> String {
    debug_assert!((2..=36).contains(&radix) && radix != 10);

    // 6. Return the String representation of this Number value using the radix specified by radixMV. Letters a-z are used for digits with values 10 through 35. The precise algorithm is implementation-defined, however the algorithm should be a generalization of that specified in 6.1.6.1.20.
    if number == f64::INFINITY {
        return "Infinity".to_string();
    }
    if number == f64::NEG_INFINITY {
        return "-Infinity".to_string();
    }
    if number.is_nan() {
        return "NaN".to_string();
    }
    if number == 0.0 {
        return "0".to_string();
    }

    let mut number = number;
    let negative = number < 0.0;
    if negative {
        number *= -1.0;
    }

    let mut int_part = number.floor();
    let mut fractional_digits: Vec<u32> = Vec::new();

    if number != int_part {
        let decomposition = decompose_double(number);
        assert!(decomposition.exponent < 0);

        // NB: At powers of two above the smallest normal value, the preceding double is half as far away as the next.
        //     Scale by four in that case, or by two otherwise, so both rounding boundaries are integers.
        let asymmetric_boundaries = decomposition.significand == (1 << 52) && decomposition.exponent > -1074;
        let scale = if asymmetric_boundaries { 2u32 } else { 1 };
        let denominator = BigUint::one() << (decomposition.exponent.unsigned_abs() + scale);
        let mut remainder = (BigUint::from(decomposition.significand) << scale) % &denominator;
        let mut lower_boundary = BigUint::one();
        let mut upper_boundary = BigUint::from(if asymmetric_boundaries { 2u32 } else { 1 });
        let inclusive_boundaries = decomposition.significand & 1 == 0;
        let mut significand_is_odd = (int_part as u64) & 1 != 0;

        // NB: Generate digits until truncating or rounding up produces a value that rounds back to the input double.
        //     Exact arithmetic preserves the rounding boundaries even for subnormal inputs and non-binary radices.
        loop {
            let (quotient, division_remainder) = (remainder * radix).div_rem(&denominator);
            let digit = quotient.to_u32().expect("a digit is below the radix");
            assert!(digit < radix);
            fractional_digits.push(digit);
            // NB: For odd radices, the parity depends on all digits, including the integer part.
            significand_is_odd = ((radix & 1 != 0) && significand_is_odd) != (digit & 1 != 0);
            remainder = division_remainder;
            lower_boundary *= radix;
            upper_boundary *= radix;

            let can_round_down = remainder < lower_boundary || (inclusive_boundaries && remainder == lower_boundary);
            let upper_candidate = &remainder + &upper_boundary;
            let can_round_up =
                upper_candidate > denominator || (inclusive_boundaries && upper_candidate == denominator);
            if !can_round_down && !can_round_up {
                continue;
            }

            let twice_remainder = &remainder << 1u32;
            let round_up = can_round_up
                && (!can_round_down
                    || twice_remainder > denominator
                    || (twice_remainder == denominator && significand_is_odd));
            if round_up {
                while fractional_digits.last() == Some(&(radix - 1)) {
                    fractional_digits.pop();
                }
                match fractional_digits.last_mut() {
                    Some(last) => *last += 1,
                    None => int_part += 1.0,
                }
            }
            while fractional_digits.last() == Some(&0) {
                fractional_digits.pop();
            }
            break;
        }
    }

    let mut backwards_characters = Vec::new();

    if int_part == 0.0 {
        backwards_characters.push(b'0');
    } else {
        let radix = f64::from(radix);
        while int_part > 0.0 {
            backwards_characters.push(DIGITS[(int_part % radix).floor() as usize]);
            int_part /= radix;
            int_part = int_part.floor();
        }
    }

    let mut builder = String::with_capacity(backwards_characters.len() + fractional_digits.len() + 2);
    if negative {
        builder.push('-');
    }

    // Reverse characters;
    builder.extend(
        backwards_characters
            .iter()
            .rev()
            .map(|character| char::from(*character)),
    );

    if !fractional_digits.is_empty() {
        builder.push('.');
        builder.extend(
            fractional_digits
                .iter()
                .map(|digit| char::from(DIGITS[*digit as usize])),
        );
    }

    builder
}
