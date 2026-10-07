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
{
    VERIFY(m_cell_size >= sizeof(FreelistEntry));
    ASAN_POISON_MEMORY_REGION(m_storage, BLOCK_SIZE - sizeof(HeapBlock));
}

Cell* HeapBlock::take_free_cells()
{
    auto head = bit_cast<FlatPtr>(freelist_head());
    m_freelist = 0;

    // NB: The lazily initialized cells come first, in address order.
    for (size_t index = cell_count(); index-- > m_next_lazy_freelist_index;) {
        auto* free_cell = cell(index);
        ASAN_UNPOISON_MEMORY_REGION(free_cell, sizeof(FreelistEntry));
        auto* freelist_entry = new (free_cell) FreelistEntry();
        freelist_entry->set_state(Cell::State::Dead);
        freelist_entry->next = head;
        head = bit_cast<FlatPtr>(freelist_entry);
    }
    m_next_lazy_freelist_index = cell_count();
    return bit_cast<Cell*>(head);
}

void HeapBlock::give_back_free_cells(Cell* cells)
{
    for (auto* head = cells; !is_end_of_freelist(head);) {
        VERIFY(is_valid_cell_pointer(head));
        VERIFY(head->state() == Cell::State::Dead);
        auto* next = next_free_cell(head);
        static_cast<FreelistEntry*>(head)->next = m_freelist;
        m_freelist = bit_cast<FlatPtr>(head);
        head = next;
    }
}

void HeapBlock::deallocate(Cell* cell)
{
    VERIFY(is_valid_cell_pointer(cell));
    VERIFY(is_end_of_freelist(freelist_head()) || is_valid_cell_pointer(freelist_head()));
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
