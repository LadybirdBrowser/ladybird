/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <LibJS/Heap/BlockAllocator.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Heap/HeapBlock.h>

namespace JS {

CellAllocator::CellAllocator(size_t cell_size, char const* class_name)
    : m_class_name(class_name)
    , m_cell_size(cell_size)
{
}

Cell* CellAllocator::allocate_cell(Heap& heap)
{
    if (!m_list_node.is_in_list())
        heap.register_cell_allocator({}, *this);

    if (m_usable_blocks.is_empty()) {
        auto block = HeapBlock::create_with_cell_size(heap, *this, m_cell_size, m_class_name);
        auto block_ptr = reinterpret_cast<FlatPtr>(block.ptr());
        if (m_min_block_address > block_ptr)
            m_min_block_address = block_ptr;
        if (m_max_block_address < block_ptr)
            m_max_block_address = block_ptr;
        m_usable_blocks.append(*block.leak_ptr());
    }

    auto& block = *m_usable_blocks.last();
    auto* cell = block.allocate();
    VERIFY(cell);
    if (block.is_full())
        m_full_blocks.append(*m_usable_blocks.last());
    return cell;
}

void CellAllocator::block_did_become_empty(Badge<Heap>, HeapBlock& block)
{
    block.m_list_node.remove();
    // NOTE: HeapBlocks are managed by the BlockAllocator, so we don't want to `delete` the block here.
    block.~HeapBlock();
    m_block_allocator.deallocate_block(&block);
}

void CellAllocator::block_did_become_usable(Badge<Heap>, HeapBlock& block)
{
    VERIFY(!block.is_full());
    m_usable_blocks.append(block);
}

}
