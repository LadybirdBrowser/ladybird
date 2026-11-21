/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/BlockDescriptor.h>
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

void CellAllocator::initialize_block(void* context, void* block)
{
    auto& allocator = *static_cast<CellAllocator*>(context);
    HeapBlock::initialize(block, allocator.m_heap, allocator);
}

// Frees the slot of a cell for LibGC's Rust core: runs its destructor first if it died, and marks it dead.
void CellAllocator::free_cell(void* context, void* address, bool destroy)
{
    auto& allocator = *static_cast<CellAllocator*>(context);
    auto* cell = static_cast<Cell*>(address);
    VERIFY(cell->state() == Cell::State::Live);
    if (auto* destroy_cell = allocator.type_info().destroy; destroy && destroy_cell) {
        allocator.m_heap.will_destroy_swept_cell({});
        destroy_cell(cell);
        allocator.m_heap.did_destroy_swept_cell({});
    }
    cell->set_state(Cell::State::Dead);

#ifdef HAS_ADDRESS_SANITIZER
    // NB: The header of a free cell, where its state is, stays accessible.
    auto after_header = round_up_to_power_of_two(reinterpret_cast<uintptr_t>(cell) + HeapBlock::min_possible_cell_size, 8);
    VERIFY(after_header - reinterpret_cast<uintptr_t>(cell) <= allocator.cell_size());
    ASAN_POISON_MEMORY_REGION(reinterpret_cast<void*>(after_header), allocator.cell_size() - (after_header - reinterpret_cast<uintptr_t>(cell)));
#endif
}

CellAllocator::CellAllocator(Heap& heap, CellAllocatorDescriptorBase& descriptor)
    : m_descriptor(descriptor)
    , m_block_allocator(descriptor.block_allocator())
    , m_heap(heap)
{
    m_local.cell_size = cell_size();
    FFI::AllocatorConfig config {
        .heap_region_base = heap_region_base(),
        .heap = &heap,
        .type_info = &type_info(),
        .pool = m_block_allocator.rust_pool({}),
        .local = reinterpret_cast<FFI::LocalFreeCells*>(&m_local),
        .cells_offset = static_cast<u32>(HeapBlock::cells_offset()),
        .cell_size = static_cast<u32>(cell_size()),
        .traced_on_owner_thread_only = descriptor.trace_thread() == TraceThread::OwnerThreadOnly,
        .movable = descriptor.is_movable(),
        .context = this,
        .initialize_block = initialize_block,
        .free_cell = free_cell,
    };
    m_allocator = FFI::rust_gc_allocator_create(&config);
}

CellAllocator::~CellAllocator()
{
    FFI::rust_gc_allocator_destroy(m_allocator);
}

static_assert(sizeof(CellAllocator::LocalFreeCells) == sizeof(FFI::LocalFreeCells));
static_assert(__builtin_offsetof(CellAllocator::LocalFreeCells, cursor) == __builtin_offsetof(FFI::LocalFreeCells, cursor));
static_assert(__builtin_offsetof(CellAllocator::LocalFreeCells, end) == __builtin_offsetof(FFI::LocalFreeCells, end));
static_assert(__builtin_offsetof(CellAllocator::LocalFreeCells, cell_size) == __builtin_offsetof(FFI::LocalFreeCells, cell_size));

Cell* CellAllocator::allocate_cell_slow(Heap& heap)
{
    if (!m_list_node.is_in_list())
        heap.register_cell_allocator({}, *this);

    // NB: A heap that collects on every allocation hands out single cells, as code that bumps through runs itself
    //     would take cells from them without collecting.
    bool can_sweep = heap.is_incremental_sweep_active() && !heap.is_gc_deferred();
    FFI::SweepStats stats {};
    // NB: The time spent sweeping goes into how far the heap grows before the next collection, see
    //     Heap::next_growth_factor().
    auto start_ns = can_sweep ? MonotonicTime::now().nanoseconds() : 0;
    auto* cell = static_cast<Cell*>(FFI::rust_gc_allocator_allocate(m_allocator, can_sweep, heap.should_collect_on_every_allocation(), &stats));
    if (stats.swept_blocks)
        heap.did_sweep_blocks_for_allocation({}, stats.swept_blocks, MonotonicTime::now().nanoseconds() - start_ns);
    ASAN_UNPOISON_MEMORY_REGION(cell, cell_size());
    return cell;
}

void CellAllocator::give_back_local_free_list(Badge<Heap>)
{
    FFI::rust_gc_allocator_give_back_local_free_cells(m_allocator);
}

size_t CellAllocator::block_count() const
{
    return FFI::rust_gc_allocator_block_count(m_allocator);
}

HeapBlock* CellAllocator::block_at(size_t index) const
{
    return static_cast<HeapBlock*>(FFI::rust_gc_allocator_block_at(m_allocator, index));
}

bool CellAllocator::is_local_block(HeapBlock const& block) const
{
    return FFI::rust_gc_allocator_local_block(m_allocator) == &block;
}

bool CellAllocator::has_blocks_pending_sweep() const
{
    return FFI::rust_gc_allocator_has_blocks_pending_sweep(m_allocator);
}

}
