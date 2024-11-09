/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BuiltinWrappers.h>
#include <AK/Function.h>
#include <AK/Random.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/MathObject.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <math.h>

namespace JS {

JS_DEFINE_ALLOCATOR(MathObject);

MathObject::MathObject(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void MathObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.abs, abs, 1, attr, Bytecode::Builtin::MathAbs);
    define_native_function(realm, vm.names.random, random, 0, attr);
    define_native_function(realm, vm.names.sqrt, sqrt, 1, attr, Bytecode::Builtin::MathSqrt);
    define_native_function(realm, vm.names.floor, floor, 1, attr, Bytecode::Builtin::MathFloor);
    define_native_function(realm, vm.names.ceil, ceil, 1, attr, Bytecode::Builtin::MathCeil);
    define_native_function(realm, vm.names.round, round, 1, attr, Bytecode::Builtin::MathRound);
    define_native_function(realm, vm.names.max, max, 2, attr);
    define_native_function(realm, vm.names.min, min, 2, attr);
    define_native_function(realm, vm.names.trunc, trunc, 1, attr);
    define_native_function(realm, vm.names.sin, sin, 1, attr);
    define_native_function(realm, vm.names.cos, cos, 1, attr);
    define_native_function(realm, vm.names.tan, tan, 1, attr);
    define_native_function(realm, vm.names.pow, pow, 2, attr, Bytecode::Builtin::MathPow);
    define_native_function(realm, vm.names.exp, exp, 1, attr, Bytecode::Builtin::MathExp);
    define_native_function(realm, vm.names.expm1, expm1, 1, attr);
    define_native_function(realm, vm.names.sign, sign, 1, attr);
    define_native_function(realm, vm.names.clz32, clz32, 1, attr);
    define_native_function(realm, vm.names.acos, acos, 1, attr);
    define_native_function(realm, vm.names.acosh, acosh, 1, attr);
    define_native_function(realm, vm.names.asin, asin, 1, attr);
    define_native_function(realm, vm.names.asinh, asinh, 1, attr);
    define_native_function(realm, vm.names.atan, atan, 1, attr);
    define_native_function(realm, vm.names.atanh, atanh, 1, attr);
    define_native_function(realm, vm.names.log1p, log1p, 1, attr);
    define_native_function(realm, vm.names.cbrt, cbrt, 1, attr);
    define_native_function(realm, vm.names.atan2, atan2, 2, attr);
    define_native_function(realm, vm.names.fround, fround, 1, attr);
    define_native_function(realm, vm.names.hypot, hypot, 2, attr);
    define_native_function(realm, vm.names.imul, imul, 2, attr);
    define_native_function(realm, vm.names.log, log, 1, attr, Bytecode::Builtin::MathLog);
    define_native_function(realm, vm.names.log2, log2, 1, attr);
    define_native_function(realm, vm.names.log10, log10, 1, attr);
    define_native_function(realm, vm.names.sinh, sinh, 1, attr);
    define_native_function(realm, vm.names.cosh, cosh, 1, attr);
    define_native_function(realm, vm.names.tanh, tanh, 1, attr);

    // 21.3.1 Value Properties of the Math Object, https://tc39.es/ecma262/#sec-value-properties-of-the-math-object
    define_direct_property(vm.names.E, Value(M_E), 0);
    define_direct_property(vm.names.LN2, Value(M_LN2), 0);
    define_direct_property(vm.names.LN10, Value(M_LN10), 0);
    define_direct_property(vm.names.LOG2E, Value(::log2(M_E)), 0);
    define_direct_property(vm.names.LOG10E, Value(::log10(M_E)), 0);
    define_direct_property(vm.names.PI, Value(M_PI), 0);
    define_direct_property(vm.names.SQRT1_2, Value(M_SQRT1_2), 0);
    define_direct_property(vm.names.SQRT2, Value(M_SQRT2), 0);

    // 21.3.1.9 Math [ @@toStringTag ], https://tc39.es/ecma262/#sec-math-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, vm.names.Math.as_string()), Attribute::Configurable);
}

// 21.3.2.1 Math.abs ( x ), https://tc39.es/ecma262/#sec-math.abs
ThrowCompletionOr<Value> MathObject::abs_impl(VM& vm, Value x)
{
    // OPTIMIZATION: Fast path for Int32 values.
    if (x.is_int32())
        return Value(AK::abs(x.as_i32()));

    // Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is NaN, return NaN.
    if (number.is_nan())
        return js_nan();

    // 3. If n is -0𝔽, return +0𝔽.
    if (number.is_negative_zero())
        return Value(0);

    // 4. If n is -∞𝔽, return +∞𝔽.
    if (number.is_negative_infinity())
        return js_infinity();

    // 5. If n < -0𝔽, return -n.
    // 6. Return n.
    return Value(number.as_double() < 0 ? -number.as_double() : number.as_double());
}

// 21.3.2.1 Math.abs ( x ), https://tc39.es/ecma262/#sec-math.abs
JS_DEFINE_NATIVE_FUNCTION(MathObject::abs)
{
    return abs_impl(vm, vm.argument(0));
}

// 21.3.2.2 Math.acos ( x ), https://tc39.es/ecma262/#sec-math.acos
JS_DEFINE_NATIVE_FUNCTION(MathObject::acos)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n > 1𝔽, or n < -1𝔽, return NaN.
    if (number.is_nan() || number.as_double() > 1 || number.as_double() < -1)
        return js_nan();

    // 3. If n is 1𝔽, return +0𝔽.
    if (number.as_double() == 1)
        return Value(0);

    // 4. Return an implementation-approximated Number value representing the result of the inverse cosine of ℝ(n).
    return Value(::acos(number.as_double()));
}

// 21.3.2.3 Math.acosh ( x ), https://tc39.es/ecma262/#sec-math.acosh
JS_DEFINE_NATIVE_FUNCTION(MathObject::acosh)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN or n is +∞𝔽, return n.
    if (number.is_nan() || number.is_positive_infinity())
        return number;

    // 3. If n is 1𝔽, return +0𝔽.
    if (number.as_double() == 1.0)
        return Value(0.0);

    // 4. If n < 1𝔽, return NaN.
    if (number.as_double() < 1)
        return js_nan();

    // 5. Return an implementation-approximated Number value representing the result of the inverse hyperbolic cosine of ℝ(n).
    return Value(::acosh(number.as_double()));
}

// 21.3.2.4 Math.asin ( x ), https://tc39.es/ecma262/#sec-math.asin
JS_DEFINE_NATIVE_FUNCTION(MathObject::asin)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. If n > 1𝔽 or n < -1𝔽, return NaN.
    if (number.as_double() > 1 || number.as_double() < -1)
        return js_nan();

    // 4. Return an implementation-approximated Number value representing the result of the inverse sine of ℝ(n).
    return Value(::asin(number.as_double()));
}

// 21.3.2.5 Math.asinh ( x ), https://tc39.es/ecma262/#sec-math.asinh
JS_DEFINE_NATIVE_FUNCTION(MathObject::asinh)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if (!number.is_finite_number() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. Return an implementation-approximated Number value representing the result of the inverse hyperbolic sine of ℝ(n).
    return Value(::asinh(number.as_double()));
}

// 21.3.2.6 Math.atan ( x ), https://tc39.es/ecma262/#sec-math.atan
JS_DEFINE_NATIVE_FUNCTION(MathObject::atan)
{
    // Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is one of NaN, +0𝔽, or -0𝔽, return n.
    if (number.is_nan() || number.as_double() == 0)
        return number;

    // 3. If n is +∞𝔽, return an implementation-approximated Number value representing π / 2.
    if (number.is_positive_infinity())
        return Value(M_PI_2);

    // 4. If n is -∞𝔽, return an implementation-approximated Number value representing -π / 2.
    if (number.is_negative_infinity())
        return Value(-M_PI_2);

    // 5. Return an implementation-approximated Number value representing the result of the inverse tangent of ℝ(n).
    return Value(::atan(number.as_double()));
}

// 21.3.2.7 Math.atanh ( x ), https://tc39.es/ecma262/#sec-math.atanh
JS_DEFINE_NATIVE_FUNCTION(MathObject::atanh)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. If n > 1𝔽 or n < -1𝔽, return NaN.
    if (number.as_double() > 1. || number.as_double() < -1.)
        return js_nan();

    // 4. If n is 1𝔽, return +∞𝔽.
    if (number.as_double() == 1.)
        return js_infinity();

    // 5. If n is -1𝔽, return -∞𝔽.
    if (number.as_double() == -1.)
        return js_negative_infinity();

    // 6. Return an implementation-approximated Number value representing the result of the inverse hyperbolic tangent of ℝ(n).
    return Value(::atanh(number.as_double()));
}

// 21.3.2.8 Math.atan2 ( y, x ), https://tc39.es/ecma262/#sec-math.atan2
JS_DEFINE_NATIVE_FUNCTION(MathObject::atan2)
{
    auto constexpr three_quarters_pi = M_PI_4 + M_PI_2;

    // 1. Let ny be ? ToNumber(y).
    auto y = TRY(vm.argument(0).to_number(vm));

    // 2. Let nx be ? ToNumber(x).
    auto x = TRY(vm.argument(1).to_number(vm));

    // 3. If ny is NaN or nx is NaN, return NaN.
    if (y.is_nan() || x.is_nan())
        return js_nan();

    // 4. If ny is +∞𝔽, then
    if (y.is_positive_infinity()) {
        // a. If nx is +∞𝔽, return an implementation-approximated Number value representing π / 4.
        if (x.is_positive_infinity())
            return Value(M_PI_4);

        // b. If nx is -∞𝔽, return an implementation-approximated Number value representing 3π / 4.
        if (x.is_negative_infinity())
            return Value(three_quarters_pi);

        // c. Return an implementation-approximated Number value representing π / 2.
        return Value(M_PI_2);
    }

    // 5. If ny is -∞𝔽, then
    if (y.is_negative_infinity()) {
        // a. If nx is +∞𝔽, return an implementation-approximated Number value representing -π / 4.
        if (x.is_positive_infinity())
            return Value(-M_PI_4);

        // b. If nx is -∞𝔽, return an implementation-approximated Number value representing -3π / 4.
        if (x.is_negative_infinity())
            return Value(-three_quarters_pi);

        // c. Return an implementation-approximated Number value representing -π / 2.
        return Value(-M_PI_2);
    }

    // 6. If ny is +0𝔽, then
    if (y.is_positive_zero()) {
        // a. If nx > +0𝔽 or nx is +0𝔽, return +0𝔽.
        if (x.as_double() > 0 || x.is_positive_zero())
            return Value(0.0);

        // b. Return an implementation-approximated Number value representing π.
        return Value(M_PI);
    }

    // 7. If ny is -0𝔽, then
    if (y.is_negative_zero()) {
        // a. If nx > +0𝔽 or nx is +0𝔽, return -0𝔽
        if (x.as_double() > 0 || x.is_positive_zero())
            return Value(-0.0);

        // b. Return an implementation-approximated Number value representing -π.
        return Value(-M_PI);
    }

    // 8. Assert: ny is finite and is neither +0𝔽 nor -0𝔽.
    VERIFY(y.is_finite_number() && !y.is_positive_zero() && !y.is_negative_zero());

    // 9. If ny > +0𝔽, then
    if (y.as_double() > 0) {
        // a. If nx is +∞𝔽, return +0𝔽.
        if (x.is_positive_infinity())
            return Value(0);

        // b. If nx is -∞𝔽, return an implementation-approximated Number value representing π.
        if (x.is_negative_infinity())
            return Value(M_PI);

        // c. If nx is either +0𝔽 or -0𝔽, return an implementation-approximated Number value representing π / 2.
        if (x.is_positive_zero() || x.is_negative_zero())
            return Value(M_PI_2);
    }

    // 10. If ny < -0𝔽, then
    if (y.as_double() < -0) {
        // a. If nx is +∞𝔽, return -0𝔽.
        if (x.is_positive_infinity())
            return Value(-0.0);

        // b. If nx is -∞𝔽, return an implementation-approximated Number value representing -π.
        if (x.is_negative_infinity())
            return Value(-M_PI);

        // c. If nx is either +0𝔽 or -0𝔽, return an implementation-approximated Number value representing -π / 2.
        if (x.is_positive_zero() || x.is_negative_zero())
            return Value(-M_PI_2);
    }

    // 11. Assert: nx is finite and is neither +0𝔽 nor -0𝔽.
    VERIFY(x.is_finite_number() && !x.is_positive_zero() && !x.is_negative_zero());

    // 12. Return an implementation-approximated Number value representing the result of the inverse tangent of the quotient ℝ(ny) / ℝ(nx).
    return Value(::atan2(y.as_double(), x.as_double()));
}

// 21.3.2.9 Math.cbrt ( x ), https://tc39.es/ecma262/#sec-math.cbrt
JS_DEFINE_NATIVE_FUNCTION(MathObject::cbrt)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if (!number.is_finite_number() || number.as_double() == 0)
        return number;

    // 3. Return an implementation-approximated Number value representing the result of the cube root of ℝ(n).
    return Value(::cbrt(number.as_double()));
}

// 21.3.2.10 Math.ceil ( x ), https://tc39.es/ecma262/#sec-math.ceil
ThrowCompletionOr<Value> MathObject::ceil_impl(VM& vm, Value x)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if (!number.is_finite_number() || number.as_double() == 0)
        return number;

    // 3. If n < -0𝔽 and n > -1𝔽, return -0𝔽.
    if (number.as_double() < 0 && number.as_double() > -1)
        return Value(-0.f);

    // 4. If n is an integral Number, return n.
    // 5. Return the smallest (closest to -∞) integral Number value that is not less than n.
    return Value(::ceil(number.as_double()));
}

// 21.3.2.10 Math.ceil ( x ), https://tc39.es/ecma262/#sec-math.ceil
JS_DEFINE_NATIVE_FUNCTION(MathObject::ceil)
{
    return ceil_impl(vm, vm.argument(0));
}

// 21.3.2.11 Math.clz32 ( x ), https://tc39.es/ecma262/#sec-math.clz32
JS_DEFINE_NATIVE_FUNCTION(MathObject::clz32)
{
    // 1. Let n be ? ToUint32(x).
    auto number = TRY(vm.argument(0).to_u32(vm));

    // 2. Let p be the number of leading zero bits in the unsigned 32-bit binary representation of n.
    // 3. Return 𝔽(p).
    return Value(count_leading_zeroes_safe(number));
}

// 21.3.2.12 Math.cos ( x ), https://tc39.es/ecma262/#sec-math.cos
JS_DEFINE_NATIVE_FUNCTION(MathObject::cos)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +∞𝔽, or n is -∞𝔽, return NaN.
    if (number.is_nan() || number.is_infinity())
        return js_nan();

    // 3. If n is +0𝔽 or n is -0𝔽, return 1𝔽.
    if (number.is_positive_zero() || number.is_negative_zero())
        return Value(1);

    // 4. Return an implementation-approximated Number value representing the result of the cosine of ℝ(n).
    return Value(::cos(number.as_double()));
}

// 21.3.2.13 Math.cosh ( x ), https://tc39.es/ecma262/#sec-math.cosh
JS_DEFINE_NATIVE_FUNCTION(MathObject::cosh)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, return NaN.
    if (number.is_nan())
        return js_nan();

    // 3. If n is +∞𝔽 or n is -∞𝔽, return +∞𝔽.
    if (number.is_positive_infinity() || number.is_negative_infinity())
        return js_infinity();

    // 4. If n is +0𝔽 or n is -0𝔽, return 1𝔽.
    if (number.is_positive_zero() || number.is_negative_zero())
        return Value(1);

    // 5. Return an implementation-approximated Number value representing the result of the hyperbolic cosine of ℝ(n).
    return Value(::cosh(number.as_double()));
}

// 21.3.2.14 Math.exp ( x ), https://tc39.es/ecma262/#sec-math.exp
ThrowCompletionOr<Value> MathObject::exp_impl(VM& vm, Value x)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is either NaN or +∞𝔽, return n.
    if (number.is_nan() || number.is_positive_infinity())
        return number;

    // 3. If n is either +0𝔽 or -0𝔽, return 1𝔽.
    if (number.as_double() == 0)
        return Value(1);

    // 4. If n is -∞𝔽, return +0𝔽.
    if (number.is_negative_infinity())
        return Value(0);

    // 5. Return an implementation-approximated Number value representing the result of the exponential function of ℝ(n).
    return Value(::exp(number.as_double()));
}

// 21.3.2.14 Math.exp ( x ), https://tc39.es/ecma262/#sec-math.exp
JS_DEFINE_NATIVE_FUNCTION(MathObject::exp)
{
    return exp_impl(vm, vm.argument(0));
}

// 21.3.2.15 Math.expm1 ( x ), https://tc39.es/ecma262/#sec-math.expm1
JS_DEFINE_NATIVE_FUNCTION(MathObject::expm1)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is one of NaN, +0𝔽, -0𝔽, or +∞𝔽, return n.
    if (number.is_nan() || number.as_double() == 0 || number.is_positive_infinity())
        return number;

    // 3. If n is -∞𝔽, return -1𝔽.
    if (number.is_negative_infinity())
        return Value(-1);

    // 4. Return an implementation-approximated Number value representing the result of subtracting 1 from the exponential function of ℝ(n).
    return Value(::expm1(number.as_double()));
}

// 21.3.2.16 Math.floor ( x ), https://tc39.es/ecma262/#sec-math.floor
ThrowCompletionOr<Value> MathObject::floor_impl(VM& vm, Value x)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if (!number.is_finite_number() || number.as_double() == 0)
        return number;

    // 3. If n < 1𝔽 and n > +0𝔽, return +0𝔽.
    // 4. If n is an integral Number, return n.
    // 5. Return the greatest (closest to +∞) integral Number value that is not greater than n.
    return Value(::floor(number.as_double()));
}

// 21.3.2.16 Math.floor ( x ), https://tc39.es/ecma262/#sec-math.floor
JS_DEFINE_NATIVE_FUNCTION(MathObject::floor)
{
    return floor_impl(vm, vm.argument(0));
}

// 21.3.2.17 Math.fround ( x ), https://tc39.es/ecma262/#sec-math.fround
JS_DEFINE_NATIVE_FUNCTION(MathObject::fround)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, return NaN.
    if (number.is_nan())
        return js_nan();

    // 3. If n is one of +0𝔽, -0𝔽, +∞𝔽, or -∞𝔽, return n.
    if (number.as_double() == 0 || number.is_infinity())
        return number;

    // 4. Let n32 be the result of converting n to a value in IEEE 754-2019 binary32 format using roundTiesToEven mode.
    // 5. Let n64 be the result of converting n32 to a value in IEEE 754-2019 binary64 format.
    // 6. Return the ECMAScript Number value corresponding to n64.
    return Value((float)number.as_double());
}

// 21.3.2.18 Math.hypot ( ...args ), https://tc39.es/ecma262/#sec-math.hypot
JS_DEFINE_NATIVE_FUNCTION(MathObject::hypot)
{
    // 1. Let coerced be a new empty List.
    Vector<Value> coerced;

    // 2. For each element arg of args, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        // a. Let n be ? ToNumber(arg).
        auto number = TRY(vm.argument(i).to_number(vm));

        // b. Append n to coerced.
        coerced.append(number);
    }

    // 3. For each element number of coerced, do
    for (auto& number : coerced) {
        // a. If number is either +∞𝔽 or -∞𝔽, return +∞𝔽.
        if (number.is_infinity())
            return js_infinity();
    }

    // 4. Let onlyZero be true.
    auto only_zero = true;

    double sum_of_squares = 0;

    // 5. For each element number of coerced, do
    for (auto& number : coerced) {
        // a. If number is NaN, return NaN.
        // OPTIMIZATION: For infinities, the result will be infinity with the same sign, so we can return early.
        if (number.is_nan() || number.is_infinity())
            return number;

        // b. If number is neither +0𝔽 nor -0𝔽, set onlyZero to false.
        if (number.as_double() != 0)
            only_zero = false;

        sum_of_squares += number.as_double() * number.as_double();
    }

    // 6. If onlyZero is true, return +0𝔽.
    if (only_zero)
        return Value(0);

    // 7. Return an implementation-approximated Number value representing the square root of the sum of squares of the mathematical values of the elements of coerced.
    return Value(::sqrt(sum_of_squares));
}

// 21.3.2.19 Math.imul ( x, y ), https://tc39.es/ecma262/#sec-math.imul
JS_DEFINE_NATIVE_FUNCTION(MathObject::imul)
{
    // 1. Let a be ℝ(? ToUint32(x)).
    auto a = TRY(vm.argument(0).to_u32(vm));

    // 2. Let b be ℝ(? ToUint32(y)).
    auto b = TRY(vm.argument(1).to_u32(vm));

    // 3. Let product be (a × b) modulo 2^32.
    // 4. If product ≥ 2^31, return 𝔽(product - 2^32); otherwise return 𝔽(product).
    return Value(static_cast<i32>(a * b));
}

// 21.3.2.20 Math.log ( x ), https://tc39.es/ecma262/#sec-math.log
ThrowCompletionOr<Value> MathObject::log_impl(VM& vm, Value x)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is NaN or n is +∞𝔽, return n.
    if (number.is_nan() || number.is_positive_infinity())
        return number;

    // 3. If n is 1𝔽, return +0𝔽.
    if (number.as_double() == 1.)
        return Value(0);

    // 4. If n is +0𝔽 or n is -0𝔽, return -∞𝔽.
    if (number.is_positive_zero() || number.is_negative_zero())
        return js_negative_infinity();

    // 5. If n < -0𝔽, return NaN.
    if (number.as_double() < -0.)
        return js_nan();

    // 6. Return an implementation-approximated Number value representing the result of the natural logarithm of ℝ(n).
    return Value(::log(number.as_double()));
}

// 21.3.2.20 Math.log ( x ), https://tc39.es/ecma262/#sec-math.log
JS_DEFINE_NATIVE_FUNCTION(MathObject::log)
{
    return log_impl(vm, vm.argument(0));
}

// 21.3.2.21 Math.log1p ( x ), https://tc39.es/ecma262/#sec-math.log1p
JS_DEFINE_NATIVE_FUNCTION(MathObject::log1p)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +0𝔽, n is -0𝔽, or n is +∞𝔽, return n.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero() || number.is_positive_infinity())
        return number;

    // 3. If n is -1𝔽, return -∞𝔽.
    if (number.as_double() == -1.)
        return js_negative_infinity();

    // 4. If n < -1𝔽, return NaN.
    if (number.as_double() < -1.)
        return js_nan();

    // 5. Return an implementation-approximated Number value representing the result of the natural logarithm of 1 + ℝ(n).
    return Value(::log1p(number.as_double()));
}

// 21.3.2.22 Math.log10 ( x ), https://tc39.es/ecma262/#sec-math.log10
JS_DEFINE_NATIVE_FUNCTION(MathObject::log10)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN or n is +∞𝔽, return n.
    if (number.is_nan() || number.is_positive_infinity())
        return number;

    // 3. If n is 1𝔽, return +0𝔽.
    if (number.as_double() == 1.)
        return Value(0);

    // 4. If n is +0𝔽 or n is -0𝔽, return -∞𝔽.
    if (number.is_positive_zero() || number.is_negative_zero())
        return js_negative_infinity();

    // 5. If n < -0𝔽, return NaN.
    if (number.as_double() < -0.)
        return js_nan();

    // 6. Return an implementation-approximated Number value representing the result of the base 10 logarithm of ℝ(n).
    return Value(::log10(number.as_double()));
}

// 21.3.2.23 Math.log2 ( x ), https://tc39.es/ecma262/#sec-math.log2
JS_DEFINE_NATIVE_FUNCTION(MathObject::log2)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN or n is +∞𝔽, return n.
    if (number.is_nan() || number.is_positive_infinity())
        return number;

    // 3. If n is 1𝔽, return +0𝔽.
    if (number.as_double() == 1.)
        return Value(0);

    // 4. If n is +0𝔽 or n is -0𝔽, return -∞𝔽.
    if (number.is_positive_zero() || number.is_negative_zero())
        return js_negative_infinity();

    // 5. If n < -0𝔽, return NaN.
    if (number.as_double() < -0.)
        return js_nan();

    // 6. Return an implementation-approximated Number value representing the result of the base 2 logarithm of ℝ(n).
    return Value(::log2(number.as_double()));
}

// 21.3.2.24 Math.max ( ...args ), https://tc39.es/ecma262/#sec-math.max
JS_DEFINE_NATIVE_FUNCTION(MathObject::max)
{
    // 1. Let coerced be a new empty List.
    Vector<Value> coerced;

    // 2. For each element arg of args, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        // a. Let n be ? ToNumber(arg).
        auto number = TRY(vm.argument(i).to_number(vm));

        // b. Append n to coerced.
        coerced.append(number);
    }

    // 3. Let highest be -∞𝔽.
    auto highest = js_negative_infinity();

    // 4. For each element number of coerced, do
    for (auto& number : coerced) {
        // a. If number is NaN, return NaN.
        if (number.is_nan())
            return js_nan();

        // b. If number is +0𝔽 and highest is -0𝔽, set highest to +0𝔽.
        // c. If number > highest, set highest to number.
        if ((number.is_positive_zero() && highest.is_negative_zero()) || number.as_double() > highest.as_double())
            highest = number;
    }

    // 5. Return highest.
    return highest;
}

// 21.3.2.25 Math.min ( ...args ), https://tc39.es/ecma262/#sec-math.min
JS_DEFINE_NATIVE_FUNCTION(MathObject::min)
{
    // 1. Let coerced be a new empty List.
    Vector<Value> coerced;

    // 2. For each element arg of args, do
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        // a. Let n be ? ToNumber(arg).
        auto number = TRY(vm.argument(i).to_number(vm));

        // b. Append n to coerced.
        coerced.append(number);
    }

    // 3. Let lowest be +∞𝔽.
    auto lowest = js_infinity();

    // 4. For each element number of coerced, do
    for (auto& number : coerced) {
        // a. If number is NaN, return NaN.
        if (number.is_nan())
            return js_nan();

        // b. If number is -0𝔽 and lowest is +0𝔽, set lowest to -0𝔽.
        // c. If number < lowest, set lowest to number.
        if ((number.is_negative_zero() && lowest.is_positive_zero()) || number.as_double() < lowest.as_double())
            lowest = number;
    }

    // 5. Return lowest.
    return lowest;
}

// 21.3.2.26 Math.pow ( base, exponent ), https://tc39.es/ecma262/#sec-math.pow
ThrowCompletionOr<Value> MathObject::pow_impl(VM& vm, Value base, Value exponent)
{
    // Set base to ? ToNumber(base).
    base = TRY(base.to_number(vm));

    // 2. Set exponent to ? ToNumber(exponent).
    exponent = TRY(exponent.to_number(vm));

    // 3. Return Number::exponentiate(base, exponent).
    return JS::exp(vm, base, exponent);
}

// 21.3.2.26 Math.pow ( base, exponent ), https://tc39.es/ecma262/#sec-math.pow
JS_DEFINE_NATIVE_FUNCTION(MathObject::pow)
{
    return pow_impl(vm, vm.argument(0), vm.argument(1));
}

// 21.3.2.27 Math.random ( ), https://tc39.es/ecma262/#sec-math.random
JS_DEFINE_NATIVE_FUNCTION(MathObject::random)
{
    // This function returns a Number value with positive sign, greater than or equal to +0𝔽 but strictly less than 1𝔽,
    // chosen randomly or pseudo randomly with approximately uniform distribution over that range, using an
    // implementation-defined algorithm or strategy.
    double r = (double)get_random<u32>() / (double)UINT32_MAX;
    return Value(r);
}

// 21.3.2.28 Math.round ( x ), https://tc39.es/ecma262/#sec-math.round
ThrowCompletionOr<Value> MathObject::round_impl(VM& vm, Value x)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is not finite or n is an integral Number, return n.
    if (!number.is_finite_number() || number.as_double() == ::trunc(number.as_double()))
        return number;

    // 3. If n < 0.5𝔽 and n > +0𝔽, return +0𝔽.
    // 4. If n < -0𝔽 and n ≥ -0.5𝔽, return -0𝔽.
    // 5. Return the integral Number closest to n, preferring the Number closer to +∞ in the case of a tie.
    double integer = ::ceil(number.as_double());
    if (integer - 0.5 > number.as_double())
        integer--;
    return Value(integer);
}

// 21.3.2.28 Math.round ( x ), https://tc39.es/ecma262/#sec-math.round
JS_DEFINE_NATIVE_FUNCTION(MathObject::round)
{
    return round_impl(vm, vm.argument(0));
}

// 21.3.2.29 Math.sign ( x ), https://tc39.es/ecma262/#sec-math.sign
JS_DEFINE_NATIVE_FUNCTION(MathObject::sign)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is one of NaN, +0𝔽, or -0𝔽, return n.
    if (number.is_nan() || number.as_double() == 0)
        return number;

    // 3. If n < -0𝔽, return -1𝔽.
    if (number.as_double() < 0)
        return Value(-1);

    // 4. Return 1𝔽.
    return Value(1);
}

// 21.3.2.30 Math.sin ( x ), https://tc39.es/ecma262/#sec-math.sin
JS_DEFINE_NATIVE_FUNCTION(MathObject::sin)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. If n is +∞𝔽 or n is -∞𝔽, return NaN.
    if (number.is_infinity())
        return js_nan();

    // 4. Return an implementation-approximated Number value representing the result of the sine of ℝ(n).
    return Value(::sin(number.as_double()));
}

// 21.3.2.31 Math.sinh ( x ), https://tc39.es/ecma262/#sec-math.sinh
JS_DEFINE_NATIVE_FUNCTION(MathObject::sinh)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if (!number.is_finite_number() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. Return an implementation-approximated Number value representing the result of the hyperbolic sine of ℝ(n).
    return Value(::sinh(number.as_double()));
}

// 21.3.2.32 Math.sqrt ( x ), https://tc39.es/ecma262/#sec-math.sqrt
ThrowCompletionOr<Value> MathObject::sqrt_impl(VM& vm, Value x)
{
    // Let n be ? ToNumber(x).
    auto number = TRY(x.to_number(vm));

    // 2. If n is one of NaN, +0𝔽, -0𝔽, or +∞𝔽, return n.
    if (number.is_nan() || number.as_double() == 0 || number.is_positive_infinity())
        return number;

    // 3. If n < -0𝔽, return NaN.
    if (number.as_double() < 0)
        return js_nan();

    // 4. Return an implementation-approximated Number value representing the result of the square root of ℝ(n).
    return Value(::sqrt(number.as_double()));
}

// 21.3.2.32 Math.sqrt ( x ), https://tc39.es/ecma262/#sec-math.sqrt
JS_DEFINE_NATIVE_FUNCTION(MathObject::sqrt)
{
    return sqrt_impl(vm, vm.argument(0));
}

// 21.3.2.33 Math.tan ( x ), https://tc39.es/ecma262/#sec-math.tan
JS_DEFINE_NATIVE_FUNCTION(MathObject::tan)
{
    // Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. If n is +∞𝔽, or n is -∞𝔽, return NaN.
    if (number.is_infinity())
        return js_nan();

    // 4. Return an implementation-approximated Number value representing the result of the tangent of ℝ(n).
    return Value(::tan(number.as_double()));
}

// 21.3.2.34 Math.tanh ( x ), https://tc39.es/ecma262/#sec-math.tanh
JS_DEFINE_NATIVE_FUNCTION(MathObject::tanh)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is NaN, n is +0𝔽, or n is -0𝔽, return n.
    if (number.is_nan() || number.is_positive_zero() || number.is_negative_zero())
        return number;

    // 3. If n is +∞𝔽, return 1𝔽.
    if (number.is_positive_infinity())
        return Value(1);

    // 4. If n is -∞𝔽, return -1𝔽.
    if (number.is_negative_infinity())
        return Value(-1);

    // 5. Return an implementation-approximated Number value representing the result of the hyperbolic tangent of ℝ(n).
    return Value(::tanh(number.as_double()));
}

// 21.3.2.35 Math.trunc ( x ), https://tc39.es/ecma262/#sec-math.trunc
JS_DEFINE_NATIVE_FUNCTION(MathObject::trunc)
{
    // 1. Let n be ? ToNumber(x).
    auto number = TRY(vm.argument(0).to_number(vm));

    // 2. If n is not finite or n is either +0𝔽 or -0𝔽, return n.
    if (number.is_nan() || number.is_infinity() || number.as_double() == 0)
        return number;

    // 3. If n < 1𝔽 and n > +0𝔽, return +0𝔽.
    // 4. If n < -0𝔽 and n > -1𝔽, return -0𝔽.
    // 5. Return the integral Number nearest n in the direction of +0𝔽.
    return Value(number.as_double() < 0
            ? ::ceil(number.as_double())
            : ::floor(number.as_double()));
}

}
