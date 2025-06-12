/*
 * Copyright (c) 2021, Nick Johnson <sylvyrfysh@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Concepts.h"

namespace AK {

template<Unsigned IntType>
inline constexpr int popcount(IntType value)
{
#if defined(AK_COMPILER_CLANG) || defined(AK_COMPILER_GCC)
    static_assert(sizeof(IntType) <= sizeof(unsigned long long));
    if constexpr (sizeof(IntType) <= sizeof(unsigned int))
        return __builtin_popcount(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long))
        return __builtin_popcountl(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long long))
        return __builtin_popcountll(value);
    VERIFY_NOT_REACHED();
#else
    int ones = 0;
    for (size_t i = 0; i < 8 * sizeof(IntType); ++i) {
        if ((value >> i) & 1) {
            ++ones;
        }
    }
    return ones;
#endif
}

// The function will return the number of trailing zeroes in the type. If
// the given number if zero, this function may contain undefined
// behavior, or it may return the number of bits in the number. If
// this function can be called with zero, the use of
// count_trailing_zeroes_safe is preferred.
template<Unsigned IntType>
inline constexpr int count_trailing_zeroes(IntType value)
{
#if defined(AK_COMPILER_CLANG) || defined(AK_COMPILER_GCC)
    static_assert(sizeof(IntType) <= sizeof(unsigned long long));
    if constexpr (sizeof(IntType) <= sizeof(unsigned int))
        return __builtin_ctz(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long))
        return __builtin_ctzl(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long long))
        return __builtin_ctzll(value);
    VERIFY_NOT_REACHED();
#else
    for (size_t i = 0; i < 8 * sizeof(IntType); ++i) {
        if ((value >> i) & 1) {
            return i;
        }
    }
    return 8 * sizeof(IntType);
#endif
}

// The function will return the number of trailing zeroes in the type. If
// the given number is zero, this function will return the number of bits
// bits in the IntType.
template<Unsigned IntType>
inline constexpr int count_trailing_zeroes_safe(IntType value)
{
    if (value == 0)
        return 8 * sizeof(IntType);
    return count_trailing_zeroes(value);
}

// The function will return the number of leading zeroes in the type. If
// the given number if zero, this function may contain undefined
// behavior, or it may return the number of bits in the number. If
// this function can be called with zero, the use of
// count_leading_zeroes_safe is preferred.
template<Unsigned IntType>
inline constexpr int count_leading_zeroes(IntType value)
{
#if defined(AK_COMPILER_CLANG) || defined(AK_COMPILER_GCC)
    static_assert(sizeof(IntType) <= sizeof(unsigned long long));
    if constexpr (sizeof(IntType) <= sizeof(unsigned int))
        return __builtin_clz(value) - (32 - (8 * sizeof(IntType)));
    if constexpr (sizeof(IntType) == sizeof(unsigned long))
        return __builtin_clzl(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long long))
        return __builtin_clzll(value);
    VERIFY_NOT_REACHED();
#else
    // Wrap around, catch going past zero by noticing that i is greater than the number of bits in the number
    for (size_t i = (8 * sizeof(IntType)) - 1; i < 8 * sizeof(IntType); --i) {
        if ((value >> i) & 1) {
            return i;
        }
    }
    return 8 * sizeof(IntType);
#endif
}

#ifdef __SIZEOF_INT128__
// This is required for math.cpp internal_scalbn
inline constexpr int count_leading_zeroes(unsigned __int128 value)
{
#    if defined(AK_COMPILER_CLANG) || defined(AK_COMPILER_GCC)
    return (value > __UINT64_MAX__) ? __builtin_clzll(value >> 64) : 64 + __builtin_clzll(value);
#    else
    unsigned __int128 mask = (unsigned __int128)1 << 127;
    int ret = 0;
    while ((value & mask) == 0) {
        ++ret;
        mask >>= 1;
    }
    return ret;
#    endif
}
#endif

// The function will return the number of leading zeroes in the type. If
// the given number is zero, this function will return the number of bits
// in the IntType.
template<Unsigned IntType>
inline constexpr int count_leading_zeroes_safe(IntType value)
{
    if (value == 0)
        return 8 * sizeof(IntType);
    return count_leading_zeroes(value);
}

// Returns one plus the index of the least significant 1-bit of x, or if x is
// zero, returns zero. (See __builtin_ffs.)
//
// For numbers above zero, bit_scan_forward(n) == count_trailing_zeroes(n) + 1.
template<Integral IntType>
inline constexpr int bit_scan_forward(IntType value)
{
#if defined(AK_COMPILER_CLANG) || (defined(AK_COMPILER_GCC) && (!ARCH(RISCV64) || defined(__riscv_zbb)))
    static_assert(sizeof(IntType) <= sizeof(unsigned long long));
    if constexpr (sizeof(IntType) <= sizeof(unsigned int))
        return __builtin_ffs(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long))
        return __builtin_ffsl(value);
    if constexpr (sizeof(IntType) == sizeof(unsigned long long))
        return __builtin_ffsll(value);
    VERIFY_NOT_REACHED();
#else
    if (value == 0)
        return 0;
    return 1 + count_trailing_zeroes(static_cast<MakeUnsigned<IntType>>(value));
#endif
}

// Counts the minimum number of bits required to represent the value (i.e. ignoring leading null bits).
template<Unsigned IntType>
inline constexpr size_t count_required_bits(IntType value)
{
    if (value == 0)
        return 1;

    return 8 * sizeof(value) - count_leading_zeroes(value);
}

}

#if USING_AK_GLOBALLY
using AK::bit_scan_forward;
using AK::count_leading_zeroes;
using AK::count_leading_zeroes_safe;
using AK::count_required_bits;
using AK::count_trailing_zeroes;
using AK::count_trailing_zeroes_safe;
using AK::popcount;
#endif
