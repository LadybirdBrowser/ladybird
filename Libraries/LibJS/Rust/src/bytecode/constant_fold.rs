/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What generating code can work out about constants.

use num_bigint::BigInt;
use num_traits::FromPrimitive;
use num_traits::One;
use num_traits::Signed;
use num_traits::ToPrimitive;
use num_traits::Zero;

use libjs_abi::value as nan_box;

use crate::ast::BinaryOp;
use crate::ast::UnaryOp;
use crate::lexer::ch;
use crate::utf16::Utf16StringBuilder;
use crate::utf16::Utf16View;
use ak::Utf16String;

use super::executable_data::ConstantValue;

/// The constants of a generator, as folding sees them.
pub(super) trait ConstantPool {
    type Operand;

    fn get_constant(&self, operand: &Self::Operand) -> Option<&ConstantValue>;
    fn add_constant_number(&mut self, value: f64) -> Self::Operand;
    fn add_constant_boolean(&mut self, value: bool) -> Self::Operand;
    fn add_constant_string(&mut self, value: Utf16String) -> Self::Operand;
    fn add_constant_bigint(&mut self, value: String) -> Self::Operand;
}

/// Convert a constant value to a boolean, matching JS `ToBoolean`.
/// Returns `None` for VM-specific constants whose truthiness cannot be
/// determined until the Executable is materialized.
pub fn constant_to_boolean(value: &ConstantValue) -> Option<bool> {
    match value {
        ConstantValue::Boolean(b) => Some(*b),
        ConstantValue::Null | ConstantValue::Undefined | ConstantValue::Empty => Some(false),
        ConstantValue::Number(n) => Some(*n != 0.0 && !n.is_nan()),
        ConstantValue::String(s) => Some(!s.is_empty()),
        ConstantValue::BigInt(s) => parse_bigint(s).map(|bi| bi != num_bigint::BigInt::ZERO),
        ConstantValue::WellKnownSymbol(_) | ConstantValue::AbstractOperation(_) => None,
    }
}

/// Parse a BigInt string to an arbitrary-precision BigInt.
/// Handles decimal, 0b binary, 0o octal, and 0x hex prefixes.
pub fn parse_bigint(s: &str) -> Option<num_bigint::BigInt> {
    use num_bigint::BigInt;
    if s.len() > 2 {
        let (prefix, rest) = s.split_at(2);
        match prefix {
            "0b" | "0B" => return BigInt::parse_bytes(rest.as_bytes(), 2),
            "0o" | "0O" => return BigInt::parse_bytes(rest.as_bytes(), 8),
            "0x" | "0X" => return BigInt::parse_bytes(rest.as_bytes(), 16),
            _ => {}
        }
    }
    s.parse::<BigInt>().ok()
}

/// Check if a UTF-16 string is a canonical array index (non-negative integer < 2^32 - 1).
/// These strings become integer PropertyKeys,
/// not string PropertyKeys, so they must NOT be optimized to GetById/PutById.
pub(crate) fn is_array_index(s: &[u16]) -> bool {
    if s.is_empty() || s.len() > 10 {
        return false;
    }
    // Must not have leading zeros (except "0" itself)
    if s.len() > 1 && s[0] == ch(b'0') {
        return false;
    }
    let mut value: u64 = 0;
    for &c in s {
        if c < ch(b'0') || c > ch(b'9') {
            return false;
        }
        value = value * 10 + (c - ch(b'0')) as u64;
    }
    value <= 0xFFFF_FFFE
}

/// JS ToInt32 conversion (ECMA-262 7.1.6).
pub(super) fn to_int32(n: f64) -> i32 {
    if n.is_nan() || n.is_infinite() || n == 0.0 {
        return 0;
    }
    let int_val = n.signum() * n.abs().floor();
    let int32bit = int_val % 4294967296.0; // 2^32
    let int32bit = if int32bit < 0.0 {
        int32bit + 4294967296.0
    } else {
        int32bit
    };
    if int32bit >= 2147483648.0 {
        (int32bit - 4294967296.0) as i32
    } else {
        int32bit as i32
    }
}

/// JS ToUint32 conversion (ECMA-262 7.1.7).
pub(super) fn to_u32(n: f64) -> u32 {
    if n.is_nan() || n.is_infinite() || n == 0.0 {
        return 0;
    }
    let int_val = n.signum() * n.abs().floor();
    let int32bit = int_val % 4294967296.0; // 2^32
    if int32bit < 0.0 {
        (int32bit + 4294967296.0) as u32
    } else {
        int32bit as u32
    }
}

// =============================================================================
// Constant folding
// =============================================================================

/// Try to constant-fold a unary operation when the operand is a constant.
pub(super) fn try_constant_fold_unary<G: ConstantPool>(
    generator: &mut G,
    op: UnaryOp,
    operand: &G::Operand,
) -> Option<G::Operand> {
    let constant = generator.get_constant(operand)?;
    match op {
        UnaryOp::Minus => {
            let n = constant_to_number(constant)?;
            Some(generator.add_constant_number(-n))
        }
        UnaryOp::Plus => {
            let n = constant_to_number(constant)?;
            Some(generator.add_constant_number(n))
        }
        UnaryOp::BitwiseNot => {
            if let ConstantValue::BigInt(s) = constant {
                let n = parse_bigint(&s.clone())?;
                return Some(generator.add_constant_bigint((-(n + BigInt::one())).to_string()));
            }
            let n = constant_to_number(constant)?;
            Some(generator.add_constant_number((!to_int32(n)) as f64))
        }
        UnaryOp::Not => {
            let as_bool = constant_to_boolean(constant)?;
            Some(generator.add_constant_boolean(!as_bool))
        }
        _ => None,
    }
}

/// Implement IsLooselyEqual for constant values.
/// https://tc39.es/ecma262/#sec-islooselyequal
fn try_constant_loosely_equals(lhs: &ConstantValue, rhs: &ConstantValue) -> Option<bool> {
    // Same type: use strict equality rules.
    match (lhs, rhs) {
        (ConstantValue::Null | ConstantValue::Undefined, ConstantValue::Null | ConstantValue::Undefined) => {
            return Some(true);
        }
        (ConstantValue::Null | ConstantValue::Undefined, _) | (_, ConstantValue::Null | ConstantValue::Undefined) => {
            return Some(false);
        }
        (ConstantValue::Number(a), ConstantValue::Number(b)) => return Some(a == b),
        (ConstantValue::String(a), ConstantValue::String(b)) => return Some(a == b),
        (ConstantValue::Boolean(a), ConstantValue::Boolean(b)) => return Some(a == b),
        (ConstantValue::BigInt(a), ConstantValue::BigInt(b)) => return Some(a == b),
        _ => {}
    }
    // Cross-type comparisons: Boolean → Number first, then retry.
    match (lhs, rhs) {
        (ConstantValue::Boolean(b), _) => {
            let coerced = ConstantValue::Number(if *b { 1.0 } else { 0.0 });
            return try_constant_loosely_equals(&coerced, rhs);
        }
        (_, ConstantValue::Boolean(b)) => {
            let coerced = ConstantValue::Number(if *b { 1.0 } else { 0.0 });
            return try_constant_loosely_equals(lhs, &coerced);
        }
        _ => {}
    }
    // Number == String → compare ToNumber(string) to number.
    // String == Number → compare number to ToNumber(string).
    match (lhs, rhs) {
        (ConstantValue::Number(n), ConstantValue::String(s)) | (ConstantValue::String(s), ConstantValue::Number(n)) => {
            Some(*n == crate::host::string_to_number(&s.to_utf16()))
        }
        // BigInt == Number or Number == BigInt: compare mathematical values.
        (ConstantValue::BigInt(b), ConstantValue::Number(n)) | (ConstantValue::Number(n), ConstantValue::BigInt(b)) => {
            if n.is_nan() || n.is_infinite() {
                return Some(false);
            }
            if n.fract() != 0.0 {
                return Some(false);
            }
            let bi = parse_bigint(b)?;
            Some(BigInt::from_f64(*n)? == bi)
        }
        // BigInt == String or String == BigInt: parse string as BigInt per StringToBigInt.
        // If the string cannot be parsed, the result is false (not equal).
        (ConstantValue::BigInt(b), ConstantValue::String(s)) | (ConstantValue::String(s), ConstantValue::BigInt(b)) => {
            match crate::host::string_to_bigint(&s.to_utf16()) {
                Some(s_bi) => {
                    let bi = parse_bigint(b)?;
                    Some(bi == s_bi)
                }
                None => Some(false),
            }
        }
        _ => None,
    }
}

pub(crate) fn ascii_code_units_to_string(text: Utf16View<'_>) -> Option<String> {
    text.code_units()
        .map(|code_unit| {
            u8::try_from(code_unit)
                .ok()
                .and_then(|byte| byte.is_ascii().then_some(char::from(byte)))
        })
        .collect()
}

/// Constant-fold a binary operation on two BigInt operands.
fn try_constant_fold_bigint_binary<G: ConstantPool>(
    generator: &mut G,
    op: BinaryOp,
    a_str: &str,
    b_str: &str,
) -> Option<G::Operand> {
    let a = parse_bigint(a_str)?;
    let b = parse_bigint(b_str)?;
    match op {
        // Arithmetic operations: produce a BigInt result.
        BinaryOp::Addition => Some(generator.add_constant_bigint((&a + &b).to_string())),
        BinaryOp::Subtraction => Some(generator.add_constant_bigint((&a - &b).to_string())),
        BinaryOp::Multiplication => Some(generator.add_constant_bigint((&a * &b).to_string())),
        BinaryOp::Division => {
            if b.is_zero() {
                return None; // Division by zero throws at runtime.
            }
            use num_integer::Integer;
            let (quotient, _) = a.div_rem(&b);
            Some(generator.add_constant_bigint(quotient.to_string()))
        }
        BinaryOp::Modulo => {
            if b.is_zero() {
                return None; // Modulo by zero throws at runtime.
            }
            // JS BigInt remainder has the sign of the dividend (truncated division).
            Some(generator.add_constant_bigint((&a % &b).to_string()))
        }
        BinaryOp::Exponentiation => {
            if b.is_negative() {
                return None; // Negative exponent throws at runtime.
            }
            let exp = b.to_u32()?;
            // Only fold small exponents to avoid huge results.
            if exp > 1000 {
                return None;
            }
            Some(generator.add_constant_bigint(num_traits::pow::pow(a, exp as usize).to_string()))
        }
        // Comparison operations: produce a Boolean result.
        BinaryOp::StrictlyEquals | BinaryOp::LooselyEquals => Some(generator.add_constant_boolean(a == b)),
        BinaryOp::StrictlyInequals | BinaryOp::LooselyInequals => Some(generator.add_constant_boolean(a != b)),
        BinaryOp::LessThan => Some(generator.add_constant_boolean(a < b)),
        BinaryOp::LessThanEquals => Some(generator.add_constant_boolean(a <= b)),
        BinaryOp::GreaterThan => Some(generator.add_constant_boolean(a > b)),
        BinaryOp::GreaterThanEquals => Some(generator.add_constant_boolean(a >= b)),
        // Bitwise operations on BigInt.
        BinaryOp::BitwiseAnd => Some(generator.add_constant_bigint((&a & &b).to_string())),
        BinaryOp::BitwiseOr => Some(generator.add_constant_bigint((&a | &b).to_string())),
        BinaryOp::BitwiseXor => Some(generator.add_constant_bigint((&a ^ &b).to_string())),
        BinaryOp::LeftShift => {
            let shift = b.to_u64()?;
            if shift > 512 {
                return None;
            }
            Some(generator.add_constant_bigint((&a << shift as usize).to_string()))
        }
        BinaryOp::RightShift => {
            let shift = b.to_u64()?;
            // BigInt right shift for negative numbers floors toward negative infinity.
            Some(generator.add_constant_bigint((&a >> shift).to_string()))
        }
        // UnsignedRightShift throws TypeError for BigInt.
        _ => None,
    }
}

/// Convert a constant value to a JS number for constant folding purposes.
fn constant_to_number(val: &ConstantValue) -> Option<f64> {
    match val {
        ConstantValue::Number(n) => Some(*n),
        ConstantValue::Boolean(b) => Some(if *b { 1.0 } else { 0.0 }),
        ConstantValue::Null => Some(0.0),
        ConstantValue::Undefined => Some(f64::NAN),
        ConstantValue::String(s) => Some(crate::host::string_to_number(&s.to_utf16())),
        _ => None,
    }
}

/// Convert a constant value to a JS string (ToString) for string concatenation.
/// Returns None for types where ToString requires runtime (e.g. objects).
pub fn constant_to_string(val: &ConstantValue) -> Option<Utf16String> {
    match val {
        ConstantValue::String(s) => Some(s.clone()),
        ConstantValue::Number(n) => Some(crate::host::js_number_to_utf16(*n)),
        ConstantValue::Boolean(b) => Some(if *b {
            Utf16String::from_utf16(utf16!("true"))
        } else {
            Utf16String::from_utf16(utf16!("false"))
        }),
        ConstantValue::Null => Some(Utf16String::from_utf16(utf16!("null"))),
        ConstantValue::Undefined => Some(Utf16String::from_utf16(utf16!("undefined"))),
        ConstantValue::BigInt(s) => {
            // BigInt.prototype.toString() returns the decimal string representation.
            Some(Utf16String::from_utf8(&parse_bigint(s)?.to_string()))
        }
        _ => None,
    }
}

/// Compare a BigInt and a Number using Abstract Relational Comparison semantics.
/// Returns None if comparison cannot be folded (non-safe integer, etc).
/// Returns Some(None) for NaN (undefined result - all comparisons return false).
/// Returns Some(Some(Ordering)) for the BigInt relative to the Number.
fn compare_bigint_and_number(bigint_str: &str, number: f64) -> Option<Option<std::cmp::Ordering>> {
    use std::cmp::Ordering;
    if number.is_nan() {
        return Some(None); // Comparisons with NaN return undefined → false.
    }
    if number == f64::INFINITY {
        return Some(Some(Ordering::Less)); // Any BigInt < +Infinity
    }
    if number == f64::NEG_INFINITY {
        return Some(Some(Ordering::Greater)); // Any BigInt > -Infinity
    }
    let bigint = parse_bigint(bigint_str)?;
    if number.fract() != 0.0 {
        // The number has a fractional part, so we can still compare:
        // floor(number) < bigint means bigint > number, etc.
        let floored = number.floor();
        let floored_bigint = BigInt::from_f64(floored)?;
        if bigint <= floored_bigint {
            // bigint <= floor(number) means bigint < number
            return Some(Some(Ordering::Less));
        } else {
            // bigint > floor(number) means bigint > number (since number = floor + fract where fract > 0)
            return Some(Some(Ordering::Greater));
        }
    }
    let number_bigint = BigInt::from_f64(number)?;
    Some(Some(bigint.cmp(&number_bigint)))
}

/// Try to constant-fold a binary operation when both operands are constants.
pub(super) fn try_constant_fold_binary<G: ConstantPool>(
    generator: &mut G,
    op: BinaryOp,
    lhs: &G::Operand,
    rhs: &G::Operand,
) -> Option<G::Operand> {
    let lhs_const = generator.get_constant(lhs)?;
    let rhs_const = generator.get_constant(rhs)?;

    // BigInt constant folding: if both operands are BigInt, handle separately.
    // Clone strings to release the immutable borrow on gen before the mutable call.
    if let (ConstantValue::BigInt(a), ConstantValue::BigInt(b)) = (lhs_const, rhs_const) {
        let a = a.clone();
        let b = b.clone();
        return try_constant_fold_bigint_binary(generator, op, &a, &b);
    }

    match op {
        BinaryOp::Addition => {
            // If either operand is a string, do string concatenation using ToString.
            if matches!(lhs_const, ConstantValue::String(_)) || matches!(rhs_const, ConstantValue::String(_)) {
                let a = constant_to_string(lhs_const)?;
                let b = constant_to_string(rhs_const)?;
                let mut result = Utf16StringBuilder::new();
                result.append(Utf16View::of_string(&a));
                result.append(Utf16View::of_string(&b));
                return Some(generator.add_constant_string(result.to_utf16_string()));
            }
            // Numeric addition: both operands coerced to number
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number(a + b))
        }
        BinaryOp::Subtraction => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number(a - b))
        }
        BinaryOp::Multiplication => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number(a * b))
        }
        BinaryOp::Division => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number(a / b))
        }
        BinaryOp::Modulo => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number(a % b))
        }
        BinaryOp::Exponentiation => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number(crate::host::exponentiate(a, b)))
        }
        BinaryOp::StrictlyEquals | BinaryOp::StrictlyInequals => {
            let equal = match (lhs_const, rhs_const) {
                (ConstantValue::Number(a), ConstantValue::Number(b)) => a == b,
                (ConstantValue::String(a), ConstantValue::String(b)) => a == b,
                (ConstantValue::Boolean(a), ConstantValue::Boolean(b)) => a == b,
                (ConstantValue::Null, ConstantValue::Null) => true,
                (ConstantValue::Undefined, ConstantValue::Undefined) => true,
                (ConstantValue::WellKnownSymbol(a), ConstantValue::WellKnownSymbol(b)) => a == b,
                // Different types are never strictly equal.
                _ => false,
            };
            let result = if op == BinaryOp::StrictlyInequals {
                !equal
            } else {
                equal
            };
            Some(generator.add_constant_boolean(result))
        }
        BinaryOp::GreaterThan | BinaryOp::GreaterThanEquals | BinaryOp::LessThan | BinaryOp::LessThanEquals => {
            // String-string comparison is lexicographic (by UTF-16 code units).
            if let (ConstantValue::String(a), ConstantValue::String(b)) = (lhs_const, rhs_const) {
                let result = match op {
                    BinaryOp::GreaterThan => a.to_utf16() > b.to_utf16(),
                    BinaryOp::GreaterThanEquals => a.to_utf16() >= b.to_utf16(),
                    BinaryOp::LessThan => a.to_utf16() < b.to_utf16(),
                    BinaryOp::LessThanEquals => a.to_utf16() <= b.to_utf16(),
                    _ => unreachable!("outer match arm only matches comparison operators"),
                };
                return Some(generator.add_constant_boolean(result));
            }
            // BigInt-Number/Boolean cross-type comparison.
            // Per spec, Booleans are converted to Number first.
            let lhs_for_cmp = match lhs_const {
                ConstantValue::Boolean(b) => &ConstantValue::Number(if *b { 1.0 } else { 0.0 }),
                other => other,
            };
            let rhs_for_cmp = match rhs_const {
                ConstantValue::Boolean(b) => &ConstantValue::Number(if *b { 1.0 } else { 0.0 }),
                other => other,
            };
            // BigInt vs Number comparison using Abstract Relational Comparison.
            let bigint_ord = match (lhs_for_cmp, rhs_for_cmp) {
                (ConstantValue::BigInt(b), ConstantValue::Number(n)) => compare_bigint_and_number(b, *n),
                (ConstantValue::Number(n), ConstantValue::BigInt(b)) => {
                    compare_bigint_and_number(b, *n).map(|o| o.map(|o| o.reverse()))
                }
                // BigInt vs String: per spec, parse the string as a BigInt
                // using StringToBigInt, then compare the two BigInts.
                // If the string is not a valid StringIntegerLiteral,
                // the result is undefined (= false for all comparisons).
                (ConstantValue::BigInt(b), ConstantValue::String(s)) => {
                    match crate::host::string_to_bigint(&s.to_utf16()) {
                        Some(rhs_bi) => {
                            let lhs_bi = parse_bigint(b)?;
                            Some(Some(lhs_bi.cmp(&rhs_bi)))
                        }
                        None => Some(None),
                    }
                }
                (ConstantValue::String(s), ConstantValue::BigInt(b)) => {
                    match crate::host::string_to_bigint(&s.to_utf16()) {
                        Some(lhs_bi) => {
                            let rhs_bi = parse_bigint(b)?;
                            Some(Some(lhs_bi.cmp(&rhs_bi)))
                        }
                        None => Some(None),
                    }
                }
                _ => None,
            };
            if let Some(ord) = bigint_ord {
                let result = match op {
                    BinaryOp::GreaterThan => matches!(ord, Some(std::cmp::Ordering::Greater)),
                    BinaryOp::GreaterThanEquals => {
                        matches!(ord, Some(std::cmp::Ordering::Greater | std::cmp::Ordering::Equal))
                    }
                    BinaryOp::LessThan => matches!(ord, Some(std::cmp::Ordering::Less)),
                    BinaryOp::LessThanEquals => {
                        matches!(ord, Some(std::cmp::Ordering::Less | std::cmp::Ordering::Equal))
                    }
                    _ => unreachable!("outer match arm only matches comparison operators"),
                };
                return Some(generator.add_constant_boolean(result));
            }
            let a = constant_to_number(lhs_for_cmp)?;
            let b = constant_to_number(rhs_for_cmp)?;
            let result = match op {
                BinaryOp::GreaterThan => a > b,
                BinaryOp::GreaterThanEquals => a >= b,
                BinaryOp::LessThan => a < b,
                BinaryOp::LessThanEquals => a <= b,
                _ => unreachable!("outer match arm only matches comparison operators"),
            };
            Some(generator.add_constant_boolean(result))
        }
        BinaryOp::LooselyEquals => {
            let result = try_constant_loosely_equals(lhs_const, rhs_const)?;
            Some(generator.add_constant_boolean(result))
        }
        BinaryOp::LooselyInequals => {
            let result = try_constant_loosely_equals(lhs_const, rhs_const)?;
            Some(generator.add_constant_boolean(!result))
        }
        BinaryOp::BitwiseAnd => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number((to_int32(a) & to_int32(b)) as f64))
        }
        BinaryOp::BitwiseOr => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number((to_int32(a) | to_int32(b)) as f64))
        }
        BinaryOp::BitwiseXor => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number((to_int32(a) ^ to_int32(b)) as f64))
        }
        BinaryOp::LeftShift => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number((to_int32(a) << (to_u32(b) & 0x1f)) as f64))
        }
        BinaryOp::RightShift => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number((to_int32(a) >> (to_u32(b) & 0x1f)) as f64))
        }
        BinaryOp::UnsignedRightShift => {
            let a = constant_to_number(lhs_const)?;
            let b = constant_to_number(rhs_const)?;
            Some(generator.add_constant_number((to_u32(a) >> (to_u32(b) & 0x1f)) as f64))
        }
        _ => None,
    }
}

// =============================================================================
// NaN-boxing helpers
// =============================================================================

// NanBoxed Value encoding helpers (ABI-compatible with GC::NanBoxedValue).
// Used by NewPrimitiveArray to encode constant primitive values inline.

pub(super) fn nanboxed_number(value: f64) -> u64 {
    let is_negative_zero = value.to_bits() == nan_box::NEGATIVE_ZERO_BITS;
    if value >= i32::MIN as f64 && value <= i32::MAX as f64 && value.trunc() == value && !is_negative_zero {
        nan_box::SHIFTED_INT32_TAG | ((value as i32 as u32) as u64)
    } else if value.is_nan() {
        nan_box::CANON_NAN_BITS
    } else {
        value.to_bits()
    }
}

pub(super) fn nanboxed_boolean(value: bool) -> u64 {
    nan_box::SHIFTED_BOOLEAN_TAG | u64::from(value)
}

pub(super) fn nanboxed_null() -> u64 {
    nan_box::NULL_VALUE
}

pub(super) fn nanboxed_empty() -> u64 {
    nan_box::EMPTY_VALUE
}
