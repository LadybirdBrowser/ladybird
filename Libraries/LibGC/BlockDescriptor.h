/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibGC/HeapRegion.h>
#include <LibGC/Internals.h>
#include <RustFFI.h>

// NB: Only LibGC's own sources include this, since RustFFI.h is generated in its build directory.

namespace GC {

static_assert(FFI::BLOCK_SIZE == HeapBlockBase::BLOCK_SIZE);

// What the heap knows about each block lives in the block's descriptor, outside the heap region, so that nothing in a
// block can change it: the heap and the type of the block, and the mark bits of its cells. The descriptor of a block
// is at the index of the block in the region, in a table that LibGC's Rust core reserves once.
extern GC_API FFI::BlockDescriptor* const g_block_descriptors;

ALWAYS_INLINE FFI::BlockDescriptor& block_descriptor_for(void const* address)
{
    return g_block_descriptors[(bit_cast<FlatPtr>(address) - heap_region_base()) / HeapBlockBase::BLOCK_SIZE];
}

// A cell's mark bit is its offset in its block divided by the cell size, which tells the cells apart since they are a
// cell size apart.
// OPTIMIZATION: Dividing by the cell size is slow, so the offset is multiplied by 2^32 / cell size, rounded up, and
//               shifted right by 32. For offsets and cell sizes up to 2^14, the rounding error never reaches the next
//               whole number.
ALWAYS_INLINE size_t mark_bit_index(FFI::BlockDescriptor const& descriptor, void const* cell)
{
    auto offset = static_cast<u64>(bit_cast<FlatPtr>(cell) & (HeapBlockBase::BLOCK_SIZE - 1));
    return static_cast<size_t>((offset * descriptor.cell_index_magic) >> 32);
}

// Marking sets mark bits from several threads at once, so every access is atomic. Outside of marking, nothing else
// writes them, so relaxed accesses are enough.
ALWAYS_INLINE bool is_marked(FFI::BlockDescriptor const& descriptor, void const* cell)
{
    auto index = mark_bit_index(descriptor, cell);
    return __atomic_load_n(&descriptor.marked[index / 64], __ATOMIC_RELAXED) & (static_cast<u64>(1) << (index % 64));
}

ALWAYS_INLINE bool is_marked(void const* cell)
{
    return is_marked(block_descriptor_for(cell), cell);
}

ALWAYS_INLINE void set_marked(void const* cell)
{
    auto& descriptor = block_descriptor_for(cell);
    auto index = mark_bit_index(descriptor, cell);
    __atomic_fetch_or(&descriptor.marked[index / 64], static_cast<u64>(1) << (index % 64), __ATOMIC_RELAXED);
}

ALWAYS_INLINE void clear_marks(FFI::BlockDescriptor& descriptor)
{
    for (auto& word : descriptor.marked)
        __atomic_store_n(&word, 0, __ATOMIC_RELAXED);
}

// Fills in the descriptor of a block that is about to hold cells, and empties it once the block goes away.
void publish_block_descriptor(HeapBlock&, CellAllocator&);
void retire_block_descriptor(HeapBlock&);

}
