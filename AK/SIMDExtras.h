/*
 * Copyright (c) 2021, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/SIMD.h>

// Functions returning vectors or accepting vector arguments have different calling conventions
// depending on whether the target architecture supports SSE or not. GCC generates warning "psabi"
// when compiling for non-SSE architectures. We disable this warning because these functions
// are static and should never be visible from outside the translation unit that includes this header.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"

namespace AK::SIMD {

// SIMD Vector Expansion

ALWAYS_INLINE static constexpr f32x4 expand4(float f)
{
    return f32x4 { f, f, f, f };
}

ALWAYS_INLINE static constexpr i32x4 expand4(i32 i)
{
    return i32x4 { i, i, i, i };
}

ALWAYS_INLINE static constexpr u32x4 expand4(u32 u)
{
    return u32x4 { u, u, u, u };
}

// Casting

template<typename TSrc>
ALWAYS_INLINE static u8x4 to_u8x4(TSrc v)
{
    return __builtin_convertvector(v, u8x4);
}

template<typename TSrc>
ALWAYS_INLINE static u16x4 to_u16x4(TSrc v)
{
    return __builtin_convertvector(v, u16x4);
}

template<typename TSrc>
ALWAYS_INLINE static u32x4 to_u32x4(TSrc v)
{
    return __builtin_convertvector(v, u32x4);
}

template<typename TSrc>
ALWAYS_INLINE static i32x4 to_i32x4(TSrc v)
{
    return __builtin_convertvector(v, i32x4);
}

template<typename TSrc>
ALWAYS_INLINE static f32x4 to_f32x4(TSrc v)
{
    return __builtin_convertvector(v, f32x4);
}

// Masking

ALWAYS_INLINE static i32 maskbits(i32x4 mask)
{
#if defined(__SSE__)
    return __builtin_ia32_movmskps((f32x4)mask);
#else
    return ((mask[0] & 0x80000000) >> 31) | ((mask[1] & 0x80000000) >> 30) | ((mask[2] & 0x80000000) >> 29) | ((mask[3] & 0x80000000) >> 28);
#endif
}

ALWAYS_INLINE static bool all(i32x4 mask)
{
    return maskbits(mask) == 15;
}

ALWAYS_INLINE static bool any(i32x4 mask)
{
    return maskbits(mask) != 0;
}

ALWAYS_INLINE static bool none(i32x4 mask)
{
    return maskbits(mask) == 0;
}

ALWAYS_INLINE static int maskcount(i32x4 mask)
{
    constexpr static int count_lut[16] { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
    return count_lut[maskbits(mask)];
}

// Load / Store

template<SIMDVector VectorType>
ALWAYS_INLINE static VectorType load_unaligned(void const* a)
{
    VectorType v;
    __builtin_memcpy(&v, a, sizeof(VectorType));
    return v;
}

template<SIMDVector VectorType>
ALWAYS_INLINE static void store_unaligned(void* a, VectorType const& v)
{
    // FIXME: Does this generate the right instructions?
    __builtin_memcpy(a, &v, sizeof(VectorType));
}

ALWAYS_INLINE static f32x4 load4(float const* a, float const* b, float const* c, float const* d)
{
    return f32x4 { *a, *b, *c, *d };
}

ALWAYS_INLINE static u32x4 load4(u32 const* a, u32 const* b, u32 const* c, u32 const* d)
{
    return u32x4 { *a, *b, *c, *d };
}

ALWAYS_INLINE static f32x4 load4_masked(float const* a, float const* b, float const* c, float const* d, i32x4 mask)
{
    int bits = maskbits(mask);
    return f32x4 {
        bits & 1 ? *a : 0.f,
        bits & 2 ? *b : 0.f,
        bits & 4 ? *c : 0.f,
        bits & 8 ? *d : 0.f,
    };
}

ALWAYS_INLINE static i32x4 load4_masked(u8 const* a, u8 const* b, u8 const* c, u8 const* d, i32x4 mask)
{
    int bits = maskbits(mask);
    return i32x4 {
        bits & 1 ? *a : 0,
        bits & 2 ? *b : 0,
        bits & 4 ? *c : 0,
        bits & 8 ? *d : 0,
    };
}

ALWAYS_INLINE static u32x4 load4_masked(u32 const* a, u32 const* b, u32 const* c, u32 const* d, i32x4 mask)
{
    int bits = maskbits(mask);
    return u32x4 {
        bits & 1 ? *a : 0u,
        bits & 2 ? *b : 0u,
        bits & 4 ? *c : 0u,
        bits & 8 ? *d : 0u,
    };
}

template<typename VectorType, typename UnderlyingType = decltype(declval<VectorType>()[0])>
ALWAYS_INLINE static void store4(VectorType v, UnderlyingType* a, UnderlyingType* b, UnderlyingType* c, UnderlyingType* d)
{
    *a = v[0];
    *b = v[1];
    *c = v[2];
    *d = v[3];
}

template<typename VectorType, typename UnderlyingType = decltype(declval<VectorType>()[0])>
ALWAYS_INLINE static void store4_masked(VectorType v, UnderlyingType* a, UnderlyingType* b, UnderlyingType* c, UnderlyingType* d, i32x4 mask)
{
    int bits = maskbits(mask);
    if (bits & 1)
        *a = v[0];
    if (bits & 2)
        *b = v[1];
    if (bits & 4)
        *c = v[2];
    if (bits & 8)
        *d = v[3];
}

// Shuffle

template<OneOf<i8x16, u8x16> T>
ALWAYS_INLINE static T shuffle(T a, T control)
{
    // FIXME: This is probably not the fastest way to do this.
    return T {
        a[control[0] & 0xf],
        a[control[1] & 0xf],
        a[control[2] & 0xf],
        a[control[3] & 0xf],
        a[control[4] & 0xf],
        a[control[5] & 0xf],
        a[control[6] & 0xf],
        a[control[7] & 0xf],
        a[control[8] & 0xf],
        a[control[9] & 0xf],
        a[control[10] & 0xf],
        a[control[11] & 0xf],
        a[control[12] & 0xf],
        a[control[13] & 0xf],
        a[control[14] & 0xf],
        a[control[15] & 0xf],
    };
}
}

#pragma GCC diagnostic pop
