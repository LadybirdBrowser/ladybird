/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/Time.h>
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

// Every BlockAllocator takes 16 KiB HeapBlock slots from one process-wide
// reserved region, which commits them 2 MiB at a time. A slot that was never
// used belongs to nobody, so the allocators can all take their slots from the
// same chunk, and a pool for a type that only needs one block costs one block.
// A slot that was used stays with the allocator that took it.
//
// Per-block madvise() is deferred to a single global background "decommit
// worker" so it never costs us GC pause time, and slots that are recycled
// before the worker sees them skip the madvise pair entirely.
static constexpr size_t CHUNK_SIZE = 2 * MiB;
static constexpr size_t BLOCKS_PER_CHUNK = CHUNK_SIZE / HeapBlock::BLOCK_SIZE;

// How long a block with MADV_FREE'd pages can sit unused before its pages go back for real. This is longer than most
// heaps take between collections, so that blocks they reuse keep skipping the page faults. And as the collections of
// every heap drive the decommit worker, it keeps a busy heap from making the blocks of a slower one look cold.
static Atomic<i64, AK::MemoryOrder::memory_order_relaxed> s_cold_block_age_in_milliseconds { 5000 };

static AK::Duration cold_block_age()
{
    return AK::Duration::from_milliseconds(s_cold_block_age_in_milliseconds.load());
}

static_assert((HeapBlock::BLOCK_SIZE & (HeapBlock::BLOCK_SIZE - 1)) == 0);
static_assert(CHUNK_SIZE % HeapBlock::BLOCK_SIZE == 0);
static_assert(BLOCKS_PER_CHUNK == 128);
static_assert(HEAP_REGION_LOW_GUARD_SIZE % CHUNK_SIZE == 0);
static_assert(HEAP_REGION_HIGH_GUARD_SIZE % CHUNK_SIZE == 0);

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

        m_base = reinterpret_cast<u8*>(aligned_start);
        VERIFY(reinterpret_cast<FlatPtr>(m_base) % size == 0);
        publish_base(reinterpret_cast<FlatPtr>(m_base));
    }

    static void register_base_copy(HeapRegionBaseCopyRegistration& registration)
    {
        MutexLocker locker(base_copies_mutex());
        registration.m_next = exchange(s_base_copies, &registration);
        if (js_heap_region_base)
            *registration.m_copy = js_heap_region_base;
    }

    // This process-global region is shared by every Heap, and heaps allocate blocks on their own threads while the
    // decommit worker prepares the next chunk ahead of time, so the chunk bookkeeping is under a lock.
    void* take_unused_block()
    {
        u8* block = nullptr;
        bool took_chunk = false;
        {
            MutexLocker locker(m_mutex);
            if (m_next_unused_block == m_unused_blocks_end) {
                take_chunk();
                took_chunk = true;
            }
            block = m_next_unused_block;
            m_next_unused_block += HeapBlock::BLOCK_SIZE;
        }
        if (took_chunk)
            request_prepared_chunk();
        return block;
    }

    // Commits the next chunk and faults its pages in, so that the first touches of its blocks take no page faults
    // on the thread that allocates them. Runs on the decommit worker.
    void prepare_chunk()
    {
        u8* chunk = nullptr;
        {
            MutexLocker locker(m_mutex);
            // NB: Running out of the region is left to take_unused_block(), on the thread that needs another chunk.
            if (m_prepared_chunk || m_next_chunk_offset > LAST_CHUNK_OFFSET)
                return;
            chunk = commit_next_chunk();
        }
#if defined(MADV_POPULATE_WRITE)
        // NB: This only fails if the kernel is too old to know it, which leaves the faults to the first touches.
        (void)madvise(chunk, CHUNK_SIZE, MADV_POPULATE_WRITE);
#endif
        MutexLocker locker(m_mutex);
        m_prepared_chunk = chunk;
    }

    FlatPtr start() const { return reinterpret_cast<FlatPtr>(m_base); }
    FlatPtr end() const { return start() + size; }

    FlatPtr committed_end()
    {
        MutexLocker locker(m_mutex);
        return start() + m_next_chunk_offset;
    }

private:
    static void request_prepared_chunk();

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

    // The guards at either end of the region are never committed.
    static constexpr size_t FIRST_CHUNK_OFFSET = HEAP_REGION_LOW_GUARD_SIZE;
    static constexpr size_t LAST_CHUNK_OFFSET = size - HEAP_REGION_HIGH_GUARD_SIZE - CHUNK_SIZE;

    u8* commit_next_chunk()
    {
        VERIFY(m_next_chunk_offset >= FIRST_CHUNK_OFFSET);
        VERIFY(m_next_chunk_offset <= LAST_CHUNK_OFFSET);
        auto* chunk = m_base + m_next_chunk_offset;
        MUST(Core::System::commit_memory(chunk, CHUNK_SIZE, Core::System::MemoryTag::GarbageCollector));
        m_next_chunk_offset += CHUNK_SIZE;
        return chunk;
    }

    void take_chunk()
    {
        auto* chunk = exchange(m_prepared_chunk, nullptr);
        if (!chunk)
            chunk = commit_next_chunk();

#if defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
        // Mark the whole chunk reusable upfront so MADV_FREE_REUSE pairs
        // symmetrically when its blocks are handed out. (Linux and Windows
        // fall through with no-op.)
        if (madvise(chunk, CHUNK_SIZE, MADV_FREE_REUSABLE) < 0) {
            perror("madvise(MADV_FREE_REUSABLE)");
            VERIFY_NOT_REACHED();
        }
#endif

        ASAN_POISON_MEMORY_REGION(chunk, CHUNK_SIZE);

        m_next_unused_block = chunk;
        m_unused_blocks_end = chunk + CHUNK_SIZE;
    }

    u8* m_base { nullptr };
    Mutex m_mutex;
    size_t m_next_chunk_offset { FIRST_CHUNK_OFFSET };
    // A committed chunk whose pages are faulted in already, which is the next one that blocks come from.
    u8* m_prepared_chunk { nullptr };
    // The blocks of the current chunk that were never used.
    u8* m_next_unused_block { nullptr };
    u8* m_unused_blocks_end { nullptr };
};

static HeapRegion& heap_region()
{
    static AK::NeverDestroyed<HeapRegion> region;
    return *region;
}

static void madvise_block_for_decommit(void* block)
{
#if defined(AK_OS_WINDOWS)
    // NB: DiscardVirtualMemory() validates its range with VirtualQuery(), which makes the kernel check every page in
    //     the run of identical pages that starts at the block. All chunks are committed back to back in one
    //     reservation, so that run is the whole committed heap. MEM_RESET drops the contents without that walk, and
    //     VirtualUnlock() on pages that are not locked takes them out of the working set right away.
    if (!VirtualAlloc(block, HeapBlock::BLOCK_SIZE, MEM_RESET, PAGE_READWRITE)) {
        warnln("{}", Error::from_windows_error());
        VERIFY_NOT_REACHED();
    }
    (void)VirtualUnlock(block, HeapBlock::BLOCK_SIZE);
#elif defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
    // macOS uses the FREE_REUSABLE/FREE_REUSE paired protocol, which integrates
    // with its RSS accounting properly.
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE_REUSABLE) < 0) {
        perror("madvise(MADV_FREE_REUSABLE)");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_FREE)
    // MADV_FREE keeps the pages mapped until the kernel needs them, so a block reused before then takes no page fault.
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE) < 0) {
        perror("madvise(MADV_FREE)");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_DONTNEED)
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_DONTNEED) < 0) {
        perror("madvise(MADV_DONTNEED)");
        VERIFY_NOT_REACHED();
    }
#endif
}

static void return_block_to_system(void* block)
{
#if defined(AK_OS_LINUX)
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_DONTNEED) < 0) {
        perror("madvise(MADV_DONTNEED)");
        VERIFY_NOT_REACHED();
    }
#else
    madvise_block_for_decommit(block);
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
    static DecommitWorker* the_if_started() { return s_the.load(AK::MemoryOrder::memory_order_acquire); }

    void register_pending(BlockAllocator&);
    void deregister(BlockAllocator&);
    void kick();
    void request_prepared_chunk();

    DecommitWorker();

private:
    enum class ReturnColdBlocks {
        No,
        Yes,
    };

    void run();
    void process_one(BlockAllocator&, ReturnColdBlocks);

    Mutex m_mutex;
    ConditionVariable m_cv { m_mutex };
    RefPtr<Threading::Thread> m_thread;
    Vector<BlockAllocator*> m_pending;
    bool m_kicked { false };
    bool m_chunk_requested { false };
    MonotonicTime m_last_return_of_cold_blocks { MonotonicTime::now_coarse() };

    static Atomic<DecommitWorker*> s_the;
};

Atomic<DecommitWorker*> DecommitWorker::s_the { nullptr };

void HeapRegion::request_prepared_chunk()
{
    // NB: Preparing a chunk only pays off where its pages can be faulted in ahead of time. And only a worker that is
    //     already running prepares chunks: starting it here could start a second thread before the process is
    //     sandboxed, which some sandboxes do not allow.
#if defined(MADV_POPULATE_WRITE)
    if (auto* worker = DecommitWorker::the_if_started())
        worker->request_prepared_chunk();
#endif
}

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
    s_the.store(this, AK::MemoryOrder::memory_order_release);
}

void DecommitWorker::register_pending(BlockAllocator& a)
{
    MutexLocker locker(m_mutex);
    m_pending.append(&a);
}

void DecommitWorker::deregister(BlockAllocator& a)
{
    MutexLocker locker(m_mutex);
    m_pending.remove_first_matching([&](auto* p) { return p == &a; });
}

void DecommitWorker::kick()
{
    {
        MutexLocker locker(m_mutex);
        m_kicked = true;
    }
    m_cv.signal();
}

void DecommitWorker::request_prepared_chunk()
{
    {
        MutexLocker locker(m_mutex);
        m_chunk_requested = true;
    }
    m_cv.signal();
}

void DecommitWorker::run()
{
    while (true) {
        Vector<BlockAllocator*> snapshot;
        bool chunk_requested = false;
        {
            MutexLocker locker(m_mutex);
            while (!m_kicked && !m_chunk_requested) {
                // NB: Collections may never come again, and what is on the list must not wait for one. Each block
                //     only takes a few rounds of this, after which the list is empty and this thread sleeps for good.
                if (m_pending.is_empty())
                    m_cv.wait();
                else if (!m_cv.wait_for(cold_block_age()))
                    m_kicked = true;
            }
            chunk_requested = exchange(m_chunk_requested, false);
            if (m_kicked) {
                m_kicked = false;
                snapshot = move(m_pending);
                // Pin every allocator we're about to process so destructors
                // block until we drop our reference.
                for (auto* a : snapshot)
                    a->m_worker_refcount.fetch_add(1);
            }
        }

        if (chunk_requested)
            heap_region().prepare_chunk();

        if (snapshot.is_empty())
            continue;

        // Stagger: give the JS thread some breathing room after the kick
        // (typically right after sweep ends) before we consume CPU and
        // syscall bandwidth.
        sleep_before_decommit();

        auto return_cold_blocks = ReturnColdBlocks::No;
        if (auto now = MonotonicTime::now_coarse(); now - m_last_return_of_cold_blocks >= cold_block_age()) {
            return_cold_blocks = ReturnColdBlocks::Yes;
            m_last_return_of_cold_blocks = now;
        }

        for (auto* a : snapshot) {
            process_one(*a, return_cold_blocks);
            int prev_refcount = a->m_worker_refcount.fetch_sub(1);
            if (prev_refcount == 1) {
                MutexLocker locker(a->m_mutex);
                a->m_worker_cv.broadcast();
            }
        }
    }
}

void DecommitWorker::process_one(BlockAllocator& a, [[maybe_unused]] ReturnColdBlocks return_cold_blocks)
{
    Vector<void*> to_process;
    Vector<void*> cold_blocks;
    {
        MutexLocker locker(a.m_mutex);
        a.m_in_decommit_registry = false;
        to_process = move(a.m_freshly_freed);
#if defined(AK_OS_LINUX)
        // Blocks that went through MADV_FREE stay in the process until the system runs short of memory, and only their
        // own type can reuse them. What that type did not reuse since the last time will likely stay unused.
        if (return_cold_blocks == ReturnColdBlocks::Yes)
            cold_blocks = move(a.m_blocks);
#endif
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
    for (size_t i = 0; i < cold_blocks.size(); ++i) {
        return_block_to_system(cold_blocks[i]);
        if ((i + 1) % BATCH == 0)
            yield_during_decommit();
    }

    bool need_to_register = false;
    {
        MutexLocker locker(a.m_mutex);
        a.m_blocks.extend(move(to_process));
        a.m_returned_blocks.extend(move(cold_blocks));
#if defined(AK_OS_LINUX)
        // NB: So an allocator is only on the list while it has blocks to look at, however many allocators there are.
        if (!a.m_blocks.is_empty() && !a.m_in_decommit_registry) {
            a.m_in_decommit_registry = true;
            need_to_register = true;
        }
#endif
    }
    if (need_to_register)
        register_pending(a);
}

void BlockAllocator::wake_decommit_worker_async()
{
    DecommitWorker::the().kick();
}

AK::Duration BlockAllocator::set_cold_block_age_for_testing(AK::Duration age)
{
    return AK::Duration::from_milliseconds(s_cold_block_age_in_milliseconds.exchange(age.to_milliseconds()));
}

void BlockAllocator::return_freed_blocks_to_system()
{
    Vector<void*> blocks;
    {
        MutexLocker locker(m_mutex);
        blocks = move(m_freshly_freed);
#if defined(AK_OS_LINUX)
        // NB: MADV_FREE leaves these pages in the process until the system runs short of memory.
        blocks.extend(move(m_blocks));
#endif
    }

    for (auto* block : blocks)
        return_block_to_system(block);

    MutexLocker locker(m_mutex);
    m_returned_blocks.extend(move(blocks));
}

BlockAllocator::BlockAllocator()
    : m_worker_cv(m_mutex)
{
}

BlockAllocator::~BlockAllocator()
{
    // Chunks are permanent -- we never tear them down. The destructor only
    // exists to make sure the global decommit worker has finished any
    // in-flight processing of *this before our storage goes away. That
    // processing can put us back on its list, so look again after it.
    while (true) {
        DecommitWorker::the().deregister(*this);

        MutexLocker locker(m_mutex);
        while (m_worker_refcount.load() != 0)
            m_worker_cv.wait();
        if (!exchange(m_in_decommit_registry, false))
            break;
    }
}

size_t BlockAllocator::block_count()
{
    MutexLocker locker(m_mutex);
    return m_blocks.size() + m_returned_blocks.size();
}

size_t BlockAllocator::returned_block_count_for_testing()
{
    MutexLocker locker(m_mutex);
    return m_returned_blocks.size();
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
    return heap_region().committed_end();
}

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    void* block = nullptr;
    bool needs_madvise_reuse = false;

    {
        MutexLocker locker(m_mutex);

        // Prefer m_freshly_freed: those slots were never madvised, so we
        // can hand them back out with zero syscalls. This is the deferred-
        // decommit payoff -- hot recycle skips both MADV_FREE_REUSABLE
        // and MADV_FREE_REUSE.
        if (!m_freshly_freed.is_empty()) {
            block = m_freshly_freed.take_last();
        } else if (!m_blocks.is_empty()) {
            block = m_blocks.take_last();
            needs_madvise_reuse = true;
        } else if (!m_returned_blocks.is_empty()) {
            block = m_returned_blocks.take_last();
            needs_madvise_reuse = true;
        }
    }

    if (block == nullptr) {
        block = heap_region().take_unused_block();
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
        MutexLocker locker(m_mutex);
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
