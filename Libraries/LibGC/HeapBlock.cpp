/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibGC/BlockDescriptor.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace GC {

static_assert(HeapBlock::min_possible_cell_size >= FFI::MIN_CELL_SIZE);

FFI::BlockDescriptor* const g_block_descriptors = FFI::rust_gc_block_descriptor_table(HEAP_REGION_SIZE);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
size_t HeapBlock::cells_offset()
{
    // NB: The cells start at the largest alignment that a cell may have, which is more than 16 bytes in builds for some
    //     CPUs.
    static_assert(offsetof(HeapBlock, m_storage) == round_up_to_power_of_two(112, max_cell_alignment));
    // NB: A block counts its cells from its size, and the Rust core from this offset, so the two have to match.
    static_assert(sizeof(HeapBlock) == offsetof(HeapBlock, m_storage));
    return offsetof(HeapBlock, m_storage);
}
#pragma GCC diagnostic pop

void HeapBlock::initialize(void* block, Heap& heap, CellAllocator& cell_allocator)
{
    new (block) HeapBlock(heap, cell_allocator);
}

HeapBlock::HeapBlock(Heap& heap, CellAllocator& cell_allocator)
    : HeapBlockBase(heap, cell_allocator.type_info(), cell_allocator.foreign_type())
    , m_cell_allocator(cell_allocator)
    , m_cell_size(cell_allocator.cell_size())
    , m_cell_count((BLOCK_SIZE - sizeof(HeapBlock)) / m_cell_size)
{
    VERIFY(m_cell_size >= min_possible_cell_size);
    ASAN_POISON_MEMORY_REGION(m_storage, BLOCK_SIZE - sizeof(HeapBlock));
}

bool HeapBlock::is_pending_sweep() const
{
    return block_descriptor_for(this).pending_sweep;
}

bool HeapBlock::is_being_swept() const
{
    return block_descriptor_for(this).being_swept;
}

u32 HeapBlock::allocated_cell_count() const
{
    return allocated_cell_count_of(block_descriptor_for(this));
}

}
