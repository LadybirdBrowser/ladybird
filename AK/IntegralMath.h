/*
 * Copyright (c) 2022, Leon Albrecht <leon2002.la@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Concepts.h>
#include <AK/Platform.h>
#include <AK/Types.h>

namespace AK {

template<Integral T>
constexpr T exp2(T exponent)
{
    return static_cast<T>(1) << exponent;
}

template<Integral T>
constexpr T log2(T x)
{
    return x ? (8 * sizeof(T) - 1) - count_leading_zeroes(static_cast<MakeUnsigned<T>>(x)) : 0;
}

template<Integral T>
constexpr T ceil_log2(T x)
{
    if (x <= 1)
        return 0;

    return AK::log2(x - 1) + 1;
}

template<Integral I>
constexpr I pow(I base, I exponent)
{
    // https://en.wikipedia.org/wiki/Exponentiation_by_squaring
    if (exponent < 0)
        return 0;
    if (exponent == 0)
        return 1;

    I res = 1;
    while (exponent > 0) {
        if (exponent & 1)
            res *= base;
        base *= base;
        exponent /= 2u;
    }
    return res;
}

template<auto base, Unsigned U = decltype(base)>
constexpr bool is_power_of(U x)
{
    if constexpr (base == 1)
        return x == 1;
    else if constexpr (base == 2)
        return is_power_of_two(x);

    if (base == 0 && x == 0)
        return true;
    if (base == 0 || x == 0)
        return false;

    while (x != 1) {
        if (x % base != 0)
            return false;
        x /= base;
    }
    return true;
}

template<Unsigned T>
constexpr T reinterpret_as_octal(T decimal)
{
    T result = 0;
    T n = 0;
    while (decimal > 0) {
        result += pow<T>(8, n++) * (decimal % 10);
        decimal /= 10;
    }
    return result;
}

template<Unsigned T>
constexpr T gcd(T x, T y)
{
    if (x == 0)
        return y;
    if (y == 0)
        return x;

    int shift = 0;
    while (((x | y) & 1) == 0) {
        x >>= 1;
        y >>= 1;
        shift++;
    }

    while (x != y) {
        if (x & 1) {
            if (y & 1) {
                if (x > y)
                    x -= y;
                else
                    y -= x;
            } else {
                y >>= 1;
            }
        } else {
            x >>= 1;
            if (y & 1) {
                if (x < y)
                    swap(x, y);
            }
        }
    }

    return x << shift;
}

template<Signed T>
constexpr T gcd(T x, T y)
{
    return gcd(static_cast<MakeUnsigned<T>>(abs(x)), static_cast<MakeUnsigned<T>>(abs(y)));
}

template<Unsigned T>
constexpr T lcm(T x, T y)
{
    if (x == 0 || y == 0)
        return 0;
    return x / gcd(x, y) * y;
}

template<Signed T>
constexpr T lcm(T x, T y)
{
    return lcm(static_cast<MakeUnsigned<T>>(abs(x)), static_cast<MakeUnsigned<T>>(abs(y)));
}

constexpr bool multiply_divide_would_overflow(u64 multiplicand, u64 multiplier, u64 divisor)
{
    VERIFY(divisor != 0);
    auto product = static_cast<unsigned __int128>(multiplicand) * multiplier;
    return static_cast<u64>(product >> 64) >= divisor;
}

constexpr u64 multiply_divide(u64 multiplicand, u64 multiplier, u64 divisor)
{
    VERIFY(!multiply_divide_would_overflow(multiplicand, multiplier, divisor));
    auto product = static_cast<unsigned __int128>(multiplicand) * multiplier;

#if ARCH(X86_64)
    // x86-64 divides the whole product in one instruction, which faults unless the quotient fits in 64 bits.
    if !consteval {
        u64 quotient = 0;
        u64 remainder = 0;
        asm("divq %[divisor]"
            : "=a"(quotient), "=d"(remainder)
            : [divisor] "r"(divisor), "a"(static_cast<u64>(product)), "d"(static_cast<u64>(product >> 64)));
        return quotient;
    }
#endif
    return static_cast<u64>(product / divisor);
}

}
