/*
 * Copyright (c) 2021, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
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
namespace Detail {

template<SIMDVector T, SIMDVector Control, size_t... Idx>
ALWAYS_INLINE static T shuffle_impl(T a, Control control, IndexSequence<Idx...>)
{
    // FIXME: Maybe make the VERIFYs optional, eg on SIMD-DEBUG, to avoid the overhead in performance oriented users, like LibWasm::SIMD
    // Note: - instead of _ to make the linter happy, as SIMD-DEBUG does not (yet) exist
    constexpr Conditional<IsSigned<ElementOf<Control>>, ssize_t, size_t> N = vector_length<T>;
    // If you hit this verify and want a 0 in these cases instead, use shuffle_or_0
    (([control] { VERIFY(control[Idx] < N); })(), ...);

    // __builtin_shuffle is only available with GCC, and has quite good codegen
    if constexpr (__has_builtin(__builtin_shuffle))
        return __builtin_shuffle(a, control);

    return T {
        a[control[Idx]]...
    };
}

// FIXME: AppleClang somehow unconditionally executes the `a[control[Idx]]` path,
//        even if its in the false branch of the ternary
//        This leads to a presumably out of bounds access, which is UB
//        Reenable the sanitizer once this is fixed
//        As a side note UBsan makes a total mess of the codegen anyway
template<SIMDVector T, SIMDVector Control, size_t... Idx>
#ifdef AK_COMPILER_CLANG
[[clang::no_sanitize("undefined")]]
#endif
ALWAYS_INLINE static T shuffle_or_0_impl(T a, Control control, IndexSequence<Idx...>)
{
    constexpr Conditional<IsSigned<ElementOf<Control>>, ssize_t, size_t> N = vector_length<T>;
    using E = ElementOf<T>;

    if constexpr (__has_builtin(__builtin_shuffle)) {
        auto vector = __builtin_shuffle(a, control);
        for (size_t i = 0; i < N; ++i)
            vector[i] = control[i] < 0 || control[i] >= N ? 0 : vector[i];
        return vector;
    }
    // 1. Set all out of bounds values to ~0
    // Note: This is done so that  the optimization mentioned down below works
    // Note: Vector compares result in bitmasks, aka all 1s or all 0s per element
    control |= ~((control >= 0) & (control < N));
    // 2. Selectively set out of bounds values to 0
    // Note: Clang successfully optimizes this to a few instructions on x86-ssse3, GCC does not
    //       Vector Optimizations/Instruction-Selection on ArmV8 seem to not be as powerful as of Clang18
    // FIXME: We could recreate the bit mask Clang uses for the select for u32 and u16
    //        control = control * explode_byte(sizeof(E)) + 0x03020100;
    //        return (T)shuffle_unchecked(Bytes(a), Bytes(control));
    // Note: On x86-ssse3, `pshufb` inserts a zero if the control byte has the highest bit set
    //       On ArmV8, `tbl` inserts a zero if the control byte is out of bounds in general
    //       On RiscV `vrgather.vv` inserts a 0 if the control index is out of bounds
    //       and is more powerful than the other two as it is able to use bigger item widths than a byte
    // Note: For u64x2 Clang seems to always unroll the compare instead of doing the fancy `phufb`

    return T {
        ((E)(control[Idx] != ~0 ? a[control[Idx]] : 0))...
    };
}

template<SIMDVector T, size_t... Idx>
ALWAYS_INLINE static T item_reverse_impl(T a, IndexSequence<Idx...>)
{
    constexpr size_t N = vector_length<T>;
    return __builtin_shufflevector(a, a, N - 1 - Idx...);
}

template<SIMDVector T, size_t... Idx>
ALWAYS_INLINE static T byte_reverse_impl(T a, IndexSequence<Idx...>)
{
    static_assert(sizeof...(Idx) == sizeof(T));
    constexpr size_t N = sizeof(T);
    // FIXME: GCC silently ignores the dependent vector_size attribute, this seems to be a bug
    //        https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68703
    //        Hence this giant conditional
    using BytesVector = Conditional<sizeof(T) == 2, u8x2, Conditional<sizeof(T) == 4, u8x4, Conditional<sizeof(T) == 8, u8x8, Conditional<sizeof(T) == 16, u8x16, Conditional<sizeof(T) == 32, u8x32, void>>>>>;
    static_assert(sizeof(BytesVector) == sizeof(T));
    return bit_cast<T>(
        __builtin_shufflevector(
            bit_cast<BytesVector>(a),
            bit_cast<BytesVector>(a),
            N - 1 - Idx...));
}

template<SIMDVector T, size_t... Idx>
ALWAYS_INLINE static T elementwise_byte_reverse_impl(T a, IndexSequence<Idx...>)
{
    static_assert(sizeof...(Idx) == vector_length<T>);
    using Element = ElementOf<T>;
    if constexpr (sizeof(Element) == 1) {
        return a;
    } else if constexpr (sizeof(Element) == 2) {
        return T {
            static_cast<Element>(__builtin_bswap16(static_cast<u16>(a[Idx])))...
        };
    } else if constexpr (sizeof(Element) == 4) {
        return T {
            static_cast<Element>(__builtin_bswap32(static_cast<u32>(a[Idx])))...
        };
    } else if constexpr (sizeof(Element) == 8) {
        return T {
            static_cast<Element>(__builtin_bswap64(static_cast<u64>(a[Idx])))...
        };
    } else {
        static_assert(DependentFalse<T>);
    }
}

}

// FIXME: Shuffles only work with integral types for now
template<SIMDVector T>
ALWAYS_INLINE static T shuffle(T a, IndexVectorFor<T> control)
{
    return Detail::shuffle_impl(a, control, MakeIndexSequence<vector_length<T>>());
}

template<SIMDVector T>
ALWAYS_INLINE static T shuffle_or_0(T a, IndexVectorFor<T> control)
{
    return Detail::shuffle_or_0_impl(a, control, MakeIndexSequence<vector_length<T>>());
}

template<SIMDVector T>
ALWAYS_INLINE static T item_reverse(T a)
{
    return Detail::item_reverse_impl(a, MakeIndexSequence<vector_length<T>>());
}

template<SIMDVector T>
ALWAYS_INLINE static T byte_reverse(T a)
{
    return Detail::byte_reverse_impl(a, MakeIndexSequence<sizeof(T)>());
}

template<SIMDVector T>
ALWAYS_INLINE static T elementwise_byte_reverse(T a)
{
    return Detail::elementwise_byte_reverse_impl(a, MakeIndexSequence<vector_length<T>>());
}

}

#pragma GCC diagnostic pop
