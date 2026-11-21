/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BitCast.h>
#include <AK/BuiltinWrappers.h>
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

// A cell's bit in the bitmaps of its block's descriptor is its offset in its block divided by the cell size, which
// tells the cells apart since they are a cell size apart.
// OPTIMIZATION: Dividing by the cell size is slow, so the offset is multiplied by 2^32 / cell size, rounded up, and
//               shifted right by 32. For offsets and cell sizes up to 2^14, the rounding error never reaches the next
//               whole number.
ALWAYS_INLINE size_t cell_index(FFI::BlockDescriptor const& descriptor, void const* cell)
{
    auto offset = static_cast<u64>(bit_cast<FlatPtr>(cell) & (HeapBlockBase::BLOCK_SIZE - 1));
    return static_cast<size_t>((offset * descriptor.cell_index_magic) >> 32);
}

ALWAYS_INLINE u64 cell_bit(size_t index) { return static_cast<u64>(1) << (index % 64); }

// Marking sets mark bits from several threads at once, so every access is atomic. Outside of marking, nothing else
// writes them, so relaxed accesses are enough.
ALWAYS_INLINE bool is_marked(FFI::BlockDescriptor const& descriptor, void const* cell)
{
    auto index = cell_index(descriptor, cell);
    return __atomic_load_n(&descriptor.marked[index / 64], __ATOMIC_RELAXED) & cell_bit(index);
}

ALWAYS_INLINE bool is_marked(void const* cell)
{
    return is_marked(block_descriptor_for(cell), cell);
}

ALWAYS_INLINE void set_marked(void const* cell)
{
    auto& descriptor = block_descriptor_for(cell);
    auto index = cell_index(descriptor, cell);
    __atomic_fetch_or(&descriptor.marked[index / 64], cell_bit(index), __ATOMIC_RELAXED);
}

// A cell is allocated from when it is handed out until the sweep or a move frees its slot. While an allocator has taken
// a run of free cells of a block to hand them out, they all count as allocated, see CellAllocator::LocalFreeCells. Only
// the thread that owns the block's heap reads and writes these bits.
ALWAYS_INLINE bool is_allocated(FFI::BlockDescriptor const& descriptor, size_t index)
{
    return descriptor.allocated[index / 64] & cell_bit(index);
}

ALWAYS_INLINE u32 allocated_cell_count_of(FFI::BlockDescriptor const& descriptor)
{
    u32 count = 0;
    for (auto word : descriptor.allocated)
        count += popcount(word);
    return count;
}

// The cell that `address` points into, if it is an allocated cell of a block of `heap`. The address must be within the
// committed part of the heap region, where every block has a descriptor.
ALWAYS_INLINE void* allocated_cell_containing(void const* heap, FlatPtr address, size_t cells_offset)
{
    auto const& descriptor = block_descriptor_for(bit_cast<void const*>(address));
    if (__atomic_load_n(&descriptor.heap, __ATOMIC_ACQUIRE) != heap)
        return nullptr;
    auto block = address & ~(HeapBlockBase::BLOCK_SIZE - 1);
    if (address - block < cells_offset)
        return nullptr;
    auto offset_in_cells = static_cast<u64>(address - block - cells_offset);
    auto index = static_cast<size_t>((offset_in_cells * descriptor.cell_index_magic) >> 32);
    if (index >= descriptor.cell_count)
        return nullptr;
    auto cell = block + cells_offset + index * descriptor.cell_size;
    if (!is_allocated(descriptor, cell_index(descriptor, bit_cast<void const*>(cell))))
        return nullptr;
    return bit_cast<void*>(cell);
}

}
