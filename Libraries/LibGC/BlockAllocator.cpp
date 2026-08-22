/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/Try.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/HeapRegion.h>
#include <LibThreading/Thread.h>
#include <sys/mman.h>

#ifdef HAS_ADDRESS_SANITIZER
#    include <sanitizer/asan_interface.h>
#    include <sanitizer/lsan_interface.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <memoryapi.h>
#else
#    include <sched.h>
#    include <unistd.h>
#endif

extern "C" {
GC_API FlatPtr js_heap_region_base = 0;
}

namespace GC {

// Each BlockAllocator gets type-isolated 2 MiB chunks from one process-wide
// reserved region and carves its 16 KiB HeapBlock slots out of those chunks.
//
// Per-block madvise() is deferred to a single global background "decommit
// worker" so it never costs us GC pause time, and slots that are recycled
// before the worker sees them skip the madvise pair entirely.
static constexpr size_t CHUNK_SIZE = 2 * MiB;
static constexpr size_t BLOCKS_PER_CHUNK = CHUNK_SIZE / HeapBlock::BLOCK_SIZE;
static_assert((HeapBlock::BLOCK_SIZE & (HeapBlock::BLOCK_SIZE - 1)) == 0);
static_assert(CHUNK_SIZE % HeapBlock::BLOCK_SIZE == 0);
static_assert(BLOCKS_PER_CHUNK == 128);

class HeapRegion {
public:
    static constexpr size_t size = HEAP_REGION_SIZE;

    HeapRegion()
    {
        static_assert(sizeof(FlatPtr) >= sizeof(u64));
        Checked<size_t> reservation_size = size;
        reservation_size += size;
        VERIFY(!reservation_size.has_overflow());

        auto* reservation = MUST(Core::System::reserve_address_space(reservation_size.value()));
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

        m_base = reinterpret_cast<u8*>(aligned_start);
        VERIFY(reinterpret_cast<FlatPtr>(m_base) % size == 0);
        js_heap_region_base = reinterpret_cast<FlatPtr>(m_base);
    }

    void* allocate_chunk()
    {
        // This process-global region is shared by every Heap. GC block allocation is single-threaded
        // per process, so m_next_chunk_offset intentionally needs no lock. Supporting another allocating
        // thread would require synchronization here or per-thread regions.
        VERIFY(m_next_chunk_offset <= size - CHUNK_SIZE);
        auto* chunk = m_base + m_next_chunk_offset;
        MUST(Core::System::commit_memory(chunk, CHUNK_SIZE));
        m_next_chunk_offset += CHUNK_SIZE;
        return chunk;
    }

    FlatPtr start() const { return reinterpret_cast<FlatPtr>(m_base); }
    FlatPtr end() const { return start() + size; }

private:
    u8* m_base { nullptr };
    size_t m_next_chunk_offset { 0 };
};

static HeapRegion& heap_region()
{
    static AK::NeverDestroyed<HeapRegion> region;
    return *region;
}

static void madvise_block_for_decommit(void* block)
{
#if defined(AK_OS_WINDOWS)
    DWORD ret = DiscardVirtualMemory(block, HeapBlock::BLOCK_SIZE);
    if (ret != ERROR_SUCCESS) {
        warnln("{}", Error::from_windows_error(ret));
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
    // macOS uses the FREE_REUSABLE/FREE_REUSE paired protocol, which integrates
    // with its RSS accounting properly.
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE_REUSABLE) < 0) {
        perror("madvise(MADV_FREE_REUSABLE)");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_DONTNEED)
    // Prefer DONTNEED over FREE on Linux: FREE is lazy and only releases pages
    // under memory pressure, which leaves freed blocks counted in RSS for
    // arbitrarily long after a busy page goes idle.
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_DONTNEED) < 0) {
        perror("madvise(MADV_DONTNEED)");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_FREE)
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE) < 0) {
        perror("madvise(MADV_FREE)");
        VERIFY_NOT_REACHED();
    }
#endif
}

static void sleep_before_decommit()
{
#if defined(AK_OS_WINDOWS)
    Sleep(50);
#else
    usleep(50 * 1000);
#endif
}

static void yield_during_decommit()
{
#if defined(AK_OS_WINDOWS)
    Sleep(0);
#else
    sched_yield();
#endif
}

class DecommitWorker {
public:
    static DecommitWorker& the();

    void register_pending(BlockAllocator&);
    void deregister(BlockAllocator&);
    void kick();

    DecommitWorker();

private:
    void run();
    void process_one(BlockAllocator&);

    Sync::Mutex m_mutex;
    Sync::ConditionVariable m_cv { m_mutex };
    RefPtr<Threading::Thread> m_thread;
    Vector<BlockAllocator*> m_pending;
    bool m_kicked { false };
};

DecommitWorker& DecommitWorker::the()
{
    static AK::NeverDestroyed<DecommitWorker> instance;
    return *instance;
}

DecommitWorker::DecommitWorker()
{
    m_thread = Threading::Thread::construct("DecommitWorker"sv, [this] {
        run();
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
    m_thread->detach();
}

void DecommitWorker::register_pending(BlockAllocator& a)
{
    Sync::MutexLocker locker(m_mutex);
    m_pending.append(&a);
}

void DecommitWorker::deregister(BlockAllocator& a)
{
    Sync::MutexLocker locker(m_mutex);
    m_pending.remove_first_matching([&](auto* p) { return p == &a; });
}

void DecommitWorker::kick()
{
    {
        Sync::MutexLocker locker(m_mutex);
        m_kicked = true;
    }
    m_cv.signal();
}

void DecommitWorker::run()
{
    while (true) {
        Vector<BlockAllocator*> snapshot;
        {
            Sync::MutexLocker locker(m_mutex);
            while (!m_kicked)
                m_cv.wait();
            m_kicked = false;
            snapshot = move(m_pending);
            // Pin every allocator we're about to process so destructors
            // block until we drop our reference.
            for (auto* a : snapshot)
                a->m_worker_refcount.fetch_add(1);
        }

        if (snapshot.is_empty())
            continue;

        // Stagger: give the JS thread some breathing room after the kick
        // (typically right after sweep ends) before we consume CPU and
        // syscall bandwidth.
        sleep_before_decommit();

        for (auto* a : snapshot) {
            process_one(*a);
            int prev_refcount = a->m_worker_refcount.fetch_sub(1);
            if (prev_refcount == 1) {
                Sync::MutexLocker locker(a->m_mutex);
                a->m_worker_cv.broadcast();
            }
        }
    }
}

void DecommitWorker::process_one(BlockAllocator& a)
{
    Vector<void*> to_process;
    {
        Sync::MutexLocker locker(a.m_mutex);
        a.m_in_decommit_registry = false;
        to_process = move(a.m_freshly_freed);
    }

    // Madvise each slot outside the per-allocator lock so the JS thread can
    // continue to allocate/free; yield every 64 slots to avoid hogging the
    // kernel's mm subsystem.
    constexpr size_t BATCH = 64;
    for (size_t i = 0; i < to_process.size(); ++i) {
        madvise_block_for_decommit(to_process[i]);
        if ((i + 1) % BATCH == 0)
            yield_during_decommit();
    }

    {
        Sync::MutexLocker locker(a.m_mutex);
        for (auto* slot : to_process)
            a.m_blocks.append(slot);
    }
}

void BlockAllocator::wake_decommit_worker_async()
{
    DecommitWorker::the().kick();
}

BlockAllocator::BlockAllocator()
    : m_worker_cv(m_mutex)
{
}

BlockAllocator::~BlockAllocator()
{
    // Chunks are permanent -- we never tear them down. The destructor only
    // exists to make sure the global decommit worker has finished any
    // in-flight processing of *this before our storage goes away.
    DecommitWorker::the().deregister(*this);

    Sync::MutexLocker locker(m_mutex);
    while (m_worker_refcount.load() != 0)
        m_worker_cv.wait();
}

size_t BlockAllocator::block_count()
{
    Sync::MutexLocker locker(m_mutex);
    return m_blocks.size();
}

FlatPtr BlockAllocator::heap_region_start()
{
    return heap_region().start();
}

FlatPtr BlockAllocator::heap_region_end()
{
    return heap_region().end();
}

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    void* block = nullptr;
    bool needs_madvise_reuse = false;

    {
        Sync::MutexLocker locker(m_mutex);

        // Prefer m_freshly_freed: those slots were never madvised, so we
        // can hand them back out with zero syscalls. This is the deferred-
        // decommit payoff -- hot recycle skips both MADV_FREE_REUSABLE
        // and MADV_FREE_REUSE.
        if (!m_freshly_freed.is_empty()) {
            block = m_freshly_freed.take_last();
        } else if (!m_blocks.is_empty()) {
            block = m_blocks.take_last();
            needs_madvise_reuse = true;
        }
    }

    if (block == nullptr) {
        // Both pools empty: commit a fresh 2 MiB chunk and slice it.
        auto* chunk_base = heap_region().allocate_chunk();

#if defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
        // Mark the whole chunk reusable upfront so MADV_FREE_REUSE pairs
        // symmetrically when slots are popped from m_blocks later. (Linux
        // and Windows fall through with no-op.)
        if (madvise(chunk_base, CHUNK_SIZE, MADV_FREE_REUSABLE) < 0) {
            perror("madvise(MADV_FREE_REUSABLE)");
            VERIFY_NOT_REACHED();
        }
#endif

        ASAN_POISON_MEMORY_REGION(chunk_base, CHUNK_SIZE);

        Sync::MutexLocker locker(m_mutex);
        for (size_t i = 0; i < BLOCKS_PER_CHUNK; ++i)
            m_blocks.append(static_cast<u8*>(chunk_base) + i * HeapBlock::BLOCK_SIZE);
        block = m_blocks.take_last();
        needs_madvise_reuse = true;
    }

    ASAN_UNPOISON_MEMORY_REGION(block, HeapBlock::BLOCK_SIZE);
    LSAN_REGISTER_ROOT_REGION(block, HeapBlock::BLOCK_SIZE);
#if defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
    if (needs_madvise_reuse) {
        if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE_REUSE) < 0) {
            perror("madvise(MADV_FREE_REUSE)");
            VERIFY_NOT_REACHED();
        }
    }
#else
    (void)needs_madvise_reuse;
#endif
    return block;
}

void BlockAllocator::deallocate_block(void* block, DeferDecommit defer_decommit)
{
    VERIFY(block);

    // Fast path: bookkeep only. The actual madvise is deferred to the
    // global decommit worker, which the GC kicks at the end of sweep.
    ASAN_POISON_MEMORY_REGION(block, HeapBlock::BLOCK_SIZE);
    LSAN_UNREGISTER_ROOT_REGION(block, HeapBlock::BLOCK_SIZE);

    bool need_to_register = false;
    {
        Sync::MutexLocker locker(m_mutex);
        m_freshly_freed.append(block);
        if (defer_decommit == DeferDecommit::Yes && !m_in_decommit_registry) {
            m_in_decommit_registry = true;
            need_to_register = true;
        }
    }
    if (need_to_register && defer_decommit == DeferDecommit::Yes)
        DecommitWorker::the().register_pending(*this);
}

}
