/*
 * Copyright (c) 2021, Leon Albrecht <leon2002.la@gmail.com>.
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtraDetails.h>

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
constexpr T L2_10 = 3.321928094887362347870319429489390175864L;
template<FloatingPoint T>
constexpr T L2_E = 1.442695040888963407359924681001892137L;

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

#define BUILTIN_SPECIALIZE(function, args...) \
    if constexpr (IsSame<T, long double>)     \
        return __builtin_##function##l(args); \
    if constexpr (IsSame<T, double>)          \
        return __builtin_##function(args);    \
    if constexpr (IsSame<T, float>)           \
        return __builtin_##function##f(args);

template<FloatingPoint T>
constexpr T fabs(T x)
{
    BUILTIN_SPECIALIZE(fabs, x);
}

namespace Rounding {

template<FloatingPoint T>
constexpr T ceil(T num)
{
    BUILTIN_SPECIALIZE(ceil, num);
}

template<FloatingPoint T>
constexpr T floor(T num)
{
    BUILTIN_SPECIALIZE(floor, num);
}

template<FloatingPoint T>
constexpr T trunc(T num)
{
    BUILTIN_SPECIALIZE(trunc, num);
}

template<FloatingPoint T>
constexpr T rint(T x)
{
    BUILTIN_SPECIALIZE(rint, x);
}

template<FloatingPoint T>
constexpr T round(T x)
{
    BUILTIN_SPECIALIZE(round, x);
}

template<Integral I, FloatingPoint P>
ALWAYS_INLINE I round_to(P value)
{
    if constexpr (IsSame<P, long double>)
        return static_cast<I>(__builtin_llrintl(value));
    if constexpr (IsSame<P, double>)
        return static_cast<I>(__builtin_llrint(value));
    if constexpr (IsSame<P, float>)
        return static_cast<I>(__builtin_llrintf(value));
}

}

using Rounding::ceil;
using Rounding::floor;
using Rounding::rint;
using Rounding::round;
using Rounding::round_to;
using Rounding::trunc;

namespace Division {

template<FloatingPoint T>
constexpr T fmod(T x, T y)
{
    BUILTIN_SPECIALIZE(fmod, x, y);
}

template<FloatingPoint T>
constexpr T remainder(T x, T y)
{
    BUILTIN_SPECIALIZE(remainder, x, y);
}

}

using Division::fmod;
using Division::remainder;

template<FloatingPoint T>
constexpr T sqrt(T x)
{
    BUILTIN_SPECIALIZE(sqrt, x);
}

template<FloatingPoint T>
constexpr T cbrt(T x)
{
    BUILTIN_SPECIALIZE(cbrt, x);
}

namespace Trigonometry {

template<FloatingPoint T>
constexpr T hypot(T x, T y)
{
    BUILTIN_SPECIALIZE(hypot, x, y);
}

template<FloatingPoint T>
constexpr T sin(T angle)
{
    BUILTIN_SPECIALIZE(sin, angle);
}

template<FloatingPoint T>
constexpr T cos(T angle)
{
    BUILTIN_SPECIALIZE(cos, angle);
}

template<FloatingPoint T>
constexpr void sincos(T angle, T& sin_val, T& cos_val)
{
    if (is_constant_evaluated()) {
        sin_val = sin(angle);
        cos_val = cos(angle);
        return;
    }
#if ARCH(X86_64)
    asm(
        "fsincos"
        : "=t"(cos_val), "=u"(sin_val)
        : "0"(angle));
#else
    sin_val = sin(angle);
    cos_val = cos(angle);
#endif
}

template<FloatingPoint T>
constexpr T tan(T angle)
{
    BUILTIN_SPECIALIZE(tan, angle);
}

template<FloatingPoint T>
constexpr T atan(T value)
{
    BUILTIN_SPECIALIZE(atan, value);
}

template<FloatingPoint T>
constexpr T asin(T x)
{
    BUILTIN_SPECIALIZE(asin, x);
}

template<FloatingPoint T>
constexpr T acos(T value)
{
    BUILTIN_SPECIALIZE(acos, value);
}

template<FloatingPoint T>
constexpr T atan2(T y, T x)
{
    BUILTIN_SPECIALIZE(atan2, y, x);
}

}

using Trigonometry::acos;
using Trigonometry::asin;
using Trigonometry::atan;
using Trigonometry::atan2;
using Trigonometry::cos;
using Trigonometry::hypot;
using Trigonometry::sin;
using Trigonometry::sincos;
using Trigonometry::tan;

namespace Exponentials {

template<FloatingPoint T>
constexpr T log2(T x)
{
    BUILTIN_SPECIALIZE(log2, x);
}

template<FloatingPoint T>
constexpr T log(T x)
{
    BUILTIN_SPECIALIZE(log, x);
}

template<FloatingPoint T>
constexpr T log10(T x)
{
    BUILTIN_SPECIALIZE(log10, x);
}

template<FloatingPoint T>
constexpr T exp(T exponent)
{
    BUILTIN_SPECIALIZE(exp, exponent);
}

template<FloatingPoint T>
constexpr T exp2(T exponent)
{
    BUILTIN_SPECIALIZE(exp2, exponent);
}

}

using Exponentials::exp;
using Exponentials::exp2;
using Exponentials::log;
using Exponentials::log10;
using Exponentials::log2;

namespace Hyperbolic {

template<FloatingPoint T>
constexpr T sinh(T x)
{
    BUILTIN_SPECIALIZE(sinh, x);
}

template<FloatingPoint T>
constexpr T cosh(T x)
{
    BUILTIN_SPECIALIZE(cosh, x);
}

template<FloatingPoint T>
constexpr T tanh(T x)
{
    BUILTIN_SPECIALIZE(tanh, x);
}

template<FloatingPoint T>
constexpr T asinh(T x)
{
    BUILTIN_SPECIALIZE(asinh, x);
}

template<FloatingPoint T>
constexpr T acosh(T x)
{
    BUILTIN_SPECIALIZE(acosh, x);
}

template<FloatingPoint T>
constexpr T atanh(T x)
{
    BUILTIN_SPECIALIZE(atanh, x);
}

}

using Hyperbolic::acosh;
using Hyperbolic::asinh;
using Hyperbolic::atanh;
using Hyperbolic::cosh;
using Hyperbolic::sinh;
using Hyperbolic::tanh;

template<FloatingPoint T>
constexpr T pow(T x, T y)
{
    BUILTIN_SPECIALIZE(pow, x, y);
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

#undef BUILTIN_SPECIALIZE

}

#if USING_AK_GLOBALLY
using AK::round_to;
#endif
