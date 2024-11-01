/*
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace AK {

template<typename T>
union FloatExtractor;

#ifdef AK_HAS_FLOAT_128
template<>
union FloatExtractor<f128> {
    using ComponentType = unsigned __int128;
    static constexpr int mantissa_bits = 112;
    static constexpr ComponentType mantissa_max = (((ComponentType)1) << 112) - 1;
    static constexpr int exponent_bias = 16383;
    static constexpr int exponent_bits = 15;
    static constexpr unsigned exponent_max = 32767;
    struct [[gnu::packed]] {
#    if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        ComponentType sign : 1;
        ComponentType exponent : 15;
        ComponentType mantissa : 112;
#    else
        ComponentType mantissa : 112;
        ComponentType exponent : 15;
        ComponentType sign : 1;
#    endif
    };
    f128 d;
};
// Validate that f128 and the FloatExtractor union are 128 bits.
static_assert(AssertSize<f128, 16>());
static_assert(AssertSize<FloatExtractor<f128>, sizeof(f128)>());
#endif

#ifdef AK_HAS_FLOAT_80
template<>
union FloatExtractor<f80> {
    using ComponentType = unsigned long long;
    static constexpr int mantissa_bits = 64;
    static constexpr ComponentType mantissa_max = ~0ull;
    static constexpr int exponent_bias = 16383;
    static constexpr int exponent_bits = 15;
    static constexpr unsigned exponent_max = 32767;
    struct [[gnu::packed]] {
        // This is technically wrong: Extended floating point values really only have 63 bits of mantissa
        // and an "integer bit" that behaves in various strange, unintuitive and non-IEEE-754 ways.
        // However, since all bit-fiddling float code assumes IEEE floats, it cannot handle this properly.
        // If we pretend that 80-bit floats are IEEE floats with 64-bit mantissas, almost everything works correctly
        // and we just need a few special cases.
#    if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        ComponentType sign : 1;
        ComponentType exponent : 15;
        ComponentType mantissa : 64;
#    else
        ComponentType mantissa : 64;
        ComponentType exponent : 15;
        ComponentType sign : 1;
#    endif
    };
    f80 d;
};
static_assert(AssertSize<FloatExtractor<f80>, sizeof(f80)>());
#endif

template<>
union FloatExtractor<f64> {
    using ComponentType = unsigned long long;
    static constexpr int mantissa_bits = 52;
    static constexpr ComponentType mantissa_max = (1ull << 52) - 1;
    static constexpr int exponent_bias = 1023;
    static constexpr int exponent_bits = 11;
    static constexpr unsigned exponent_max = 2047;
    struct [[gnu::packed]] {
        // FIXME: These types have to all be the same, otherwise this struct
        //        goes from being a bitfield describing the layout of an f64
        //        into being a multibyte mess on windows.
        //        Technically, '-mno-ms-bitfields' is supposed to disable this
        //        very intuitive and portable behaviour on windows, but it doesn't
        //        work with the msvc ABI.
        //        See <https://github.com/llvm/llvm-project/issues/24757>
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        ComponentType sign : 1;
        ComponentType exponent : 11;
        ComponentType mantissa : 52;
#else
        ComponentType mantissa : 52;
        ComponentType exponent : 11;
        ComponentType sign : 1;
#endif
    };
    f64 d;
};
static_assert(AssertSize<FloatExtractor<f64>, sizeof(f64)>());

template<>
union FloatExtractor<f32> {
    using ComponentType = unsigned;
    static constexpr int mantissa_bits = 23;
    static constexpr ComponentType mantissa_max = (1 << 23) - 1;
    static constexpr int exponent_bias = 127;
    static constexpr int exponent_bits = 8;
    static constexpr ComponentType exponent_max = 255;
    struct [[gnu::packed]] {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        ComponentType sign : 1;
        ComponentType exponent : 8;
        ComponentType mantissa : 23;
#else
        ComponentType mantissa : 23;
        ComponentType exponent : 8;
        ComponentType sign : 1;
#endif
    };
    f32 d;
};
static_assert(AssertSize<FloatExtractor<f32>, sizeof(f32)>());

}

#if USING_AK_GLOBALLY
using AK::FloatExtractor;
#endif
