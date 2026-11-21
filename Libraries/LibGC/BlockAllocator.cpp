/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/Time.h>
#include <AK/Try.h>
#include <LibCore/System.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/BlockDescriptor.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/HeapRegion.h>

extern "C" {
GC_API FlatPtr js_heap_region_base = 0;
}

namespace GC {

static_assert(FFI::CHUNK_SIZE % HeapBlock::BLOCK_SIZE == 0);
static_assert(HEAP_REGION_LOW_GUARD_SIZE % FFI::CHUNK_SIZE == 0);
static_assert(HEAP_REGION_HIGH_GUARD_SIZE % FFI::CHUNK_SIZE == 0);

// Every block of every heap is in one process-wide region, which this reserves at an address aligned to its size, so
// that a cell's offset in the region is its address modulo the region's size. LibGC's Rust core commits the region's
// chunks and hands out its blocks, see block_pool.rs.
class HeapRegion {
public:
    static constexpr size_t size = HEAP_REGION_SIZE;

    HeapRegion()
    {
        static_assert(sizeof(FlatPtr) >= sizeof(u64));
        Checked<size_t> reservation_size = size;
        reservation_size += size;
        VERIFY(!reservation_size.has_overflow());

        auto* reservation = MUST(Core::System::reserve_address_space(reservation_size.value(), Core::System::MemoryTag::GarbageCollector));
        auto reservation_start = reinterpret_cast<FlatPtr>(reservation);
        auto aligned_start = align_up_to(reservation_start, size);
        auto reservation_end = reservation_start + reservation_size.value();
        auto head_size = aligned_start - reservation_start;
        auto tail_size = reservation_end - (aligned_start + size);

#if !defined(AK_OS_WINDOWS)
        if (head_size != 0)
            MUST(Core::System::release_address_space(reservation, head_size));
        if (tail_size != 0)
            MUST(Core::System::release_address_space(reinterpret_cast<void*>(aligned_start + size), tail_size));
#else
        // VirtualFree with MEM_RELEASE can only release the complete
        // reservation, so keep the inaccessible head and tail on Windows.
        (void)head_size;
        (void)tail_size;
#endif

        m_base = aligned_start;
        VERIFY(m_base % size == 0);
        // NB: The guards at either end of the region are never committed.
        FFI::rust_gc_heap_region_init(reinterpret_cast<u8*>(m_base), HEAP_REGION_LOW_GUARD_SIZE, size - HEAP_REGION_HIGH_GUARD_SIZE - FFI::CHUNK_SIZE);
        publish_base(m_base);
    }

    static void register_base_copy(HeapRegionBaseCopyRegistration& registration)
    {
        MutexLocker locker(base_copies_mutex());
        registration.m_next = exchange(s_base_copies, &registration);
        if (js_heap_region_base)
            *registration.m_copy = js_heap_region_base;
    }

    FlatPtr start() const { return m_base; }
    FlatPtr end() const { return start() + size; }

private:
    static void publish_base(FlatPtr base)
    {
        MutexLocker locker(base_copies_mutex());
        js_heap_region_base = base;
        for (auto* registration = s_base_copies; registration; registration = registration->m_next)
            *registration->m_copy = base;
    }

    // NB: Static initialization registers copies, possibly before that of this file has run, so the list starts out
    //     constant-initialized and its mutex is set up on first use.
    static Mutex& base_copies_mutex()
    {
        static AK::NeverDestroyed<Mutex> mutex;
        return *mutex;
    }

    static constinit inline HeapRegionBaseCopyRegistration* s_base_copies { nullptr };

    FlatPtr m_base { 0 };
};

static HeapRegion& heap_region()
{
    static AK::NeverDestroyed<HeapRegion> region;
    return *region;
}

void BlockAllocator::wake_decommit_worker_async()
{
    FFI::rust_gc_wake_decommit_worker();
}

AK::Duration BlockAllocator::set_cold_block_age_for_testing(AK::Duration age)
{
    return AK::Duration::from_milliseconds(static_cast<i64>(FFI::rust_gc_set_cold_block_age_in_milliseconds(static_cast<u64>(age.to_milliseconds()))));
}

BlockAllocator::BlockAllocator()
    : m_pool(FFI::rust_gc_block_pool_create())
{
    // NB: LibGC's Rust core takes blocks from the pool directly, out of the region this reserves.
    (void)heap_region();
}

BlockAllocator::~BlockAllocator()
{
    FFI::rust_gc_block_pool_destroy(m_pool);
}

void BlockAllocator::return_freed_blocks_to_system()
{
    FFI::rust_gc_block_pool_return_freed_blocks_to_system(m_pool);
}

size_t BlockAllocator::block_count()
{
    return FFI::rust_gc_block_pool_block_count(m_pool);
}

size_t BlockAllocator::returned_block_count_for_testing()
{
    return FFI::rust_gc_block_pool_returned_block_count(m_pool);
}

HeapRegionBaseCopyRegistration::HeapRegionBaseCopyRegistration(FlatPtr& copy)
    : m_copy(&copy)
{
    HeapRegion::register_base_copy(*this);
}

FlatPtr BlockAllocator::heap_region_start()
{
    return heap_region().start();
}

FlatPtr BlockAllocator::heap_region_end()
{
    return heap_region().end();
}

FlatPtr BlockAllocator::heap_region_committed_end()
{
    (void)heap_region();
    return FFI::rust_gc_heap_region_committed_end();
}

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    (void)heap_region();
    return FFI::rust_gc_block_pool_allocate_block(m_pool);
}

void BlockAllocator::deallocate_block(void* block, DeferDecommit defer_decommit)
{
    VERIFY(block);
    FFI::rust_gc_block_pool_deallocate_block(m_pool, static_cast<u8*>(block), defer_decommit == DeferDecommit::Yes);
}

}
