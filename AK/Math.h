/*
 * Copyright (c) 2021, Leon Albrecht <leon2002.la@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BuiltinWrappers.h>
#include <AK/Concepts.h>
#include <AK/Endian.h>
#include <AK/FloatingPoint.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtraDetails.h>
#include <AK/Types.h>

namespace AK {

template<FloatingPoint T>
constexpr T NaN = __builtin_nan("");
template<FloatingPoint T>
constexpr T Infinity = __builtin_huge_vall();
template<FloatingPoint T>
constexpr T Pi = 3.141592653589793238462643383279502884L;
template<FloatingPoint T>
constexpr T E = 2.718281828459045235360287471352662498L;
template<FloatingPoint T>
constexpr T Sqrt2 = 1.414213562373095048801688724209698079L;
template<FloatingPoint T>
constexpr T Sqrt1_2 = 0.707106781186547524400844362104849039L;

template<FloatingPoint T>
constexpr T to_radians(T degrees)
{
    return degrees * AK::Pi<T> / 180;
}

template<FloatingPoint T>
constexpr T to_degrees(T radians)
{
    return radians * 180 / AK::Pi<T>;
}

// NB: These all forward to the compiler builtins, which lower to the host's libm (or to single instructions where the
//     target has them), and are constant-evaluated by the compiler where it supports that.
#define AK_MATH_BUILTIN(function, args...)    \
    if constexpr (IsSame<T, long double>)     \
        return __builtin_##function##l(args); \
    else if constexpr (IsSame<T, double>)     \
        return __builtin_##function(args);    \
    else if constexpr (IsSame<T, float>)      \
        return __builtin_##function##f(args); \
    else                                      \
        static_assert(DependentFalse<T>, "There is no builtin for this floating point type");

#define AK_MATH_UNARY_FUNCTION(function) \
    template<FloatingPoint T>            \
    constexpr T function(T x)            \
    {                                    \
        AK_MATH_BUILTIN(function, x);    \
    }

#define AK_MATH_BINARY_FUNCTION(function) \
    template<FloatingPoint T>             \
    constexpr T function(T x, T y)        \
    {                                     \
        AK_MATH_BUILTIN(function, x, y);  \
    }

AK_MATH_UNARY_FUNCTION(fabs)
AK_MATH_UNARY_FUNCTION(rint)
AK_MATH_UNARY_FUNCTION(round)
AK_MATH_UNARY_FUNCTION(sqrt)
AK_MATH_UNARY_FUNCTION(cbrt)
AK_MATH_UNARY_FUNCTION(sin)
AK_MATH_UNARY_FUNCTION(cos)
AK_MATH_UNARY_FUNCTION(tan)
AK_MATH_UNARY_FUNCTION(asin)
AK_MATH_UNARY_FUNCTION(acos)
AK_MATH_UNARY_FUNCTION(atan)
AK_MATH_UNARY_FUNCTION(sinh)
AK_MATH_UNARY_FUNCTION(cosh)
AK_MATH_UNARY_FUNCTION(tanh)
AK_MATH_UNARY_FUNCTION(asinh)
AK_MATH_UNARY_FUNCTION(acosh)
AK_MATH_UNARY_FUNCTION(atanh)
AK_MATH_UNARY_FUNCTION(exp)
AK_MATH_UNARY_FUNCTION(exp2)
AK_MATH_UNARY_FUNCTION(log)
AK_MATH_UNARY_FUNCTION(log2)
AK_MATH_UNARY_FUNCTION(log10)

AK_MATH_BINARY_FUNCTION(fmod)
AK_MATH_BINARY_FUNCTION(remainder)
AK_MATH_BINARY_FUNCTION(hypot)
AK_MATH_BINARY_FUNCTION(atan2)
AK_MATH_BINARY_FUNCTION(pow)

// NB: Not every compiler constant-evaluates these builtins, so they get a fallback for constant evaluation.
template<FloatingPoint T>
constexpr T ceil(T num)
{
    if consteval {
        if (!(num > static_cast<T>(NumericLimits<i64>::min()) && num < static_cast<T>(NumericLimits<i64>::max())))
            return num;
        return (static_cast<T>(static_cast<i64>(num)) == num)
            ? static_cast<i64>(num)
            : static_cast<i64>(num) + ((num > 0) ? 1 : 0);
    }
    AK_MATH_BUILTIN(ceil, num);
}

template<FloatingPoint T>
constexpr T floor(T num)
{
    if consteval {
        if (!(num > static_cast<T>(NumericLimits<i64>::min()) && num < static_cast<T>(NumericLimits<i64>::max())))
            return num;
        return (static_cast<T>(static_cast<i64>(num)) == num)
            ? static_cast<i64>(num)
            : static_cast<i64>(num) - ((num > 0) ? 0 : 1);
    }
    AK_MATH_BUILTIN(floor, num);
}

template<FloatingPoint T>
constexpr T trunc(T num)
{
    if consteval {
        if (!(num > static_cast<T>(NumericLimits<i64>::min()) && num < static_cast<T>(NumericLimits<i64>::max())))
            return num;
        return static_cast<T>(static_cast<i64>(num));
    }
    AK_MATH_BUILTIN(trunc, num);
}

template<FloatingPoint T>
constexpr void sincos(T angle, T& sin_value, T& cos_value)
{
    sin_value = sin(angle);
    cos_value = cos(angle);
}

// Rounds to the nearest integer, with ties going to even (in the default floating point environment). Values outside of
// the range of I saturate to its limits, and NaN becomes zero.
template<Integral I, FloatingPoint T>
ALWAYS_INLINE I round_to(T value)
{
    if (__builtin_isnan(value))
        return 0;
    T rounded = rint(value);
    // NB: The maximum of I can round up when converted to T (to 2^63 for i64), so a value equal to it may already be
    //     out of range.
    if (rounded >= static_cast<T>(NumericLimits<I>::max()))
        return NumericLimits<I>::max();
    if (rounded <= static_cast<T>(NumericLimits<I>::min()))
        return NumericLimits<I>::min();
    return static_cast<I>(rounded);
}

template<Integral I, typename T>
constexpr I clamp_to(T value)
{
    constexpr auto max = static_cast<T>(NumericLimits<I>::max());
    if constexpr (max > 0) {
        if (value >= static_cast<T>(NumericLimits<I>::max()))
            return NumericLimits<I>::max();
    }

    constexpr auto min = static_cast<T>(NumericLimits<I>::min());
    if constexpr (min <= 0) {
        if (value <= static_cast<T>(NumericLimits<I>::min()))
            return NumericLimits<I>::min();
    }

    if constexpr (IsFloatingPoint<T>)
        return round_to<I>(value);

    return value;
}

#undef AK_MATH_BINARY_FUNCTION
#undef AK_MATH_UNARY_FUNCTION
#undef AK_MATH_BUILTIN

}

#if USING_AK_GLOBALLY
using AK::round_to;
#endif
