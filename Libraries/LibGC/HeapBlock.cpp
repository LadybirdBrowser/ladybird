/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <stdio.h>
#include <sys/mman.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#endif

namespace GC {

size_t HeapBlockBase::block_size = PAGE_SIZE;

NonnullOwnPtr<HeapBlock> HeapBlock::create_with_cell_size(Heap& heap, CellAllocator& cell_allocator, size_t cell_size, [[maybe_unused]] char const* class_name)
{
    char const* name = nullptr;
    auto* block = static_cast<HeapBlock*>(cell_allocator.block_allocator().allocate_block(name));
    new (block) HeapBlock(heap, cell_allocator, cell_size);
    return NonnullOwnPtr<HeapBlock>(NonnullOwnPtr<HeapBlock>::Adopt, *block);
}

HeapBlock::HeapBlock(Heap& heap, CellAllocator& cell_allocator, size_t cell_size)
    : HeapBlockBase(heap)
    , m_cell_allocator(cell_allocator)
    , m_cell_size(cell_size)
{
    VERIFY(cell_size >= sizeof(FreelistEntry));
    ASAN_POISON_MEMORY_REGION(m_storage, block_size - sizeof(HeapBlock));
}

void HeapBlock::deallocate(Cell* cell)
{
    VERIFY(is_valid_cell_pointer(cell));
    VERIFY(!m_freelist || is_valid_cell_pointer(m_freelist));
    VERIFY(cell->state() == Cell::State::Live);
    VERIFY(!cell->is_marked());

    cell->~Cell();
    auto* freelist_entry = new (cell) FreelistEntry();
    freelist_entry->set_state(Cell::State::Dead);
    freelist_entry->next = m_freelist;
    m_freelist = freelist_entry;

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
