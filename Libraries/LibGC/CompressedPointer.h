/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibGC/HeapRegion.h>

namespace GC {

// A compressed cell pointer is the cell's offset into the heap region in units of the 8-byte cell alignment. A u32 then
// covers the whole region, so decoding never leaves it whatever the 32 bits are. Null compresses to 0.
static constexpr u32 COMPRESSED_POINTER_SHIFT = 3;
static_assert((static_cast<u64>(1) << (32 + COMPRESSED_POINTER_SHIFT)) == HEAP_REGION_SIZE);

// Every compressed pointer below this decodes into the low guard, so it is never a cell.
static constexpr u32 MIN_COMPRESSED_CELL_POINTER = HEAP_REGION_LOW_GUARD_SIZE >> COMPRESSED_POINTER_SHIFT;

ALWAYS_INLINE u32 compress(void const* cell)
{
    if (!cell)
        return 0;
    auto offset = reinterpret_cast<FlatPtr>(cell) - js_heap_region_base;
    ASSERT(offset < HEAP_REGION_SIZE);
    ASSERT(offset % (1 << COMPRESSED_POINTER_SHIFT) == 0);
    return static_cast<u32>(offset >> COMPRESSED_POINTER_SHIFT);
}

// Decodes a compressed pointer that is not null.
//
// Makes the compressed pointer live up to this point, and the decoded address from this point on. The conservative
// stack scan recognizes a cell by its address or by its compressed pointer in either half of a word, but not by its
// unshifted offset, which the compiler could otherwise keep instead while either is held across an allocation.
ALWAYS_INLINE FlatPtr decompress_nonnull(u32 compressed, FlatPtr heap_region_base = js_heap_region_base)
{
    auto address = heap_region_base + (static_cast<FlatPtr>(compressed) << COMPRESSED_POINTER_SHIFT);
    // NB: The assembly is empty. It only has the compiler put the compressed pointer and the address in registers here.
    asm("" : "+r"(address) : "r"(compressed));
    return address;
}

ALWAYS_INLINE FlatPtr decompress(u32 compressed)
{
    if (!compressed)
        return 0;
    return decompress_nonnull(compressed);
}

}
