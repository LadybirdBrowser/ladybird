/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Forward.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace GC {

NonnullOwnPtr<HeapBlock> HeapBlock::create(Heap& heap, CellAllocator& cell_allocator)
{
    char const* name = nullptr;
    auto* block = static_cast<HeapBlock*>(cell_allocator.block_allocator().allocate_block(name));
    new (block) HeapBlock(heap, cell_allocator);
    heap.m_live_heap_blocks.set(block);
    return adopt_own(*block);
}

HeapBlock::HeapBlock(Heap& heap, CellAllocator& cell_allocator)
    : HeapBlockBase(heap, cell_allocator.type_info())
    , m_cell_allocator(cell_allocator)
    , m_cell_size(cell_allocator.cell_size())
    , m_cell_count((BLOCK_SIZE - sizeof(HeapBlock)) / m_cell_size)
{
    VERIFY(m_cell_size >= sizeof(FreelistEntry));
    ASAN_POISON_MEMORY_REGION(m_storage, BLOCK_SIZE - sizeof(HeapBlock));
}

HeapBlock::FreeCells HeapBlock::take_free_cells()
{
    FreeCells free_cells {
        .free_list = freelist_head(),
        .lazy_start = bit_cast<FlatPtr>(cell(m_next_lazy_freelist_index)),
        .lazy_end = bit_cast<FlatPtr>(cell(cell_count())),
    };
    // NB: Inline allocators bump through these cells without further checks, so make sure they are within the block.
    VERIFY(free_cells.lazy_start <= free_cells.lazy_end);
    VERIFY(free_cells.lazy_end <= bit_cast<FlatPtr>(this) + BLOCK_SIZE);
    m_freelist = 0;
    m_next_lazy_freelist_index = cell_count();
    return free_cells;
}

void HeapBlock::give_back_free_cells(Cell* free_list, FlatPtr lazy_cursor)
{
    // NB: take_free_cells() left the block without free cells of its own, and nothing frees cells in a block while
    //     its free cells are out, so the list becomes the block's free list as it is. Its links can only lead into
    //     this block (see follow_freelist_link()).
    VERIFY(is_end_of_freelist(freelist_head()));
    VERIFY(!has_lazy_freelist());
    VERIFY(is_end_of_freelist(free_list) || is_valid_cell_pointer(free_list));
    m_freelist = bit_cast<FlatPtr>(free_list);

    auto storage = reinterpret_cast<FlatPtr>(m_storage);
    VERIFY(lazy_cursor >= storage);
    VERIFY((lazy_cursor - storage) % m_cell_size == 0);
    auto lazy_index = (lazy_cursor - storage) / m_cell_size;
    VERIFY(lazy_index <= cell_count());
    m_next_lazy_freelist_index = lazy_index;
}

void HeapBlock::deallocate(Cell* cell)
{
    // NB: The cell is one of this block's, since for_each_cell() found it, and the free list only ever holds cells
    //     that this function or give_back_free_cells() put there.
    VERIFY(cell->state() == Cell::State::Live);
    VERIFY(!cell->is_marked());

    if (auto* destroy = type_info().destroy)
        destroy(cell);
    auto* freelist_entry = new (cell) FreelistEntry();
    freelist_entry->set_state(Cell::State::Dead);
    freelist_entry->next = m_freelist;
    m_freelist = bit_cast<FlatPtr>(freelist_entry);

#ifdef HAS_ADDRESS_SANITIZER
    auto dword_after_freelist = round_up_to_power_of_two(reinterpret_cast<uintptr_t>(freelist_entry) + sizeof(FreelistEntry), 8);
    VERIFY((dword_after_freelist - reinterpret_cast<uintptr_t>(freelist_entry)) <= m_cell_size);
    VERIFY(m_cell_size >= sizeof(FreelistEntry));
    // We can't poision the cell tracking data, nor the FreeListEntry's vtable or next pointer
    // This means there's sizeof(FreelistEntry) data at the front of each cell that is always read/write
    // On x86_64, this ends up being 24 bytes due to the size of the FreeListEntry's vtable, while on x86, it's only 12 bytes.
    ASAN_POISON_MEMORY_REGION(reinterpret_cast<void*>(dword_after_freelist), m_cell_size - sizeof(FreelistEntry));
#endif
}

}
