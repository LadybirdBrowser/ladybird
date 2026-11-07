/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>

namespace GC {

struct BlockPoolKey {
    void const* type { nullptr };
    size_t cell_size { 0 };

    bool operator==(BlockPoolKey const&) const = default;
};

}

template<>
struct AK::Traits<GC::BlockPoolKey> : public DefaultTraits<GC::BlockPoolKey> {
    static unsigned hash(GC::BlockPoolKey const& key) { return pair_int_hash(ptr_hash(key.type), u64_hash(key.cell_size)); }
};

namespace GC {

// Descriptors are shared by the heaps of all threads, so they keep no per-heap state. Each has an index instead, which
// keeps the tables of the heaps dense: the indices of descriptors that went away are used again.
struct DescriptorIndices {
    Mutex mutex;
    size_t next_index { 0 };
    Vector<size_t> free_indices;
};

static DescriptorIndices& descriptor_indices()
{
    static AK::NeverDestroyed<DescriptorIndices> indices;
    return *indices;
}

CellAllocatorDescriptorBase::CellAllocatorDescriptorBase(CellTypeInfo const& type_info, StringView class_name, void const* foreign_type)
    : m_type_info(type_info)
    , m_class_name(class_name)
    , m_foreign_type(foreign_type)
{
    auto& indices = descriptor_indices();
    MutexLocker locker { indices.mutex };
    m_index = indices.free_indices.is_empty() ? indices.next_index++ : indices.free_indices.take_last();
}

void CellAllocatorDescriptorBase::release_index()
{
    auto& indices = descriptor_indices();
    MutexLocker locker { indices.mutex };
    indices.free_indices.append(m_index);
}

// A block that was used for a type stays with that type for as long as the process lives, whichever heap had it. So a
// stale pointer into the region can only ever find a cell of the type it was made for, at the same offsets. Heaps die
// while the process lives, so this has to hold across heaps: the pools outlive them, and a new heap reuses the blocks.
//
// A foreign type names the type for the whole process, while its descriptors come and go with the heaps. A descriptor
// without one lives as long as the process, as C++ ones do, so its own address does. The cell size tells the size
// classes of a foreign type apart, whose blocks do not fit each other.
BlockAllocator& CellAllocatorDescriptorBase::block_allocator() const
{
    static AK::NeverDestroyed<Mutex> mutex;
    static AK::NeverDestroyed<HashMap<BlockPoolKey, NonnullOwnPtr<BlockAllocator>>> pools;

    BlockPoolKey key { m_foreign_type ? m_foreign_type : this, cell_size() };
    MutexLocker locker { *mutex };
    return *pools->ensure(key, [] { return make<BlockAllocator>(); });
}

CellAllocator::CellAllocator(CellAllocatorDescriptorBase& descriptor)
    : m_descriptor(descriptor)
    , m_block_allocator(descriptor.block_allocator())
{
    m_local.cell_size = cell_size();
}

CellAllocator::~CellAllocator()
{
    m_blocks_pending_sweep.clear();

    auto reclaim_all = [this](BlockList& blocks) {
        while (auto* block = blocks.take_first()) {
            block->~HeapBlock();
            m_block_allocator.deallocate_block(block, DeferDecommit::No);
        }
    };
    reclaim_all(m_full_blocks);
    reclaim_all(m_usable_blocks);

    // NB: This heap is gone, and another one may not come along to use the blocks of this type for a long time.
    m_block_allocator.return_freed_blocks_to_system();
}

Cell* CellAllocator::allocate_cell_slow(Heap& heap)
{
    // NB: The local block has no free cells left, and take_free_cells() left it full.
    VERIFY(HeapBlock::is_end_of_freelist(m_local.free_list.ptr()));
    VERIFY(m_local.cursor == m_local.end);
    m_local.free_list = nullptr;
    m_local.cursor = 0;
    m_local.end = 0;
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
        //     destructor may allocate from. A heap that collects on every
        //     allocation has no local free lists, which code that pops them
        //     itself would take cells from without collecting.
        if (block.is_pending_sweep() || block.is_being_swept() || heap.should_collect_on_every_allocation()) {
            auto* cell = block.allocate();
            VERIFY(cell);
            if (block.is_full())
                m_full_blocks.append(block);
            return cell;
        }

        auto free_cells = block.take_free_cells();
        VERIFY(!HeapBlock::is_end_of_freelist(free_cells.free_list.ptr()) || free_cells.lazy_start < free_cells.lazy_end);
        m_local.free_list = free_cells.free_list;
        m_local.cursor = free_cells.lazy_start;
        m_local.end = free_cells.lazy_end;
        m_full_blocks.append(block);
        m_local_block = &block;
        return allocate_cell(heap);
    }
}

void CellAllocator::give_back_local_free_list(Badge<Heap>)
{
    if (!m_local_block)
        return;
    if (!HeapBlock::is_end_of_freelist(m_local.free_list.ptr()) || m_local.cursor < m_local.end) {
        m_local_block->give_back_free_cells(m_local.free_list.ptr(), m_local.cursor);
        m_usable_blocks.append(*m_local_block);
    }
    m_local.free_list = nullptr;
    m_local.cursor = 0;
    m_local.end = 0;
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
