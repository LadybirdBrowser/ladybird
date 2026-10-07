/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>

namespace GC {

BlockAllocator& CellAllocator::shared_block_allocator()
{
    static AK::NeverDestroyed<BlockAllocator> allocator;
    return *allocator;
}

CellAllocator::CellAllocator(CellAllocatorDescriptorBase& descriptor)
    : m_descriptor(descriptor)
    , m_block_allocator(shared_block_allocator())
{
}

CellAllocator::~CellAllocator()
{
    m_blocks_pending_sweep.clear();

    auto reclaim_all = [this](BlockList& blocks) {
        while (auto* block = blocks.take_first()) {
            block->~HeapBlock();
            m_block_allocator.deallocate_block(block, DeferDecommit::Yes);
        }
    };
    reclaim_all(m_full_blocks);
    reclaim_all(m_usable_blocks);
}

CellAllocator& CellAllocatorDescriptorBase::for_heap(Heap& heap)
{
    if (m_last_heap == &heap) [[likely]]
        return *m_last_allocator;
    auto& allocator = heap.cell_allocator_for({}, *this);
    m_last_heap = &heap;
    m_last_allocator = &allocator;
    return allocator;
}

Cell* CellAllocator::allocate_cell_slow(Heap& heap)
{
    VERIFY(HeapBlock::is_end_of_freelist(m_local_free_list.ptr()));
    m_local_free_list = nullptr;
    m_local_block = nullptr;

    if (!m_list_node.is_in_list())
        heap.register_cell_allocator({}, *this);

    bool can_sweep = heap.is_incremental_sweep_active() && !heap.is_gc_deferred();
    for (;;) {
        if (m_usable_blocks.is_empty() && can_sweep && !m_blocks_pending_sweep.is_empty()) {
            // Sweep our own pending blocks first to try to find free cells
            // before allocating a new block.
            heap.sweep_block(*m_blocks_pending_sweep.first());
            continue;
        }

        if (m_usable_blocks.is_empty()) {
            auto block = HeapBlock::create(heap, *this);
            m_usable_blocks.append(*block.leak_ptr());
        }

        auto& block = *m_usable_blocks.last();
        if (block.is_pending_sweep() && can_sweep) {
            heap.sweep_block(block);
            continue;
        }

        // NB: Without sweeping the block first, take a single cell from it,
        //     which the heap marks as allocated during the sweep. The same
        //     goes for the block the sweep is in the middle of, which a
        //     destructor may allocate from.
        if (block.is_pending_sweep() || block.is_being_swept()) {
            auto* cell = block.allocate();
            VERIFY(cell);
            if (block.is_full())
                m_full_blocks.append(block);
            return cell;
        }

        m_local_free_list = block.take_free_cells();
        VERIFY(!HeapBlock::is_end_of_freelist(m_local_free_list.ptr()));
        m_full_blocks.append(block);
        m_local_block = &block;
        return allocate_cell(heap);
    }
}

void CellAllocator::give_back_local_free_list(Badge<Heap>)
{
    if (!m_local_block)
        return;
    if (!HeapBlock::is_end_of_freelist(m_local_free_list.ptr())) {
        m_local_block->give_back_free_cells(m_local_free_list.ptr());
        m_usable_blocks.append(*m_local_block);
    }
    m_local_free_list = nullptr;
    m_local_block = nullptr;
}

void CellAllocator::block_did_become_empty(Badge<Heap>, HeapBlock& block, DeferDecommit defer_decommit)
{
    block.m_list_node.remove();
    block.heap().m_live_heap_blocks.remove(&block);
    // NOTE: HeapBlocks are managed by the BlockAllocator, so we don't want to `delete` the block here.
    block.~HeapBlock();
    m_block_allocator.deallocate_block(&block, defer_decommit);
}

void CellAllocator::block_did_become_usable(Badge<Heap>, HeapBlock& block)
{
    VERIFY(!block.is_full());
    m_usable_blocks.append(block);
}

}
