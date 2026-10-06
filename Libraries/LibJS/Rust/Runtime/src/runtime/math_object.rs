/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::{LazyLock, Mutex};

use libjs_abi::Builtin;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::random::XorShift128PlusRNG;
use crate::runtime::abstract_operations::require_object_coercible;
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::iterator::{IteratorHint, get_iterator, iterator_close, iterator_step_value};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value;

// NB: Rust's acosh, asinh and atanh are computed in Rust rather than by the C library, whose results these return
//     instead. Every other function here is one Rust also takes from the C library.
// SAFETY: These are the C library's math functions, which take and return a double and have no other effects.
unsafe extern "C" {
    #[link_name = "acosh"]
    safe fn libm_acosh(x: f64) -> f64;
    #[link_name = "asinh"]
    safe fn libm_asinh(x: f64) -> f64;
    #[link_name = "atanh"]
    safe fn libm_atanh(x: f64) -> f64;
}

const M_PI_4: f64 = core::f64::consts::FRAC_PI_4;
const M_PI_2: f64 = core::f64::consts::FRAC_PI_2;
const M_PI: f64 = core::f64::consts::PI;

#[repr(C)]
#[derive(Trace)]
pub struct MathObject {
    base: Object,
}

define_object_class!(MathObject, extends: [Object], methods: {
    initialize: MathObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn js_nan() -> Value {
    Value::from_f64(f64::NAN)
}

fn js_infinity() -> Value {
    Value::from_f64(f64::INFINITY)
}

fn js_negative_infinity() -> Value {
    Value::from_f64(f64::NEG_INFINITY)
}

/// The List of arguments that Math.hypot, Math.max and Math.min coerce with ToNumber, in order. The first eight stay
/// inline, as in C++'s Vector<Value, 8>, so that the common calls do not allocate.
struct CoercedArguments {
    inline: [f64; 8],
    spilled: Vec<f64>,
    length: usize,
}

impl CoercedArguments {
    fn of(vm: &Vm) -> ThrowCompletionOr<Self> {
        let length = vm.argument_count();
        let mut coerced = Self {
            inline: [0.0; 8],
            spilled: Vec::new(),
            length,
        };
        let fits_inline = length <= coerced.inline.len();
        if !fits_inline {
            coerced.spilled.reserve_exact(length);
        }
        for index in 0..length {
            let number = vm.argument(index).to_number(vm)?.as_f64();
            if fits_inline {
                coerced.inline[index] = number;
            } else {
                coerced.spilled.push(number);
            }
        }
        Ok(coerced)
    }
}

impl core::ops::Deref for CoercedArguments {
    type Target = [f64];

    fn deref(&self) -> &[f64] {
        if self.length <= self.inline.len() {
            &self.inline[..self.length]
        } else {
            &self.spilled
        }
    }
}

impl MathObject {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<MathObject> {
        realm.create_object(
            vm,
            MathObject {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |name: &PropertyKey, function, length, builtin| {
            object.define_native_function(vm, realm, name, function, length, attr, builtin);
        };
        define(&names.abs, raw_native!(MathObject::abs), 1, Some(Builtin::MathAbs));
        define(
            &names.random,
            raw_native!(MathObject::random),
            0,
            Some(Builtin::MathRandom),
        );
        define(&names.sqrt, raw_native!(MathObject::sqrt), 1, Some(Builtin::MathSqrt));
        define(
            &names.floor,
            raw_native!(MathObject::floor),
            1,
            Some(Builtin::MathFloor),
        );
        define(&names.ceil, raw_native!(MathObject::ceil), 1, Some(Builtin::MathCeil));
        define(
            &names.round,
            raw_native!(MathObject::round),
            1,
            Some(Builtin::MathRound),
        );
        define(&names.max, raw_native!(MathObject::max), 2, None);
        define(&names.min, raw_native!(MathObject::min), 2, None);
        define(&names.trunc, raw_native!(MathObject::trunc), 1, None);
        define(&names.sin, raw_native!(MathObject::sin), 1, Some(Builtin::MathSin));
        define(&names.cos, raw_native!(MathObject::cos), 1, Some(Builtin::MathCos));
        define(&names.tan, raw_native!(MathObject::tan), 1, Some(Builtin::MathTan));
        define(&names.pow, raw_native!(MathObject::pow), 2, Some(Builtin::MathPow));
        define(&names.exp, raw_native!(MathObject::exp), 1, Some(Builtin::MathExp));
        define(&names.expm1, raw_native!(MathObject::expm1), 1, None);
        define(&names.sign, raw_native!(MathObject::sign), 1, None);
        define(&names.clz32, raw_native!(MathObject::clz32), 1, None);
        define(&names.acos, raw_native!(MathObject::acos), 1, None);
        define(&names.acosh, raw_native!(MathObject::acosh), 1, None);
        define(&names.asin, raw_native!(MathObject::asin), 1, None);
        define(&names.asinh, raw_native!(MathObject::asinh), 1, None);
        define(&names.atan, raw_native!(MathObject::atan), 1, None);
        define(&names.atanh, raw_native!(MathObject::atanh), 1, None);
        define(&names.log1p, raw_native!(MathObject::log1p), 1, None);
        define(&names.cbrt, raw_native!(MathObject::cbrt), 1, None);
        define(&names.atan2, raw_native!(MathObject::atan2), 2, None);
        define(&names.fround, raw_native!(MathObject::fround), 1, None);
        define(&names.f16round, raw_native!(MathObject::f16round), 1, None);
        define(&names.hypot, raw_native!(MathObject::hypot), 2, None);
        define(&names.imul, raw_native!(MathObject::imul), 2, Some(Builtin::MathImul));
        define(&names.log, raw_native!(MathObject::log), 1, Some(Builtin::MathLog));
        define(&names.log2, raw_native!(MathObject::log2), 1, None);
        define(&names.log10, raw_native!(MathObject::log10), 1, None);
        define(&names.sinh, raw_native!(MathObject::sinh), 1, None);
        define(&names.cosh, raw_native!(MathObject::cosh), 1, None);
        define(&names.tanh, raw_native!(MathObject::tanh), 1, None);
        define(&names.sumPrecise, raw_native!(MathObject::sum_precise), 1, None);

        // 21.3.1 Value Properties of the Math Object, https://tc39.es/ecma262/#sec-value-properties-of-the-math-object
        let constant = |name: &PropertyKey, value: f64| {
            object.define_direct_property(vm, name, Value::from_f64(value), PropertyAttributes::new(0));
        };
        constant(&names.E, core::f64::consts::E);
        constant(&names.LN2, core::f64::consts::LN_2);
        constant(&names.LN10, core::f64::consts::LN_10);
        constant(&names.LOG2E, core::f64::consts::E.log2());
        constant(&names.LOG10E, core::f64::consts::E.log10());
        constant(&names.PI, M_PI);
        constant(&names.SQRT1_2, core::f64::consts::FRAC_1_SQRT_2);
        constant(&names.SQRT2, core::f64::consts::SQRT_2);

        // 21.3.1.9 Math [ @@toStringTag ], https://tc39.es/ecma262/#sec-math-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(vm, names.Math.as_string())),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 21.3.2.1 Math.abs ( x ), https://tc39.es/ecma262/#sec-math.abs
    fn abs(vm: &Vm) -> ThrowCompletionOr<Value> {
        abs_impl(vm, vm.argument(0))
    }

    // 21.3.2.2 Math.acos ( x ), https://tc39.es/ecma262/#sec-math.acos
    fn acos(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, n > 1𝔽, or n < -1𝔽, return NaN.
        if number.is_nan() || number.as_f64() > 1.0 || number.as_f64() < -1.0 {
            return Ok(js_nan());
        }

        // 3. If n is 1𝔽, return +0𝔽.
        if number.as_f64() == 1.0 {
            return Ok(Value::from_i32(0));
        }

        // 4. Return an implementation-approximated Number value representing the result of the inverse cosine of ℝ(n).
        Ok(Value::from_f64(number.as_f64().acos()))
    }

    // 21.3.2.3 Math.acosh ( x ), https://tc39.es/ecma262/#sec-math.acosh
    fn acosh(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN or n is +∞𝔽, return n.
        if number.is_nan() || number.is_positive_infinity() {
            return Ok(number);
        }

        // 3. If n is 1𝔽, return +0𝔽.
        if number.as_f64() == 1.0 {
            return Ok(Value::from_f64(0.0));
        }

        // 4. If n < 1𝔽, return NaN.
        if number.as_f64() < 1.0 {
            return Ok(js_nan());
        }

        // 5. Return an implementation-approximated Number value representing the result of the inverse hyperbolic cosine of ℝ(n).
        Ok(Value::from_f64(libm_acosh(number.as_f64())))
    }

    // 21.3.2.4 Math.asin ( x ), https://tc39.es/ecma262/#sec-math.asin
    fn asin(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
        if number.is_nan() || number.is_positive_zero() || number.is_negative_zero() {
            return Ok(number);
        }

        // 3. If n > 1𝔽 or n < -1𝔽, return NaN.
        if number.as_f64() > 1.0 || number.as_f64() < -1.0 {
            return Ok(js_nan());
        }

        // 4. Return an implementation-approximated Number value representing the result of the inverse sine of ℝ(n).
        Ok(Value::from_f64(number.as_f64().asin()))
    }

    // 21.3.2.5 Math.asinh ( x ), https://tc39.es/ecma262/#sec-math.asinh
    fn asinh(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
        if !number.is_finite_number() || number.is_positive_zero() || number.is_negative_zero() {
            return Ok(number);
        }

        // 3. Return an implementation-approximated Number value representing the result of the inverse hyperbolic sine of ℝ(n).
        Ok(Value::from_f64(libm_asinh(number.as_f64())))
    }

    // 21.3.2.6 Math.atan ( x ), https://tc39.es/ecma262/#sec-math.atan
    fn atan(vm: &Vm) -> ThrowCompletionOr<Value> {
        // Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is one of NaN, +0𝔽, or -0𝔽, return n.
        if number.is_nan() || number.as_f64() == 0.0 {
            return Ok(number);
        }

        // 3. If n is +∞𝔽, return an implementation-approximated Number value representing π / 2.
        if number.is_positive_infinity() {
            return Ok(Value::from_f64(M_PI_2));
        }

        // 4. If n is -∞𝔽, return an implementation-approximated Number value representing -π / 2.
        if number.is_negative_infinity() {
            return Ok(Value::from_f64(-M_PI_2));
        }

        // 5. Return an implementation-approximated Number value representing the result of the inverse tangent of ℝ(n).
        Ok(Value::from_f64(number.as_f64().atan()))
    }

    // 21.3.2.7 Math.atanh ( x ), https://tc39.es/ecma262/#sec-math.atanh
    fn atanh(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
        if number.is_nan() || number.is_positive_zero() || number.is_negative_zero() {
            return Ok(number);
        }

        // 3. If n > 1𝔽 or n < -1𝔽, return NaN.
        if number.as_f64() > 1.0 || number.as_f64() < -1.0 {
            return Ok(js_nan());
        }

        // 4. If n is 1𝔽, return +∞𝔽.
        if number.as_f64() == 1.0 {
            return Ok(js_infinity());
        }

        // 5. If n is -1𝔽, return -∞𝔽.
        if number.as_f64() == -1.0 {
            return Ok(js_negative_infinity());
        }

        // 6. Return an implementation-approximated Number value representing the result of the inverse hyperbolic tangent of ℝ(n).
        Ok(Value::from_f64(libm_atanh(number.as_f64())))
    }

    // 21.3.2.8 Math.atan2 ( y, x ), https://tc39.es/ecma262/#sec-math.atan2
    fn atan2(vm: &Vm) -> ThrowCompletionOr<Value> {
        let three_quarters_pi = M_PI_4 + M_PI_2;

        // 1. Let ny be ? ToNumber(y).
        let y = vm.argument(0).to_number(vm)?;

        // 2. Let nx be ? ToNumber(x).
        let x = vm.argument(1).to_number(vm)?;

        // 3. If ny is NaN or nx is NaN, return NaN.
        if y.is_nan() || x.is_nan() {
            return Ok(js_nan());
        }

        // 4. If ny is +∞𝔽, then
        if y.is_positive_infinity() {
            // a. If nx is +∞𝔽, return an implementation-approximated Number value representing π / 4.
            if x.is_positive_infinity() {
                return Ok(Value::from_f64(M_PI_4));
            }

            // b. If nx is -∞𝔽, return an implementation-approximated Number value representing 3π / 4.
            if x.is_negative_infinity() {
                return Ok(Value::from_f64(three_quarters_pi));
            }

            // c. Return an implementation-approximated Number value representing π / 2.
            return Ok(Value::from_f64(M_PI_2));
        }

        // 5. If ny is -∞𝔽, then
        if y.is_negative_infinity() {
            // a. If nx is +∞𝔽, return an implementation-approximated Number value representing -π / 4.
            if x.is_positive_infinity() {
                return Ok(Value::from_f64(-M_PI_4));
            }

            // b. If nx is -∞𝔽, return an implementation-approximated Number value representing -3π / 4.
            if x.is_negative_infinity() {
                return Ok(Value::from_f64(-three_quarters_pi));
            }

            // c. Return an implementation-approximated Number value representing -π / 2.
            return Ok(Value::from_f64(-M_PI_2));
        }

        // 6. If ny is +0𝔽, then
        if y.is_positive_zero() {
            // a. If nx > +0𝔽 or nx is +0𝔽, return +0𝔽.
            if x.as_f64() > 0.0 || x.is_positive_zero() {
                return Ok(Value::from_f64(0.0));
            }

            // b. Return an implementation-approximated Number value representing π.
            return Ok(Value::from_f64(M_PI));
        }

        // 7. If ny is -0𝔽, then
        if y.is_negative_zero() {
            // a. If nx > +0𝔽 or nx is +0𝔽, return -0𝔽
            if x.as_f64() > 0.0 || x.is_positive_zero() {
                return Ok(Value::from_f64(-0.0));
            }

            // b. Return an implementation-approximated Number value representing -π.
            return Ok(Value::from_f64(-M_PI));
        }

        // 8. Assert: ny is finite and is neither +0𝔽 nor -0𝔽.
        assert!(y.is_finite_number() && !y.is_positive_zero() && !y.is_negative_zero());

        // 9. If ny > +0𝔽, then
        if y.as_f64() > 0.0 {
            // a. If nx is +∞𝔽, return +0𝔽.
            if x.is_positive_infinity() {
                return Ok(Value::from_i32(0));
            }

            // b. If nx is -∞𝔽, return an implementation-approximated Number value representing π.
            if x.is_negative_infinity() {
                return Ok(Value::from_f64(M_PI));
            }

            // c. If nx is either +0𝔽 or -0𝔽, return an implementation-approximated Number value representing π / 2.
            if x.is_positive_zero() || x.is_negative_zero() {
                return Ok(Value::from_f64(M_PI_2));
            }
        }

        // 10. If ny < -0𝔽, then
        if y.as_f64() < -0.0 {
            // a. If nx is +∞𝔽, return -0𝔽.
            if x.is_positive_infinity() {
                return Ok(Value::from_f64(-0.0));
            }

            // b. If nx is -∞𝔽, return an implementation-approximated Number value representing -π.
            if x.is_negative_infinity() {
                return Ok(Value::from_f64(-M_PI));
            }

            // c. If nx is either +0𝔽 or -0𝔽, return an implementation-approximated Number value representing -π / 2.
            if x.is_positive_zero() || x.is_negative_zero() {
                return Ok(Value::from_f64(-M_PI_2));
            }
        }

        // 11. Assert: nx is finite and is neither +0𝔽 nor -0𝔽.
        assert!(x.is_finite_number() && !x.is_positive_zero() && !x.is_negative_zero());

        // 12. Return an implementation-approximated Number value representing the result of the inverse tangent of the quotient ℝ(ny) / ℝ(nx).
        Ok(Value::from_f64(y.as_f64().atan2(x.as_f64())))
    }

    // 21.3.2.9 Math.cbrt ( x ), https://tc39.es/ecma262/#sec-math.cbrt
    fn cbrt(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
        if !number.is_finite_number() || number.as_f64() == 0.0 {
            return Ok(number);
        }

        // 3. Return an implementation-approximated Number value representing the result of the cube root of ℝ(n).
        Ok(Value::from_f64(number.as_f64().cbrt()))
    }

    // 21.3.2.10 Math.ceil ( x ), https://tc39.es/ecma262/#sec-math.ceil
    fn ceil(vm: &Vm) -> ThrowCompletionOr<Value> {
        ceil_impl(vm, vm.argument(0))
    }

    // 21.3.2.11 Math.clz32 ( x ), https://tc39.es/ecma262/#sec-math.clz32
    fn clz32(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToUint32(x).
        let number = vm.argument(0).to_u32(vm)?;

        // 2. Let p be the number of leading zero bits in the unsigned 32-bit binary representation of n.
        // 3. Return 𝔽(p).
        Ok(Value::from_i32(number.leading_zeros() as i32))
    }

    fn cos(vm: &Vm) -> ThrowCompletionOr<Value> {
        cos_impl(vm, vm.argument(0))
    }

    // 21.3.2.13 Math.cosh ( x ), https://tc39.es/ecma262/#sec-math.cosh
    fn cosh(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, return NaN.
        if number.is_nan() {
            return Ok(js_nan());
        }

        // 3. If n is +∞𝔽 or n is -∞𝔽, return +∞𝔽.
        if number.is_positive_infinity() || number.is_negative_infinity() {
            return Ok(js_infinity());
        }

        // 4. If n is +0𝔽 or n is -0𝔽, return 1𝔽.
        if number.is_positive_zero() || number.is_negative_zero() {
            return Ok(Value::from_i32(1));
        }

        // 5. Return an implementation-approximated Number value representing the result of the hyperbolic cosine of ℝ(n).
        Ok(Value::from_f64(number.as_f64().cosh()))
    }

    // 21.3.2.14 Math.exp ( x ), https://tc39.es/ecma262/#sec-math.exp
    fn exp(vm: &Vm) -> ThrowCompletionOr<Value> {
        exp_impl(vm, vm.argument(0))
    }

    // 21.3.2.15 Math.expm1 ( x ), https://tc39.es/ecma262/#sec-math.expm1
    fn expm1(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is one of NaN, +0𝔽, -0𝔽, or +∞𝔽, return n.
        if number.is_nan() || number.as_f64() == 0.0 || number.is_positive_infinity() {
            return Ok(number);
        }

        // 3. If n is -∞𝔽, return -1𝔽.
        if number.is_negative_infinity() {
            return Ok(Value::from_i32(-1));
        }

        // 4. Return an implementation-approximated Number value representing the result of subtracting 1 from the exponential function of ℝ(n).
        Ok(Value::from_f64(number.as_f64().exp_m1()))
    }

    // 21.3.2.16 Math.floor ( x ), https://tc39.es/ecma262/#sec-math.floor
    fn floor(vm: &Vm) -> ThrowCompletionOr<Value> {
        floor_impl(vm, vm.argument(0))
    }

    // 21.3.2.17 Math.fround ( x ), https://tc39.es/ecma262/#sec-math.fround
    fn fround(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, return NaN.
        if number.is_nan() {
            return Ok(js_nan());
        }

        // 3. If n is one of +0𝔽, -0𝔽, +∞𝔽, or -∞𝔽, return n.
        if number.as_f64() == 0.0 || number.is_infinity() {
            return Ok(number);
        }

        // 4. Let n32 be the result of converting n to a value in IEEE 754-2019 binary32 format using roundTiesToEven mode.
        // 5. Let n64 be the result of converting n32 to a value in IEEE 754-2019 binary64 format.
        // 6. Return the ECMAScript Number value corresponding to n64.
        Ok(Value::from_f64(f64::from(number.as_f64() as f32)))
    }

    // 21.3.2.18 Math.f16round ( x ), https://tc39.es/ecma262/#sec-math.f16round
    fn f16round(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, return NaN.
        if number.is_nan() {
            return Ok(js_nan());
        }

        // 3. If n is one of +0𝔽, -0𝔽, +∞𝔽, or -∞𝔽, return n.
        if number.as_f64() == 0.0 || number.is_infinity() {
            return Ok(number);
        }

        // 4. Let n16 be the result of converting n to IEEE 754-2019 binary16 format using roundTiesToEven mode.
        // 5. Let n64 be the result of converting n16 to IEEE 754-2019 binary64 format.
        // 6. Return the ECMAScript Number value corresponding to n64.
        Ok(Value::from_f64(round_to_binary16(number.as_f64())))
    }

    // 21.3.2.19 Math.hypot ( ...args ), https://tc39.es/ecma262/#sec-math.hypot
    fn hypot(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let coerced be a new empty List.
        // 2. For each element arg of args, do
        //     a. Let n be ? ToNumber(arg).
        //     b. Append n to coerced.
        let coerced = CoercedArguments::of(vm)?;

        // 3. For each element number of coerced, do
        for number in coerced.iter() {
            // a. If number is either +∞𝔽 or -∞𝔽, return +∞𝔽.
            if number.is_infinite() {
                return Ok(js_infinity());
            }
        }

        // 4. Let onlyZero be true.
        let mut only_zero = true;

        let mut sum_of_squares = 0.0;

        // 5. For each element number of coerced, do
        for &number in coerced.iter() {
            // a. If number is NaN, return NaN.
            // OPTIMIZATION: For infinities, the result will be infinity with the same sign, so we can return early.
            if number.is_nan() || number.is_infinite() {
                return Ok(Value::from_f64(number));
            }

            // b. If number is neither +0𝔽 nor -0𝔽, set onlyZero to false.
            if number != 0.0 {
                only_zero = false;
            }

            sum_of_squares += number * number;
        }

        // 6. If onlyZero is true, return +0𝔽.
        if only_zero {
            return Ok(Value::from_i32(0));
        }

        // 7. Return an implementation-approximated Number value representing the square root of the sum of squares of the mathematical values of the elements of coerced.
        Ok(Value::from_f64(f64::sqrt(sum_of_squares)))
    }

    // 21.3.2.20 Math.imul ( x, y ), https://tc39.es/ecma262/#sec-math.imul
    fn imul(vm: &Vm) -> ThrowCompletionOr<Value> {
        imul_impl(vm, vm.argument(0), vm.argument(1))
    }

    // 21.3.2.21 Math.log ( x ), https://tc39.es/ecma262/#sec-math.log
    fn log(vm: &Vm) -> ThrowCompletionOr<Value> {
        log_impl(vm, vm.argument(0))
    }

    // 21.3.2.22 Math.log1p ( x ), https://tc39.es/ecma262/#sec-math.log1p
    fn log1p(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, n is +0𝔽, n is -0𝔽, or n is +∞𝔽, return n.
        if number.is_nan() || number.is_positive_zero() || number.is_negative_zero() || number.is_positive_infinity() {
            return Ok(number);
        }

        // 3. If n is -1𝔽, return -∞𝔽.
        if number.as_f64() == -1.0 {
            return Ok(js_negative_infinity());
        }

        // 4. If n < -1𝔽, return NaN.
        if number.as_f64() < -1.0 {
            return Ok(js_nan());
        }

        // 5. Return an implementation-approximated Number value representing the result of the natural logarithm of 1 + ℝ(n).
        Ok(Value::from_f64(number.as_f64().ln_1p()))
    }

    // 21.3.2.23 Math.log10 ( x ), https://tc39.es/ecma262/#sec-math.log10
    fn log10(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

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

        // 6. Return an implementation-approximated Number value representing the result of the base 10 logarithm of ℝ(n).
        Ok(Value::from_f64(number.as_f64().log10()))
    }

    // 21.3.2.24 Math.log2 ( x ), https://tc39.es/ecma262/#sec-math.log2
    fn log2(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

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

        // 6. Return an implementation-approximated Number value representing the result of the base 2 logarithm of ℝ(n).
        Ok(Value::from_f64(number.as_f64().log2()))
    }

    // 21.3.2.25 Math.max ( ...args ), https://tc39.es/ecma262/#sec-math.max
    fn max(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let coerced be a new empty List.
        // 2. For each element arg of args, do
        //     a. Let n be ? ToNumber(arg).
        //     b. Append n to coerced.
        let coerced = CoercedArguments::of(vm)?;

        // 3. Let highest be -∞𝔽.
        let mut highest = f64::NEG_INFINITY;

        // 4. For each element number of coerced, do
        for &number in coerced.iter() {
            // a. If number is NaN, return NaN.
            if number.is_nan() {
                return Ok(js_nan());
            }

            // b. If number is +0𝔽 and highest is -0𝔽, set highest to +0𝔽.
            // c. If number > highest, set highest to number.
            if (is_positive_zero(number) && is_negative_zero(highest)) || number > highest {
                highest = number;
            }
        }

        // 5. Return highest.
        Ok(Value::from_f64(highest))
    }

    // 21.3.2.26 Math.min ( ...args ), https://tc39.es/ecma262/#sec-math.min
    fn min(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let coerced be a new empty List.
        // 2. For each element arg of args, do
        //     a. Let n be ? ToNumber(arg).
        //     b. Append n to coerced.
        let coerced = CoercedArguments::of(vm)?;

        // 3. Let lowest be +∞𝔽.
        let mut lowest = f64::INFINITY;

        // 4. For each element number of coerced, do
        for &number in coerced.iter() {
            // a. If number is NaN, return NaN.
            if number.is_nan() {
                return Ok(js_nan());
            }

            // b. If number is -0𝔽 and lowest is +0𝔽, set lowest to -0𝔽.
            // c. If number < lowest, set lowest to number.
            if (is_negative_zero(number) && is_positive_zero(lowest)) || number < lowest {
                lowest = number;
            }
        }

        // 5. Return lowest.
        Ok(Value::from_f64(lowest))
    }

    // 21.3.2.27 Math.pow ( base, exponent ), https://tc39.es/ecma262/#sec-math.pow
    fn pow(vm: &Vm) -> ThrowCompletionOr<Value> {
        pow_impl(vm, vm.argument(0), vm.argument(1))
    }

    // 21.3.2.28 Math.random ( ), https://tc39.es/ecma262/#sec-math.random
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn random(_vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(random_impl())
    }

    // 21.3.2.29 Math.round ( x ), https://tc39.es/ecma262/#sec-math.round
    fn round(vm: &Vm) -> ThrowCompletionOr<Value> {
        round_impl(vm, vm.argument(0))
    }

    // 21.3.2.30 Math.sign ( x ), https://tc39.es/ecma262/#sec-math.sign
    fn sign(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is one of NaN, +0𝔽, or -0𝔽, return n.
        if number.is_nan() || number.as_f64() == 0.0 {
            return Ok(number);
        }

        // 3. If n < -0𝔽, return -1𝔽.
        if number.as_f64() < 0.0 {
            return Ok(Value::from_i32(-1));
        }

        // 4. Return 1𝔽.
        Ok(Value::from_i32(1))
    }

    fn sin(vm: &Vm) -> ThrowCompletionOr<Value> {
        sin_impl(vm, vm.argument(0))
    }

    // 21.3.2.32 Math.sinh ( x ), https://tc39.es/ecma262/#sec-math.sinh
    fn sinh(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
        if !number.is_finite_number() || number.is_positive_zero() || number.is_negative_zero() {
            return Ok(number);
        }

        // 3. Return an implementation-approximated Number value representing the result of the hyperbolic sine of ℝ(n).
        Ok(Value::from_f64(number.as_f64().sinh()))
    }

    // 21.3.2.33 Math.sqrt ( x ), https://tc39.es/ecma262/#sec-math.sqrt
    fn sqrt(vm: &Vm) -> ThrowCompletionOr<Value> {
        sqrt_impl(vm, vm.argument(0))
    }

    // 21.3.2.34 Math.sumPrecise ( items ), https://tc39.es/ecma262/#sec-math.sumprecise
    fn sum_precise(vm: &Vm) -> ThrowCompletionOr<Value> {
        const MAX_DOUBLE: f64 = f64::MAX;
        let max_ulp = MAX_DOUBLE - f64::from_bits(MAX_DOUBLE.to_bits() - 1);

        const POW_2_1023: f64 = 8.98846567431158e+307;

        #[derive(Clone, Copy, PartialEq, Eq)]
        enum State {
            MinusZero,
            PlusInfinity,
            MinusInfinity,
            NotANumber,
            Finite,
        }

        let items = vm.argument(0);

        // 1. Perform ? RequireObjectCoercible(items).
        require_object_coercible(vm, items)?;

        // 2. Let iteratorRecord be ? GetIterator(items, SYNC).
        let iterator_record = get_iterator(vm, items, IteratorHint::Sync)?;

        // 3. Let state be MINUS-ZERO.
        let mut state = State::MinusZero;

        // 4. Let sum be 0.
        // 5. Let count be 0.
        let mut overflow = 0.0;
        let mut count: u64 = 0;
        let mut partials: Vec<f64> = Vec::new();

        // 6. Let next be NOT-STARTED.
        // 7. Repeat, while next is not DONE
        // a. Set next to ? IteratorStepValue(iteratorRecord).
        while let Some(next_value) = iterator_step_value(vm, &iterator_record)? {
            // b. If next is not DONE, then

            // i. If count ≥ 2**53 - 1, then
            if count >= (1u64 << 53) - 1 {
                // 1. NOTE: This step is not expected to be reached in practice and is included only so that implementations
                //    may rely on inputs being "reasonably sized" without violating this specification.

                // 2. Let error be ThrowCompletion(a newly created RangeError object).
                let error = vm.throw_completion::<Value>(ErrorKind::RangeError, ErrorType::ArrayMaxSize, &[]);

                // 3. Return ? IteratorClose(iteratorRecord, error).
                return iterator_close(vm, &iterator_record, Completion::from(error)).into_throw_completion_or();
            }

            // ii. If next is not a Number, then
            if !next_value.is_number() {
                // 1. Let error be ThrowCompletion(a newly created TypeError object).
                let error =
                    vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::IsNotA, &[&next_value, &"number"]);

                // 2. Return ? IteratorClose(iteratorRecord, error).
                return iterator_close(vm, &iterator_record, Completion::from(error)).into_throw_completion_or();
            }

            // iii. Let n be next.
            let n = next_value.as_f64();

            // iv. If state is not NOT-A-NUMBER, then
            if state != State::NotANumber {
                // 1. If n is NaN, then
                if next_value.is_nan() {
                    // a. Set state to NOT-A-NUMBER.
                    state = State::NotANumber;
                }
                // 2. Else if n is +∞𝔽, then
                else if next_value.is_positive_infinity() {
                    // a. If state is MINUS-INFINITY, set state to NOT-A-NUMBER.
                    // b. Else, set state to PLUS-INFINITY.
                    state = if state == State::MinusInfinity {
                        State::NotANumber
                    } else {
                        State::PlusInfinity
                    };
                }
                // 3. Else if n is -∞𝔽, then
                else if next_value.is_negative_infinity() {
                    // a. If state is PLUS-INFINITY, set state to NOT-A-NUMBER.
                    // b. Else, set state to MINUS-INFINITY.
                    state = if state == State::PlusInfinity {
                        State::NotANumber
                    } else {
                        State::MinusInfinity
                    };
                }
                // 4. Else if n is not -0𝔽 and state is either MINUS-ZERO or FINITE, then
                else if !next_value.is_negative_zero() && (state == State::MinusZero || state == State::Finite) {
                    // a. Set state to FINITE.
                    state = State::Finite;

                    // b. Set sum to sum + ℝ(n).
                    let mut x = n;
                    let mut used_partials = 0;

                    for i in 0..partials.len() {
                        let mut y = partials[i];

                        if x.abs() < y.abs() {
                            core::mem::swap(&mut x, &mut y);
                        }

                        let mut result = two_sum(x, y);
                        let mut hi = result.hi;
                        let mut lo = result.lo;

                        if hi.is_infinite() {
                            let sign = if hi.is_sign_negative() { -1.0 } else { 1.0 };
                            overflow += sign;

                            if f64::abs(overflow) >= (1u64 << 53) as f64 {
                                return vm.throw_completion(
                                    ErrorKind::RangeError,
                                    ErrorType::MathSumPreciseOverflow,
                                    &[],
                                );
                            }

                            x = (x - sign * POW_2_1023) - sign * POW_2_1023;

                            if x.abs() < y.abs() {
                                core::mem::swap(&mut x, &mut y);
                            }

                            result = two_sum(x, y);
                            hi = result.hi;
                            lo = result.lo;
                        }

                        if lo != 0.0 {
                            partials[used_partials] = lo;
                            used_partials += 1;
                        }

                        x = hi;
                    }

                    partials.truncate(used_partials);

                    if x != 0.0 {
                        partials.push(x);
                    }
                }
            }

            // v. Set count to count + 1.
            count += 1;
        }

        // 8. If state is NOT-A-NUMBER, return NaN.
        if state == State::NotANumber {
            return Ok(js_nan());
        }

        // 9. If state is PLUS-INFINITY, return +∞𝔽.
        if state == State::PlusInfinity {
            return Ok(js_infinity());
        }

        // 10. If state is MINUS-INFINITY, return -∞𝔽.
        if state == State::MinusInfinity {
            return Ok(js_negative_infinity());
        }

        // 11. If state is MINUS-ZERO, return -0𝔽.
        if state == State::MinusZero {
            return Ok(Value::from_f64(-0.0));
        }

        // 12. Return 𝔽(sum).
        let mut n = partials.len() as isize - 1;
        let mut hi = 0.0;
        let mut lo = 0.0;

        if overflow != 0.0 {
            let next = if n >= 0 { partials[n as usize] } else { 0.0 };
            n -= 1;

            if f64::abs(overflow) > 1.0 || (overflow > 0.0 && next > 0.0) || (overflow < 0.0 && next < 0.0) {
                return Ok(if overflow > 0.0 {
                    js_infinity()
                } else {
                    js_negative_infinity()
                });
            }

            let result = two_sum(overflow * POW_2_1023, next / 2.0);
            hi = result.hi;
            lo = result.lo * 2.0;

            if (hi * 2.0).is_infinite() {
                if hi > 0.0 {
                    if hi == POW_2_1023 && lo == -(max_ulp / 2.0) && n >= 0 && partials[n as usize] < 0.0 {
                        return Ok(Value::from_f64(MAX_DOUBLE));
                    }

                    return Ok(js_infinity());
                }

                if hi == -POW_2_1023 && lo == (max_ulp / 2.0) && n >= 0 && partials[n as usize] > 0.0 {
                    return Ok(Value::from_f64(-MAX_DOUBLE));
                }

                return Ok(js_negative_infinity());
            }

            if lo != 0.0 {
                partials[(n + 1) as usize] = lo;
                n += 1;
                lo = 0.0;
            }

            hi *= 2.0;
        }

        while n >= 0 {
            let x = hi;
            let y = partials[n as usize];
            n -= 1;

            let result = two_sum(x, y);
            hi = result.hi;
            lo = result.lo;

            if lo != 0.0 {
                break;
            }
        }

        if n >= 0 && ((lo < 0.0 && partials[n as usize] < 0.0) || (lo > 0.0 && partials[n as usize] > 0.0)) {
            let y = lo * 2.0;
            let x = hi + y;
            let yr = x - hi;

            if y == yr {
                hi = x;
            }
        }

        Ok(Value::from_f64(hi))
    }

    fn tan(vm: &Vm) -> ThrowCompletionOr<Value> {
        tan_impl(vm, vm.argument(0))
    }

    // 21.3.2.36 Math.tanh ( x ), https://tc39.es/ecma262/#sec-math.tanh
    fn tanh(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
        if number.is_nan() || number.is_positive_zero() || number.is_negative_zero() {
            return Ok(number);
        }

        // 3. If n is +∞𝔽, return 1𝔽.
        if number.is_positive_infinity() {
            return Ok(Value::from_i32(1));
        }

        // 4. If n is -∞𝔽, return -1𝔽.
        if number.is_negative_infinity() {
            return Ok(Value::from_i32(-1));
        }

        // 5. Return an implementation-approximated Number value representing the result of the hyperbolic tangent of ℝ(n).
        Ok(Value::from_f64(number.as_f64().tanh()))
    }

    // 21.3.2.37 Math.trunc ( x ), https://tc39.es/ecma262/#sec-math.trunc
    fn trunc(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let n be ? ToNumber(x).
        let number = vm.argument(0).to_number(vm)?;

        // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
        if number.is_nan() || number.is_infinity() || number.as_f64() == 0.0 {
            return Ok(number);
        }

        // 3. If n < 1𝔽 and n > +0𝔽, return +0𝔽.
        // 4. If n < -0𝔽 and n > -1𝔽, return -0𝔽.
        // 5. Return the integral Number nearest n in the direction of +0𝔽.
        Ok(Value::from_f64(if number.as_f64() < 0.0 {
            number.as_f64().ceil()
        } else {
            number.as_f64().floor()
        }))
    }
}

fn is_positive_zero(number: f64) -> bool {
    number == 0.0 && number.is_sign_positive()
}

fn is_negative_zero(number: f64) -> bool {
    number == 0.0 && number.is_sign_negative()
}

/// static_cast<f16>(double): the binary16 value nearest a finite, nonzero double, ties to the even significand, as a
/// double.
pub(crate) fn round_to_binary16(number: f64) -> f64 {
    const LARGEST_FINITE_BINARY16: f64 = 65504.0;
    const SMALLEST_NORMAL_BINARY16_EXPONENT: i32 = -14;
    const BINARY16_SIGNIFICAND_BITS: i32 = 10;

    let magnitude = number.abs();
    let unbiased_exponent = ((magnitude.to_bits() >> 52) as i32) - 1023;
    // Subnormal binary16 values are all multiples of the smallest subnormal, 2^-24.
    let quantum_exponent = unbiased_exponent.max(SMALLEST_NORMAL_BINARY16_EXPONENT) - BINARY16_SIGNIFICAND_BITS;
    // Scaling by powers of two is exact here, so only the rounding to an integer is inexact.
    let quantum = f64::from_bits(((quantum_exponent + 1023) as u64) << 52);
    let rounded = (magnitude / quantum).round_ties_even() * quantum;
    let rounded = if rounded > LARGEST_FINITE_BINARY16 {
        f64::INFINITY
    } else {
        rounded
    };
    rounded.copysign(number)
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

/// The generator of Math.random. Every realm and VM of the process draws from the one generator, seeded the first time
/// it is used.
static RANDOM_NUMBER_GENERATOR: LazyLock<Mutex<XorShift128PlusRNG>> =
    LazyLock::new(|| Mutex::new(XorShift128PlusRNG::new()));

pub fn random_impl() -> Value {
    // This function returns a Number value with positive sign, greater than or equal to +0𝔽 but strictly less than 1𝔽,
    // chosen randomly or pseudo randomly with approximately uniform distribution over that range, using an
    // implementation-defined algorithm or strategy.
    let mut rng = RANDOM_NUMBER_GENERATOR
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    Value::from_f64(rng.get())
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

struct TwoSumResult {
    hi: f64,
    lo: f64,
}

fn two_sum(x: f64, y: f64) -> TwoSumResult {
    let hi = x + y;
    let lo = y - (hi - x);
    TwoSumResult { hi, lo }
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
