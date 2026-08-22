/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/Types.h>
#include <LibGC/Cell.h>
#include <LibGC/HeapRegion.h>

namespace GC {

static_assert(sizeof(double) == 8);
static_assert(sizeof(void*) == sizeof(u64));
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
// This means we can use all of these NaNs to store all other options for Value.
// To make sure all of these other representations we use 0x7FF8 as the base top
// 2 bytes which ensures the value is always a NaN.
static constexpr u64 BASE_TAG = 0x7FF8;
// This leaves the sign bit and the three lower bits for tagging a value and then
// 48 bits of potential payload.
// First the pointer backed types (Object, String etc.), to signify this category
// and make stack scanning easier we use the sign bit (top most bit) of 1 to
// signify that it is a pointer backed type.
static constexpr u64 IS_CELL_BIT = 0x8000 | BASE_TAG;
// The payload stores an offset from the process-wide heap region base. The
// region is aligned to its size so masking the offset keeps the decoded
// pointer inside the region.

static constexpr u64 IS_CELL_PATTERN = 0xFFF8ULL;
static constexpr u64 TAG_SHIFT = 48;
static constexpr u64 TAG_EXTRACTION = 0xFFFF000000000000;
static_assert((HEAP_REGION_OFFSET_MASK & TAG_EXTRACTION) == 0);
static constexpr u64 SHIFTED_IS_CELL_PATTERN = IS_CELL_PATTERN << TAG_SHIFT;

class GC_API NanBoxedValue {
public:
    bool is_cell() const { return (m_value.tag & IS_CELL_PATTERN) == IS_CELL_PATTERN; }

    static FlatPtr encode_pointer_bits(void const* pointer)
    {
        auto pointer_bits = reinterpret_cast<FlatPtr>(pointer);
        ASSERT((pointer_bits & ~HEAP_REGION_OFFSET_MASK) == js_heap_region_base);
        return pointer_bits & HEAP_REGION_OFFSET_MASK;
    }

    static FlatPtr extract_pointer_bits(u64 encoded)
    {
        return js_heap_region_base + (encoded & HEAP_REGION_OFFSET_MASK);
    }

    template<typename PointerType>
    PointerType* extract_pointer() const
    {
        VERIFY(is_cell());
        return reinterpret_cast<PointerType*>(extract_pointer_bits(m_value.encoded));
    }

    Cell& as_cell()
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    Cell& as_cell() const
    {
        VERIFY(is_cell());
        return *extract_pointer<Cell>();
    }

    bool is_nan() const
    {
        return m_value.encoded == CANON_NAN_BITS;
    }

protected:
    union {
        double as_double;
        struct {
            u64 payload : 48;
            u64 tag : 16;
        };
        u64 encoded;
    } m_value { .encoded = 0 };
};

static_assert(sizeof(NanBoxedValue) == sizeof(double));

}
