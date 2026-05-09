/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/NeverDestroyed.h>
#include <AK/Platform.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/HeapBlock.h>
#include <LibThreading/Thread.h>
#include <sys/mman.h>

#if defined(AK_OS_MACOS)
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#endif

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

namespace GC {

// Each BlockAllocator carves its 16 KiB HeapBlock slots out of 2 MiB
// chunks. Chunks are owned exclusively by a single BlockAllocator and are
// never released back to the OS or shared across allocators -- the heap's
// VM is permanently type-isolated.
//
// Per-block madvise() is deferred to a single global background "decommit
// worker" so it never costs us GC pause time, and slots that are recycled
// before the worker sees them skip the madvise pair entirely.
static constexpr size_t CHUNK_SIZE = 2 * MiB;
static constexpr size_t BLOCKS_PER_CHUNK = CHUNK_SIZE / HeapBlock::BLOCK_SIZE;
static_assert((HeapBlock::BLOCK_SIZE & (HeapBlock::BLOCK_SIZE - 1)) == 0);
static_assert(CHUNK_SIZE % HeapBlock::BLOCK_SIZE == 0);
static_assert(BLOCKS_PER_CHUNK == 128);

#if !defined(AK_OS_MACOS) && !defined(AK_OS_WINDOWS)
static auto const s_page_size = [] {
    auto page_size_result = sysconf(_SC_PAGESIZE);
    VERIFY(page_size_result > 0);
    return static_cast<size_t>(page_size_result);
}();

static void* allocate_chunk_with_aligned_heap_blocks()
{
    auto const page_size = s_page_size;
    VERIFY((page_size & (page_size - 1)) == 0);
    VERIFY(HeapBlock::BLOCK_SIZE % page_size == 0 || page_size % HeapBlock::BLOCK_SIZE == 0);
    VERIFY(CHUNK_SIZE % page_size == 0);

    auto const extra_size = page_size < HeapBlock::BLOCK_SIZE ? HeapBlock::BLOCK_SIZE - page_size : 0;
    auto* mapped = mmap(nullptr, CHUNK_SIZE + extra_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    VERIFY(mapped != MAP_FAILED);

    auto const mapped_address = reinterpret_cast<FlatPtr>(mapped);
    auto const aligned_address = align_up_to(mapped_address, HeapBlock::BLOCK_SIZE);
    VERIFY(aligned_address % HeapBlock::BLOCK_SIZE == 0);

    auto const left_padding = aligned_address - mapped_address;
    if (left_padding > 0) {
        VERIFY(left_padding % page_size == 0);
        if (munmap(mapped, left_padding) < 0) {
            perror("munmap");
            VERIFY_NOT_REACHED();
        }
    }

    auto const mapped_end = mapped_address + CHUNK_SIZE + extra_size;
    auto const chunk_end = aligned_address + CHUNK_SIZE;
    VERIFY(chunk_end <= mapped_end);

    auto const right_padding = mapped_end - chunk_end;
    if (right_padding > 0) {
        VERIFY(right_padding % page_size == 0);
        if (munmap(reinterpret_cast<void*>(chunk_end), right_padding) < 0) {
            perror("munmap");
            VERIFY_NOT_REACHED();
        }
    }

    return reinterpret_cast<void*>(aligned_address);
}
#endif

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
        // Both pools empty: allocate a fresh 2 MiB chunk and slice it.
        void* chunk_base = nullptr;
#if defined(AK_OS_MACOS)
        mach_vm_address_t address = 0;
        kern_return_t kr = mach_vm_map(
            mach_task_self(),
            &address,
            CHUNK_SIZE,
            HeapBlock::BLOCK_SIZE - 1,
            VM_FLAGS_ANYWHERE,
            MEMORY_OBJECT_NULL,
            0,
            false,
            VM_PROT_READ | VM_PROT_WRITE,
            VM_PROT_READ | VM_PROT_WRITE,
            VM_INHERIT_DEFAULT);
        VERIFY(kr == KERN_SUCCESS);
        chunk_base = reinterpret_cast<void*>(address);
#elif defined(AK_OS_WINDOWS)
        chunk_base = VirtualAlloc(nullptr, CHUNK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        VERIFY(chunk_base);
#else
        chunk_base = allocate_chunk_with_aligned_heap_blocks();
#endif

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

void BlockAllocator::deallocate_block(void* block)
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
        if (!m_in_decommit_registry) {
            m_in_decommit_registry = true;
            need_to_register = true;
        }
    }
    if (need_to_register)
        DecommitWorker::the().register_pending(*this);
}

}
