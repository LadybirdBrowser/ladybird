/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/NumericLimits.h>

namespace AK {

template<Integral T>
requires(Signed<T>)
constexpr T saturating_add(T a, T b)
{
    using U = MakeUnsigned<T>;
    T result;
    U overflowed = __builtin_add_overflow(a, b, &result);
    T saturated = static_cast<T>(static_cast<U>(NumericLimits<T>::max()) + (static_cast<U>(a) >> (sizeof(T) * 8 - 1)));
    U mask = -overflowed;
    return static_cast<T>((static_cast<U>(saturated) & mask) | (static_cast<U>(result) & ~mask));
}

template<Integral T>
requires(Unsigned<T>)
constexpr T saturating_add(T a, T b)
{
    T result = a + b;
    result |= -static_cast<T>(result < a);
    return result;
}

template<Integral T>
requires(Signed<T>)
constexpr T saturating_sub(T a, T b)
{
    using U = MakeUnsigned<T>;
    T result;
    U overflowed = __builtin_sub_overflow(a, b, &result);
    T saturated = static_cast<T>(static_cast<U>(NumericLimits<T>::max()) + (static_cast<U>(a) >> (sizeof(T) * 8 - 1)));
    U mask = -overflowed;
    return static_cast<T>((static_cast<U>(saturated) & mask) | (static_cast<U>(result) & ~mask));
}

template<Integral T>
requires(Unsigned<T>)
constexpr T saturating_sub(T a, T b)
{
    T result = a - b;
    result &= -static_cast<T>(result <= a);
    return result;
}

template<Integral T>
requires(Signed<T>)
constexpr T saturating_mul(T a, T b)
{
    using U = MakeUnsigned<T>;
    T result;
    U overflowed = __builtin_mul_overflow(a, b, &result);
    // Same signs → positive overflow → max. Different signs → negative overflow → min.
    T saturated = static_cast<T>(static_cast<U>(NumericLimits<T>::max()) + ((static_cast<U>(a) ^ static_cast<U>(b)) >> (sizeof(T) * 8 - 1)));
    U mask = -overflowed;
    return static_cast<T>((static_cast<U>(saturated) & mask) | (static_cast<U>(result) & ~mask));
}

template<Integral T>
requires(Unsigned<T>)
constexpr T saturating_mul(T a, T b)
{
    T result;
    if (__builtin_mul_overflow(a, b, &result))
        return NumericLimits<T>::max();
    return result;
}

}

#if USING_AK_GLOBALLY
using AK::saturating_add;
using AK::saturating_mul;
using AK::saturating_sub;
#endif
