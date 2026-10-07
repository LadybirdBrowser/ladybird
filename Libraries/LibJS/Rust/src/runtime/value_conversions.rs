/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The numeric conversions on values that need no objects and no VM, as functions over the numbers and strings the spec
//! operations have already obtained. The Value methods apply ToNumber or ToPrimitive and then call these.

use core::cmp::Ordering;

use num_bigint::{BigInt, BigUint};
use num_traits::Zero;

use crate::interpreter::vm::Vm;
use crate::runtime::big_int_algorithms::unsigned_to_double;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::string_conversions::parse_number_f64;

pub const MAX_ARRAY_LIKE_INDEX: f64 = 9007199254740991.0;

/// An error that one of these numeric operations throws, named after its ErrorType. The operations return it so that
/// the caller, which has a VM, can throw the matching error object.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NumericOperationError {
    /// A RangeError with ErrorType::InvalidIndex.
    InvalidIndex,
    /// A RangeError with ErrorType::BigIntFromNonIntegral.
    BigIntFromNonIntegral,
    /// A RangeError with ErrorType::BigIntSizeExceeded.
    BigIntSizeExceeded,
    /// A RangeError with ErrorType::NegativeExponent.
    NegativeExponent,
    /// The InternalError that TRY_OR_THROW_OOM throws when an operation reports ENOMEM.
    OutOfMemory,
}

impl NumericOperationError {
    /// The constructor and message of the error to throw.
    pub fn error_kind_and_type(self) -> (ErrorKind, ErrorType) {
        match self {
            Self::InvalidIndex => (ErrorKind::RangeError, ErrorType::InvalidIndex),
            Self::BigIntFromNonIntegral => (ErrorKind::RangeError, ErrorType::BigIntFromNonIntegral),
            Self::BigIntSizeExceeded => (ErrorKind::RangeError, ErrorType::BigIntSizeExceeded),
            Self::NegativeExponent => (ErrorKind::RangeError, ErrorType::NegativeExponent),
            Self::OutOfMemory => (ErrorKind::InternalError, ErrorType::OutOfMemory),
        }
    }

    pub fn throw_completion<T>(self, vm: &Vm) -> ThrowCompletionOr<T> {
        let (kind, error_type) = self.error_kind_and_type();
        vm.throw_completion(kind, error_type, &[])
    }
}

const JS_WHITESPACE_CODE_UNITS: [u16; 25] = [
    0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x0020, 0x00A0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
    0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF,
];

/// The code units that StringToNumber and StringToBigInt trim: WhiteSpace and LineTerminator.
pub fn is_js_whitespace(code_unit: u16) -> bool {
    if code_unit < 0xA0 {
        return matches!(code_unit, 0x09..=0x0D | 0x20);
    }
    JS_WHITESPACE_CODE_UNITS.contains(&code_unit)
}

pub fn trim_js_whitespace(string: &[u16]) -> &[u16] {
    let Some(start) = string.iter().position(|code_unit| !is_js_whitespace(*code_unit)) else {
        return &[];
    };
    let end = string
        .iter()
        .rposition(|code_unit| !is_js_whitespace(*code_unit))
        .map_or(start, |index| index + 1);
    &string[start..end]
}

fn is_ascii_digit(code_unit: u16) -> bool {
    (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit)
}

fn is_ascii_binary_digit(code_unit: u16) -> bool {
    code_unit == u16::from(b'0') || code_unit == u16::from(b'1')
}

fn is_ascii_octal_digit(code_unit: u16) -> bool {
    (u16::from(b'0')..=u16::from(b'7')).contains(&code_unit)
}

fn is_ascii_hex_digit(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_hexdigit())
}

fn is_ascii_number(code_unit: u16) -> bool {
    is_ascii_digit(code_unit) || b".eE+-".iter().any(|byte| code_unit == u16::from(*byte))
}

fn equals_ascii(code_units: &[u16], ascii: &str) -> bool {
    code_units.len() == ascii.len()
        && code_units
            .iter()
            .zip(ascii.bytes())
            .all(|(code_unit, byte)| *code_unit == u16::from(byte))
}

fn starts_with_either(text: &[u16], lower_prefix: &str, upper_prefix: &str) -> bool {
    let starts_with = |prefix: &str| text.len() >= prefix.len() && equals_ascii(&text[..prefix.len()], prefix);
    starts_with(lower_prefix) || starts_with(upper_prefix)
}

/// Parses validated digits in the given base, as Crypto::UnsignedBigInteger::from_base does.
pub fn unsigned_big_integer_from_base(base: u8, digits: &[u16]) -> BigUint {
    if digits.is_empty() {
        return BigUint::zero();
    }
    let bytes: Vec<u8> = digits.iter().map(|code_unit| *code_unit as u8).collect();
    BigUint::parse_bytes(&bytes, u32::from(base)).expect("the digits were validated for the base")
}

pub struct StringNumericLiteral<'a> {
    pub literal: &'a [u16],
    pub base: u8,
}

fn parse_number_text(text: &[u16]) -> Option<StringNumericLiteral<'_>> {
    let check_prefix = |lower_prefix: &str, upper_prefix: &str| {
        if text.len() <= 2 {
            return false;
        }
        starts_with_either(text, lower_prefix, upper_prefix)
    };

    // https://tc39.es/ecma262/#sec-tonumber-applied-to-the-string-type
    let (literal, base, is_valid_digit): (&[u16], u8, fn(u16) -> bool) = if check_prefix("0b", "0B") {
        (&text[2..], 2, is_ascii_binary_digit)
    } else if check_prefix("0o", "0O") {
        (&text[2..], 8, is_ascii_octal_digit)
    } else if check_prefix("0x", "0X") {
        (&text[2..], 16, is_ascii_hex_digit)
    } else {
        (text, 10, is_ascii_number)
    };

    if !literal.iter().all(|code_unit| is_valid_digit(*code_unit)) {
        return None;
    }

    Some(StringNumericLiteral { literal, base })
}

pub fn parse_string_numeric_literal(string: &[u16]) -> Option<StringNumericLiteral<'_>> {
    parse_number_text(trim_js_whitespace(string))
}

/// 7.1.4.1.1 StringToNumber ( str ), https://tc39.es/ecma262/#sec-stringtonumber
pub fn string_to_number(string: &[u16]) -> f64 {
    // 1. Let text be StringToCodePoints(str).
    let text = trim_js_whitespace(string);

    // 2. Let literal be ParseText(text, StringNumericLiteral).
    if text.is_empty() {
        return 0.0;
    }
    if equals_ascii(text, "Infinity") || equals_ascii(text, "+Infinity") {
        return f64::INFINITY;
    }
    if equals_ascii(text, "-Infinity") {
        return f64::NEG_INFINITY;
    }

    let result = parse_number_text(text);

    // 3. If literal is a List of errors, return NaN.
    let Some(result) = result else {
        return f64::NAN;
    };

    // 4. Return StringNumericValue of literal.
    if result.base != 10 {
        return unsigned_to_double(&unsigned_big_integer_from_base(result.base, result.literal));
    }

    parse_number_f64(text).unwrap_or(f64::NAN)
}

struct BigIntParseResult<'a> {
    literal: &'a [u16],
    base: u8,
    is_negative: bool,
}

fn parse_bigint_text(mut text: &[u16]) -> Option<BigIntParseResult<'_>> {
    let parse_for_prefixed_base = |lower_prefix: &str, upper_prefix: &str, validator: fn(u16) -> bool| {
        if text.len() <= 2 {
            return false;
        }
        if !starts_with_either(text, lower_prefix, upper_prefix) {
            return false;
        }
        text[2..].iter().all(|code_unit| validator(*code_unit))
    };

    if parse_for_prefixed_base("0b", "0B", is_ascii_binary_digit) {
        return Some(BigIntParseResult {
            literal: &text[2..],
            base: 2,
            is_negative: false,
        });
    }
    if parse_for_prefixed_base("0o", "0O", is_ascii_octal_digit) {
        return Some(BigIntParseResult {
            literal: &text[2..],
            base: 8,
            is_negative: false,
        });
    }
    if parse_for_prefixed_base("0x", "0X", is_ascii_hex_digit) {
        return Some(BigIntParseResult {
            literal: &text[2..],
            base: 16,
            is_negative: false,
        });
    }

    let mut is_negative = false;
    if text.first() == Some(&u16::from(b'-')) {
        text = &text[1..];
        is_negative = true;
    } else if text.first() == Some(&u16::from(b'+')) {
        text = &text[1..];
    }

    if !text.iter().all(|code_unit| is_ascii_digit(*code_unit)) {
        return None;
    }

    Some(BigIntParseResult {
        literal: text,
        base: 10,
        is_negative,
    })
}

/// 7.1.14 StringToBigInt ( str ), https://tc39.es/ecma262/#sec-stringtobigint
/// This accepts a lone sign as 0n.
pub fn string_to_bigint(string: &[u16]) -> Option<BigInt> {
    // 1. Let text be StringToCodePoints(str).
    let text = trim_js_whitespace(string);

    // 2. Let literal be ParseText(text, StringIntegerLiteral).
    let result = parse_bigint_text(text);

    // 3. If literal is a List of errors, return undefined.
    let result = result?;

    // 4. Let mv be the MV of literal.
    // 5. Assert: mv is an integer.
    let magnitude = BigInt::from(unsigned_big_integer_from_base(result.base, result.literal));

    // 6. Return ℤ(mv).
    Some(if result.is_negative { -magnitude } else { magnitude })
}

/// x modulo y, https://tc39.es/ecma262/#eqn-modulo
fn modulo(x: f64, y: f64) -> f64 {
    // The notation “x modulo y” (y must be finite and non-zero) computes a value k of the same sign as y (or zero) such that abs(k) < abs(y) and x - k = q × y for some integer q.
    assert!(y != 0.0 && y.is_finite());
    let r = x % y;
    if r < 0.0 { r + y } else { r }
}

/// Steps 2-4 shared by ToInt32 and the narrower integer conversions: the integer with the sign of number and the
/// magnitude floor(abs(ℝ(number))), modulo 2^bits. None stands for the +0𝔽 that step 2 returns.
fn truncate_modulo_power_of_two(number: f64, modulus: f64) -> Option<f64> {
    // 2. If number is not finite or number is either +0𝔽 or -0𝔽, return +0𝔽.
    if !number.is_finite() || number == 0.0 {
        return None;
    }

    // 3. Let int be the mathematical value whose sign is the sign of number and whose magnitude is floor(abs(ℝ(number))).
    let mut int_val = number.abs().floor();
    if number.is_sign_negative() {
        int_val = -int_val;
    }

    // 4. Let intNbit be int modulo 2^N.
    Some(modulo(int_val, modulus))
}

/// 7.1.6 ToInt32 ( argument ), https://tc39.es/ecma262/#sec-toint32
pub fn to_i32(number: f64) -> i32 {
    // 1. Let number be ? ToNumber(argument).

    // 2-4.
    let Some(mut int32bit) = truncate_modulo_power_of_two(number, f64::from(u32::MAX) + 1.0) else {
        return 0;
    };

    // 5. If int32bit ≥ 2^31, return 𝔽(int32bit - 2^32); otherwise return 𝔽(int32bit).
    if int32bit >= 2147483648.0 {
        int32bit -= 4294967296.0;
    }
    int32bit as i32
}

/// 7.1.7 ToUint32 ( argument ), https://tc39.es/ecma262/#sec-touint32
pub fn to_u32(number: f64) -> u32 {
    to_i32(number) as u32
}

/// 7.1.8 ToInt16 ( argument ), https://tc39.es/ecma262/#sec-toint16
pub fn to_i16(number: f64) -> i16 {
    // 1. Let number be ? ToNumber(argument).

    // 2-4.
    let Some(mut int16bit) = truncate_modulo_power_of_two(number, f64::from(u16::MAX) + 1.0) else {
        return 0;
    };

    // 5. If int16bit ≥ 2^15, return 𝔽(int16bit - 2^16); otherwise return 𝔽(int16bit).
    if int16bit >= 32768.0 {
        int16bit -= 65536.0;
    }
    int16bit as i16
}

/// 7.1.9 ToUint16 ( argument ), https://tc39.es/ecma262/#sec-touint16
pub fn to_u16(number: f64) -> u16 {
    // 1. Let number be ? ToNumber(argument).

    // 2-4.
    let Some(int16bit) = truncate_modulo_power_of_two(number, f64::from(u16::MAX) + 1.0) else {
        return 0;
    };

    // 5. Return 𝔽(int16bit).
    int16bit as u16
}

/// 7.1.10 ToInt8 ( argument ), https://tc39.es/ecma262/#sec-toint8
pub fn to_i8(number: f64) -> i8 {
    // 1. Let number be ? ToNumber(argument).

    // 2-4.
    let Some(mut int8bit) = truncate_modulo_power_of_two(number, f64::from(u8::MAX) + 1.0) else {
        return 0;
    };

    // 5. If int8bit ≥ 2^7, return 𝔽(int8bit - 2^8); otherwise return 𝔽(int8bit).
    if int8bit >= 128.0 {
        int8bit -= 256.0;
    }
    int8bit as i8
}

/// 7.1.11 ToUint8 ( argument ), https://tc39.es/ecma262/#sec-touint8
pub fn to_u8(number: f64) -> u8 {
    // 1. Let number be ? ToNumber(argument).

    // 2-4.
    let Some(int8bit) = truncate_modulo_power_of_two(number, f64::from(u8::MAX) + 1.0) else {
        return 0;
    };

    // 5. Return 𝔽(int8bit).
    int8bit as u8
}

/// 7.1.12 ToUint8Clamp ( argument ), https://tc39.es/ecma262/#sec-touint8clamp
pub fn to_u8_clamp(number: f64) -> u8 {
    // 1. Let number be ? ToNumber(argument).

    // 2. If number is NaN, return +0𝔽.
    if number.is_nan() {
        return 0;
    }

    let value = number;

    // 3. If ℝ(number) ≤ 0, return +0𝔽.
    if value <= 0.0 {
        return 0;
    }

    // 4. If ℝ(number) ≥ 255, return 255𝔽.
    if value >= 255.0 {
        return 255;
    }

    // 5. Let f be floor(ℝ(number)).
    let int_val = value.floor();

    // 6. If f + 0.5 < ℝ(number), return 𝔽(f + 1).
    if int_val + 0.5 < value {
        return (int_val + 1.0) as u8;
    }

    // 7. If ℝ(number) < f + 0.5, return 𝔽(f).
    if value < int_val + 0.5 {
        return int_val as u8;
    }

    // 8. If f is odd, return 𝔽(f + 1).
    if int_val % 2.0 == 1.0 {
        return (int_val + 1.0) as u8;
    }

    // 9. Return 𝔽(f).
    int_val as u8
}

/// 7.1.20 ToLength ( argument ), https://tc39.es/ecma262/#sec-tolength
pub fn to_length(number: f64) -> u64 {
    // 1. Let len be ? ToIntegerOrInfinity(argument).
    let len = to_integer_or_infinity(number);

    // 2. If len ≤ 0, return +0𝔽.
    if len <= 0.0 {
        return 0;
    }

    // 3. Return 𝔽(min(len, 2^53 - 1)).
    len.min(MAX_ARRAY_LIKE_INDEX) as u64
}

/// 7.1.22 ToIndex ( argument ), https://tc39.es/ecma262/#sec-toindex
/// Steps 2.a-e for a value that is not undefined, given ToNumber of it; step 1 returns 0 for undefined.
pub fn to_index(number: f64) -> Result<u64, NumericOperationError> {
    // 2. Else,
    // a. Let integer be ? ToIntegerOrInfinity(value).
    let integer = to_integer_or_infinity(number);

    // OPTIMIZATION: If the value is negative, ToLength normalizes it to 0, and we fail the SameValue comparison below.
    //               Bail out early instead.
    if integer < 0.0 {
        return Err(NumericOperationError::InvalidIndex);
    }

    // b. Let clamped be ! ToLength(𝔽(integer)).
    let clamped = to_length(integer);

    // c. If SameValue(𝔽(integer), clamped) is false, throw a RangeError exception.
    if integer != clamped as f64 {
        return Err(NumericOperationError::InvalidIndex);
    }

    // d. Assert: 0 ≤ integer ≤ 2^53 - 1.
    assert!((0.0..=MAX_ARRAY_LIKE_INDEX).contains(&integer));

    // e. Return integer.
    // NOTE: We return the clamped value here, which already has the right type.
    Ok(clamped)
}

/// 7.1.5 ToIntegerOrInfinity ( argument ), https://tc39.es/ecma262/#sec-tointegerorinfinity
pub fn to_integer_or_infinity(number: f64) -> f64 {
    // 1. Let number be ? ToNumber(argument).

    // 2. If number is NaN, +0𝔽, or -0𝔽, return 0.
    if number.is_nan() || number == 0.0 {
        return 0.0;
    }

    // 3. If number is +∞𝔽, return +∞.
    // 4. If number is -∞𝔽, return -∞.
    if number.is_infinite() {
        return number;
    }

    // 5. Let integer be floor(abs(ℝ(number))).
    let mut integer = number.abs().floor();

    // 6. If number < -0𝔽, set integer to -integer.
    // NOTE: The zero check is required as 'integer' is a double here but an MV in the spec,
    //       which doesn't have negative zero.
    if number < 0.0 && integer != 0.0 {
        integer = -integer;
    }

    // 7. Return integer.
    integer
}

fn is_integral_number(number: f64) -> bool {
    number.is_finite() && number.trunc() == number
}

/// 6.1.6.1.3 Number::exponentiate ( base, exponent ), https://tc39.es/ecma262/#sec-numeric-types-number-exponentiate
pub fn exp_double(base: f64, exponent: f64) -> f64 {
    // 1. If exponent is NaN, return NaN.
    if exponent.is_nan() {
        return f64::NAN;
    }

    // 2. If exponent is +0𝔽 or exponent is -0𝔽, return 1𝔽.
    if exponent == 0.0 {
        return 1.0;
    }

    // 3. If base is NaN, return NaN.
    if base.is_nan() {
        return f64::NAN;
    }

    let is_odd_integral_number = is_integral_number(exponent) && exponent % 2.0 != 0.0;

    // 4. If base is +∞𝔽, then
    if base == f64::INFINITY {
        // a. If exponent > +0𝔽, return +∞𝔽. Otherwise, return +0𝔽.
        return if exponent > 0.0 { f64::INFINITY } else { 0.0 };
    }

    // 5. If base is -∞𝔽, then
    if base == f64::NEG_INFINITY {
        // a. If exponent > +0𝔽, then
        if exponent > 0.0 {
            // i. If exponent is an odd integral Number, return -∞𝔽. Otherwise, return +∞𝔽.
            return if is_odd_integral_number {
                f64::NEG_INFINITY
            } else {
                f64::INFINITY
            };
        }
        // b. Else,
        // i. If exponent is an odd integral Number, return -0𝔽. Otherwise, return +0𝔽.
        return if is_odd_integral_number { -0.0 } else { 0.0 };
    }

    // 6. If base is +0𝔽, then
    if base == 0.0 && base.is_sign_positive() {
        // a. If exponent > +0𝔽, return +0𝔽. Otherwise, return +∞𝔽.
        return if exponent > 0.0 { 0.0 } else { f64::INFINITY };
    }

    // 7. If base is -0𝔽, then
    if base == 0.0 {
        // a. If exponent > +0𝔽, then
        if exponent > 0.0 {
            // i. If exponent is an odd integral Number, return -0𝔽. Otherwise, return +0𝔽.
            return if is_odd_integral_number { -0.0 } else { 0.0 };
        }
        // b. Else,
        // i. If exponent is an odd integral Number, return -∞𝔽. Otherwise, return +∞𝔽.
        return if is_odd_integral_number {
            f64::NEG_INFINITY
        } else {
            f64::INFINITY
        };
    }

    // 8. Assert: base is finite and is neither +0𝔽 nor -0𝔽.
    assert!(base.is_finite() && base != 0.0);

    let absolute_base = base.abs();

    // 9. If exponent is +∞𝔽, then
    if exponent == f64::INFINITY {
        // a. If abs(ℝ(base)) > 1, return +∞𝔽.
        // b. If abs(ℝ(base)) is 1, return NaN.
        // c. If abs(ℝ(base)) < 1, return +0𝔽.
        return match absolute_base.partial_cmp(&1.0) {
            Some(Ordering::Greater) => f64::INFINITY,
            Some(Ordering::Equal) => f64::NAN,
            _ => 0.0,
        };
    }

    // 10. If exponent is -∞𝔽, then
    if exponent == f64::NEG_INFINITY {
        // a. If abs(ℝ(base)) > 1, return +0𝔽.
        // b. If abs(ℝ(base)) is 1, return NaN.
        // c. If abs(ℝ(base)) < 1, return +∞𝔽.
        return match absolute_base.partial_cmp(&1.0) {
            Some(Ordering::Greater) => 0.0,
            Some(Ordering::Equal) => f64::NAN,
            _ => f64::INFINITY,
        };
    }

    // 11. Assert: exponent is finite and is neither +0𝔽 nor -0𝔽.
    assert!(exponent.is_finite() && exponent != 0.0);

    // 12. If base < -0𝔽 and exponent is not an integral Number, return NaN.
    if base < 0.0 && !is_integral_number(exponent) {
        return f64::NAN;
    }

    // 13. Return an implementation-approximated Number value representing the result of raising ℝ(base) to the ℝ(exponent) power.
    base.powf(exponent)
}
