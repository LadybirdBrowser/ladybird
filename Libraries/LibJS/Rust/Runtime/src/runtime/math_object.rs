/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The implementations of the Math functions the interpreter calls directly when a call site names one of them, from
//! Libraries/LibJS/Runtime/MathObject.cpp. The Math object itself comes with the builtins.

use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::value;

fn js_nan() -> Value {
    Value::from_f64(f64::NAN)
}

fn js_infinity() -> Value {
    Value::from_f64(f64::INFINITY)
}

fn js_negative_infinity() -> Value {
    Value::from_f64(f64::NEG_INFINITY)
}

// 21.3.2.1 Math.abs ( x ), https://tc39.es/ecma262/#sec-math.abs
pub fn abs_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // OPTIMIZATION: Fast path for Int32 values.
    if x.is_int32() {
        let x_int32 = x.as_i32();
        if x_int32 != i32::MIN {
            return Ok(Value::from_i32(x_int32.abs()));
        }
        return Ok(Value::from_f64(f64::from(i32::MAX as u32 + 1)));
    }

    // Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is NaN, return NaN.
    if number.is_nan() {
        return Ok(js_nan());
    }

    // 3. If n is -0𝔽, return +0𝔽.
    if number.is_negative_zero() {
        return Ok(Value::from_i32(0));
    }

    // 4. If n is -∞𝔽, return +∞𝔽.
    if number.is_negative_infinity() {
        return Ok(js_infinity());
    }

    // 5. If n < -0𝔽, return -n.
    // 6. Return n.
    let number = number.as_f64();
    Ok(Value::from_f64(if number < 0.0 { -number } else { number }))
}

// 21.3.2.10 Math.ceil ( x ), https://tc39.es/ecma262/#sec-math.ceil
pub fn ceil_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if !number.is_finite_number() || number.as_f64() == 0.0 {
        return Ok(number);
    }

    // 3. If n < -0𝔽 and n > -1𝔽, return -0𝔽.
    if number.as_f64() < 0.0 && number.as_f64() > -1.0 {
        return Ok(Value::from_f64(-0.0));
    }

    // 4. If n is an integral Number, return n.
    // 5. Return the smallest (closest to -∞) integral Number value that is not less than n.
    Ok(Value::from_f64(number.as_f64().ceil()))
}

// 21.3.2.12 Math.cos ( x ), https://tc39.es/ecma262/#sec-math.cos
pub fn cos_impl(vm: &Vm, value: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = value.to_number(vm)?;

    // 2. If n is NaN, n is +∞𝔽, or n is -∞𝔽, return NaN.
    if number.is_nan() || number.is_infinity() {
        return Ok(js_nan());
    }

    // 3. If n is +0𝔽 or n is -0𝔽, return 1𝔽.
    if number.is_positive_zero() || number.is_negative_zero() {
        return Ok(Value::from_i32(1));
    }

    // 4. Return an implementation-approximated Number value representing the result of the cosine of ℝ(n).
    Ok(Value::from_f64(number.as_f64().cos()))
}

// 21.3.2.14 Math.exp ( x ), https://tc39.es/ecma262/#sec-math.exp
pub fn exp_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is either NaN or +∞𝔽, return n.
    if number.is_nan() || number.is_positive_infinity() {
        return Ok(number);
    }

    // 3. If n is either +0𝔽 or -0𝔽, return 1𝔽.
    if number.as_f64() == 0.0 {
        return Ok(Value::from_i32(1));
    }

    // 4. If n is -∞𝔽, return +0𝔽.
    if number.is_negative_infinity() {
        return Ok(Value::from_i32(0));
    }

    // 5. Return an implementation-approximated Number value representing the result of the exponential function of ℝ(n).
    Ok(Value::from_f64(number.as_f64().exp()))
}

// 21.3.2.16 Math.floor ( x ), https://tc39.es/ecma262/#sec-math.floor
pub fn floor_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if !number.is_finite_number() || number.as_f64() == 0.0 {
        return Ok(number);
    }

    // 3. If n < 1𝔽 and n > +0𝔽, return +0𝔽.
    // 4. If n is an integral Number, return n.
    // 5. Return the greatest (closest to +∞) integral Number value that is not greater than n.
    Ok(Value::from_f64(number.as_f64().floor()))
}

// 21.3.2.20 Math.imul ( x, y ), https://tc39.es/ecma262/#sec-math.imul
pub fn imul_impl(vm: &Vm, arg_a: Value, arg_b: Value) -> ThrowCompletionOr<Value> {
    // 1. Let a be ℝ(? ToUint32(x)).
    let a = arg_a.to_u32(vm)?;

    // 2. Let b be ℝ(? ToUint32(y)).
    let b = arg_b.to_u32(vm)?;

    // 3. Let product be (a × b) modulo 2^32.
    // 4. If product ≥ 2^31, return 𝔽(product - 2^32); otherwise return 𝔽(product).
    Ok(Value::from_i32(a.wrapping_mul(b) as i32))
}

// 21.3.2.21 Math.log ( x ), https://tc39.es/ecma262/#sec-math.log
pub fn log_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is NaN or n is +∞𝔽, return n.
    if number.is_nan() || number.is_positive_infinity() {
        return Ok(number);
    }

    // 3. If n is 1𝔽, return +0𝔽.
    if number.as_f64() == 1.0 {
        return Ok(Value::from_i32(0));
    }

    // 4. If n is +0𝔽 or n is -0𝔽, return -∞𝔽.
    if number.is_positive_zero() || number.is_negative_zero() {
        return Ok(js_negative_infinity());
    }

    // 5. If n < -0𝔽, return NaN.
    if number.as_f64() < -0.0 {
        return Ok(js_nan());
    }

    // 6. Return an implementation-approximated Number value representing the result of the natural logarithm of ℝ(n).
    Ok(Value::from_f64(number.as_f64().ln()))
}

// 21.3.2.27 Math.pow ( base, exponent ), https://tc39.es/ecma262/#sec-math.pow
pub fn pow_impl(vm: &Vm, base: Value, exponent: Value) -> ThrowCompletionOr<Value> {
    // Set base to ? ToNumber(base).
    let base = base.to_number(vm)?;

    // 2. Set exponent to ? ToNumber(exponent).
    let exponent = exponent.to_number(vm)?;

    // 3. Return Number::exponentiate(base, exponent).
    value::exp(vm, base, exponent)
}

// 21.3.2.28 Math.random ( ), https://tc39.es/ecma262/#sec-math.random
pub fn random_impl() -> Value {
    // This function returns a Number value with positive sign, greater than or equal to +0𝔽 but strictly less than 1𝔽,
    // chosen randomly or pseudo randomly with approximately uniform distribution over that range, using an
    // implementation-defined algorithm or strategy.
    unimplemented_runtime_function("MathObject::random_impl, which draws from AK's XorShift128PlusRNG", 0)
}

// 21.3.2.29 Math.round ( x ), https://tc39.es/ecma262/#sec-math.round
pub fn round_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is not finite or n is an integral Number, return n.
    if !number.is_finite_number() || number.as_f64() == number.as_f64().trunc() {
        return Ok(number);
    }

    // 3. If n < 0.5𝔽 and n > +0𝔽, return +0𝔽.
    // 4. If n < -0𝔽 and n ≥ -0.5𝔽, return -0𝔽.
    // 5. Return the integral Number closest to n, preferring the Number closer to +∞ in the case of a tie.
    let mut integer = number.as_f64().ceil();
    if integer - 0.5 > number.as_f64() {
        integer -= 1.0;
    }
    Ok(Value::from_f64(integer))
}

// 21.3.2.31 Math.sin ( x ), https://tc39.es/ecma262/#sec-math.sin
pub fn sin_impl(vm: &Vm, value: Value) -> ThrowCompletionOr<Value> {
    // 1. Let n be ? ToNumber(x).
    let number = value.to_number(vm)?;

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if number.is_nan() || number.is_positive_zero() || number.is_negative_zero() {
        return Ok(number);
    }

    // 3. If n is +∞𝔽 or n is -∞𝔽, return NaN.
    if number.is_infinity() {
        return Ok(js_nan());
    }

    // 4. Return an implementation-approximated Number value representing the result of the sine of ℝ(n).
    Ok(Value::from_f64(number.as_f64().sin()))
}

// 21.3.2.33 Math.sqrt ( x ), https://tc39.es/ecma262/#sec-math.sqrt
pub fn sqrt_impl(vm: &Vm, x: Value) -> ThrowCompletionOr<Value> {
    // Let n be ? ToNumber(x).
    let number = x.to_number(vm)?;

    // 2. If n is one of NaN, +0𝔽, -0𝔽, or +∞𝔽, return n.
    if number.is_nan() || number.as_f64() == 0.0 || number.is_positive_infinity() {
        return Ok(number);
    }

    // 3. If n < -0𝔽, return NaN.
    if number.as_f64() < 0.0 {
        return Ok(js_nan());
    }

    // 4. Return an implementation-approximated Number value representing the result of the square root of ℝ(n).
    Ok(Value::from_f64(number.as_f64().sqrt()))
}

// 21.3.2.35 Math.tan ( x ), https://tc39.es/ecma262/#sec-math.tan
pub fn tan_impl(vm: &Vm, value: Value) -> ThrowCompletionOr<Value> {
    // Let n be ? ToNumber(x).
    let number = value.to_number(vm)?;

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if number.is_nan() || number.is_positive_zero() || number.is_negative_zero() {
        return Ok(number);
    }

    // 3. If n is +∞𝔽, or n is -∞𝔽, return NaN.
    if number.is_infinity() {
        return Ok(js_nan());
    }

    // 4. Return an implementation-approximated Number value representing the result of the tangent of ℝ(n).
    Ok(Value::from_f64(number.as_f64().tan()))
}
