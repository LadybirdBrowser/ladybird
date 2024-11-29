/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/Types.h>
#include <LibGC/Cell.h>

namespace GC {

static_assert(sizeof(double) == 8);
static_assert(sizeof(void*) <= sizeof(double));
// To make our Value representation compact we can use the fact that IEEE
// doubles have a lot (2^52 - 2) of NaN bit patterns. The canonical form being
// just 0x7FF8000000000000 i.e. sign = 0 exponent is all ones and the top most
// bit of the mantissa set.
static constexpr u64 CANON_NAN_BITS = bit_cast<u64>(__builtin_nan(""));
static_assert(CANON_NAN_BITS == 0x7FF8000000000000);
// (Unfortunately all the other values are valid so we have to convert any
// incoming NaNs to this pattern although in practice it seems only the negative
// version of these CANON_NAN_BITS)
// +/- Infinity are represented by a full exponent but without any bits of the
// mantissa set.
static constexpr u64 POSITIVE_INFINITY_BITS = bit_cast<u64>(__builtin_huge_val());
static constexpr u64 NEGATIVE_INFINITY_BITS = bit_cast<u64>(-__builtin_huge_val());
static_assert(POSITIVE_INFINITY_BITS == 0x7FF0000000000000);
static_assert(NEGATIVE_INFINITY_BITS == 0xFFF0000000000000);
// However as long as any bit is set in the mantissa with the exponent of all
// ones this value is a NaN, and it even ignores the sign bit.
// (NOTE: we have to use __builtin_isnan here since some isnan implementations are not constexpr)
static_assert(__builtin_isnan(bit_cast<double>(0x7FF0000000000001)));
static_assert(__builtin_isnan(bit_cast<double>(0xFFF0000000040000)));

// Any user-space pointer values will have their upper bits set to 0.
// Conveniently, when those same bits of a _double_ value are 0,
// then the encoded value will represent be a _subnormal_ (or `0.0`).
// Subnormals are rare, they typically indicate an underflow error, and are often
// avoided since computations involving subnormals are slower on most hardware.
// We can therefore encode these rare values by NaN-boxing them, and re-use
// the newly available encodings for the much more frequent cell pointers.
// Storing pointers with their usual bit pattern also makes CPUs and compilers happy.
static_assert(!__builtin_isnormal(bit_cast<double>(0x0000700000000000)));
static_assert(!__builtin_isnormal(bit_cast<double>(0x00007FFFFFFFFFFF)));
static_assert(!__builtin_isnormal(bit_cast<double>(0x000FFFFFFFFFFFFF)));
static_assert(__builtin_isnormal(bit_cast<double>(0x0010000000000000)));

static constexpr u64 SUBNORMAL_PATTERN = 0xFFFC000000000000;
static constexpr u64 TAG_PATTERN = 0xFFFF800000000000;
static constexpr u64 MAX_PAYLOAD_BITS = 47;
// Bottom tags are 3 bits since `Cell` pointers are at least 8-byte aligned.
static constexpr u64 BOTTOM_TAG_PATTERN = 0x7ULL;

class NanBoxedCell {
public:
    // A cell is any non-zero NanBoxedCell with the first 17 bits unset.
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_cell() const
    {
        return (m_value.encoded & TAG_PATTERN) == 0 && m_value.encoded != 0;
    }

    // A nan-boxed value is any NanBoxedCell with the first 17 bits set.
    // This wastes a few bits, but keeps the `is_double` check more efficient.
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_nan_boxed_value() const
    {
        return (m_value.encoded & TAG_PATTERN) == TAG_PATTERN;
    }

    // A nan-boxed subnormal is any NanBoxedCell with the first 17 bits equal to `SUBNORMAL_PATTERN`.
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_nan_boxed_subnormal() const
    {
        return (m_value.encoded & TAG_PATTERN) == SUBNORMAL_PATTERN;
    }

    // A double is any other NanBoxedCell.
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_double() const
    {
        return !is_cell() && !is_nan_boxed_value();
    }

    // Returns true if `m_value.as_double` contains a valid `double`.
    [[nodiscard]] ALWAYS_INLINE constexpr bool has_double() const
    {
        return is_double() && !is_nan_boxed_subnormal();
    }

    [[nodiscard]] ALWAYS_INLINE constexpr double as_double() const
    {
        if (is_nan_boxed_subnormal()) [[unlikely]]
            return bit_cast<double>(m_value.encoded & ~SUBNORMAL_PATTERN);
        if (is_double())
            return m_value.as_double;
        VERIFY_NOT_REACHED();
    }

    static ALWAYS_INLINE constexpr FlatPtr extract_pointer_bits(u64 encoded)
    {
        return static_cast<FlatPtr>(encoded) & ~BOTTOM_TAG_PATTERN;
    }

    template<typename PointerType>
    ALWAYS_INLINE constexpr PointerType* extract_pointer() const
    {
        VERIFY(is_cell());
        return reinterpret_cast<PointerType*>(extract_pointer_bits(m_value.encoded));
    }

    ALWAYS_INLINE constexpr Cell& as_cell() const
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    ALWAYS_INLINE constexpr bool is_nan() const
    {
        return m_value.encoded == CANON_NAN_BITS;
    }

    ALWAYS_INLINE constexpr u64 cell_tag() const
    {
        return m_value.encoded & BOTTOM_TAG_PATTERN;
    }

protected:
    union {
        double as_double;
        u64 encoded;
    } m_value { .encoded = 0 };
};

template<size_t tag_bits = 3>
class NanBoxedValue : public NanBoxedCell {
public:
    static constexpr size_t TAG_BITS = tag_bits;
    static constexpr size_t PAYLOAD_BITS = MAX_PAYLOAD_BITS - TAG_BITS;

    static_assert(TAG_BITS <= MAX_PAYLOAD_BITS);

    template<typename T = u16>
    [[nodiscard]] T nan_boxed_tag() const
    {
        return static_cast<T>((m_value.encoded >> (MAX_PAYLOAD_BITS - TAG_BITS)) & ((1 << TAG_BITS) - 1));
    }
};

static_assert(sizeof(NanBoxedCell) == sizeof(double));

}
