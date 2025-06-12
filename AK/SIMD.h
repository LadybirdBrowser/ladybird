/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK::SIMD {

using i8x2 = i8 __attribute__((vector_size(2)));
using i8x4 = i8 __attribute__((vector_size(4)));
using i8x8 = i8 __attribute__((vector_size(8)));
using i8x16 = i8 __attribute__((vector_size(16)));
using i8x32 = i8 __attribute__((vector_size(32)));

using i16x2 = i16 __attribute__((vector_size(4)));
using i16x4 = i16 __attribute__((vector_size(8)));
using i16x8 = i16 __attribute__((vector_size(16)));
using i16x16 = i16 __attribute__((vector_size(32)));

// the asm intrinsics demand chars as  the 8-bit type, and do not allow
// (un-)signed ones to be used
using c8x2 = char __attribute__((vector_size(2)));
using c8x4 = char __attribute__((vector_size(4)));
using c8x8 = char __attribute__((vector_size(8)));
using c8x16 = char __attribute__((vector_size(16)));
using c8x32 = char __attribute__((vector_size(32)));

using i32x2 = i32 __attribute__((vector_size(8)));
using i32x4 = i32 __attribute__((vector_size(16)));
using i32x8 = i32 __attribute__((vector_size(32)));

using i64x2 = i64 __attribute__((vector_size(16)));
using i64x4 = i64 __attribute__((vector_size(32)));

using u8x2 = u8 __attribute__((vector_size(2)));
using u8x4 = u8 __attribute__((vector_size(4)));
using u8x8 = u8 __attribute__((vector_size(8)));
using u8x16 = u8 __attribute__((vector_size(16)));
using u8x32 = u8 __attribute__((vector_size(32)));

using u16x2 = u16 __attribute__((vector_size(4)));
using u16x4 = u16 __attribute__((vector_size(8)));
using u16x8 = u16 __attribute__((vector_size(16)));
using u16x16 = u16 __attribute__((vector_size(32)));

using u32x2 = u32 __attribute__((vector_size(8)));
using u32x4 = u32 __attribute__((vector_size(16)));
using u32x8 = u32 __attribute__((vector_size(32)));

using u64x2 = u64 __attribute__((vector_size(16)));
using u64x4 = u64 __attribute__((vector_size(32)));

using f32x2 = float __attribute__((vector_size(8)));
using f32x4 = float __attribute__((vector_size(16)));
using f32x8 = float __attribute__((vector_size(32)));

using f64x2 = double __attribute__((vector_size(16)));
using f64x4 = double __attribute__((vector_size(32)));

// FIXME: Is this really the best/simplest way?
//        Ideally we should look for the presence of the vector_size attribute instead
// FIXME: This cannot just be vector as that would somehow clash with AK::Vector, even without being exported
template<typename T>
concept SIMDVector = requires { __builtin_convertvector(declval<T>(), T); };

template<typename T>
using ElementOf = RemoveReference<decltype(declval<T>()[0])>;

static_assert(IsSame<ElementOf<i8x4>, i8>);
static_assert(IsSame<ElementOf<f32x4>, float>);

template<SIMDVector V>
constexpr static size_t vector_length = sizeof(V) / sizeof(ElementOf<V>);

static_assert(vector_length<i8x4> == 4);
static_assert(vector_length<f32x4> == 4);

namespace Detail {

template<typename T>
struct IndexVectorFor;

template<SIMDVector T>
requires(IsIntegral<ElementOf<T>>)
struct IndexVectorFor<T> {
    using Type = T;
};

template<SIMDVector T>
requires(IsFloatingPoint<ElementOf<T>>)
struct IndexVectorFor<T> {
    using Type = Conditional<
        IsSame<ElementOf<T>, float>,
        u32 __attribute__((vector_size(sizeof(T)))),
        u64 __attribute__((vector_size(sizeof(T))))>;
};

}

template<SIMDVector T>
using IndexVectorFor = typename Detail::IndexVectorFor<T>::Type;

static_assert(IsSame<IndexVectorFor<i8x16>, i8x16>);
static_assert(IsSame<IndexVectorFor<u32x4>, u32x4>);
static_assert(IsSame<IndexVectorFor<u64x4>, u64x4>);
#if defined(AK_COMPILER_CLANG)
// FIXME: GCC silently ignores the dependent vector_size attribute, this seems to be a bug
//        https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68703
static_assert(IsSame<IndexVectorFor<f32x4>, u32x4>);
static_assert(IsSame<IndexVectorFor<f64x4>, u64x4>);
#endif

}
