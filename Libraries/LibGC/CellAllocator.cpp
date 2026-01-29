/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>

namespace GC {

CellAllocator::CellAllocator(size_t cell_size, StringView class_name, bool overrides_must_survive_garbage_collection, bool overrides_finalize)
    : m_class_name(class_name)
    , m_cell_size(cell_size)
    , m_overrides_must_survive_garbage_collection(overrides_must_survive_garbage_collection)
    , m_overrides_finalize(overrides_finalize)
{
}

Cell* CellAllocator::allocate_cell(Heap& heap)
{
    if (!m_list_node.is_in_list())
        heap.register_cell_allocator({}, *this);

    if (m_usable_blocks.is_empty() && heap.is_incremental_sweep_active() && !heap.is_gc_deferred()) {
        // Sweep our own pending blocks first to try to find free cells
        // before allocating a new block.
        while (!m_usable_blocks.is_empty() || !m_blocks_pending_sweep.is_empty()) {
            if (!m_usable_blocks.is_empty())
                break;
            heap.sweep_block(*m_blocks_pending_sweep.first());
        }
    }

    if (m_usable_blocks.is_empty()) {
        auto block = HeapBlock::create_with_cell_size(heap, *this, m_cell_size, m_class_name, m_overrides_must_survive_garbage_collection, m_overrides_finalize);
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
    block.heap().m_live_heap_blocks.remove(&block);
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
