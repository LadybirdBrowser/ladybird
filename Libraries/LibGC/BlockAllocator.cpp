/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/Vector.h>
#include <LibGC/BlockAllocator.h>
#include <LibGC/HeapBlock.h>
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
#endif

namespace GC {

// Each BlockAllocator carves its 16 KiB HeapBlock slots out of 2 MiB
// chunks, so the kernel sees one mmap per 128 blocks instead of one per
// block. Chunks are owned exclusively by a single BlockAllocator and are
// never released back to the OS or shared across allocators -- that's how
// we keep the heap's VM permanently type-isolated, where a virtual address
// used for a cell of type T is never reused for any other type.
//
// We do not hint MADV_HUGEPAGE: per-block madvise() in deallocate_block
// would split any THP backing the chunk anyway. Per-block memory return
// matches what V8, SpiderMonkey, and WebKit's libpas all do.
static constexpr size_t CHUNK_SIZE = 2 * MiB;
static constexpr size_t BLOCKS_PER_CHUNK = CHUNK_SIZE / HeapBlock::BLOCK_SIZE;
static_assert(CHUNK_SIZE % HeapBlock::BLOCK_SIZE == 0);
static_assert(BLOCKS_PER_CHUNK == 128);

BlockAllocator::~BlockAllocator() = default;

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    if (m_blocks.is_empty()) {
        void* chunk_base = nullptr;
#if defined(AK_OS_MACOS)
        mach_vm_address_t address = 0;
        kern_return_t kr = mach_vm_map(
            mach_task_self(),
            &address,
            CHUNK_SIZE,
            CHUNK_SIZE - 1,
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
        auto rc = posix_memalign(&chunk_base, CHUNK_SIZE, CHUNK_SIZE);
        VERIFY(rc == 0);
#endif

#if defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
        // Mark the whole chunk reusable upfront so MADV_FREE_REUSE pairs
        // symmetrically when slots are popped from m_blocks below. (Linux
        // and Windows fall through with no-op.)
        if (madvise(chunk_base, CHUNK_SIZE, MADV_FREE_REUSABLE) < 0) {
            perror("madvise(MADV_FREE_REUSABLE)");
            VERIFY_NOT_REACHED();
        }
#endif

        ASAN_POISON_MEMORY_REGION(chunk_base, CHUNK_SIZE);
        for (size_t i = 0; i < BLOCKS_PER_CHUNK; ++i)
            m_blocks.append(static_cast<u8*>(chunk_base) + i * HeapBlock::BLOCK_SIZE);
    }

    // Random pick to preserve the previous anti-predictability behavior.
    size_t random_index = get_random_uniform(m_blocks.size());
    auto* block = m_blocks.unstable_take(random_index);

    ASAN_UNPOISON_MEMORY_REGION(block, HeapBlock::BLOCK_SIZE);
    LSAN_REGISTER_ROOT_REGION(block, HeapBlock::BLOCK_SIZE);
#if defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE_REUSE) < 0) {
        perror("madvise(MADV_FREE_REUSE)");
        VERIFY_NOT_REACHED();
    }
#endif
    return block;
}

void BlockAllocator::deallocate_block(void* block)
{
    VERIFY(block);

    // Tell the kernel it can reclaim physical pages backing this 16 KiB
    // slot. The slot stays in m_blocks for reuse by this same
    // BlockAllocator -- never seen by a different cell type.
#if defined(AK_OS_WINDOWS)
    DWORD ret = DiscardVirtualMemory(block, HeapBlock::BLOCK_SIZE);
    if (ret != ERROR_SUCCESS) {
        warnln("{}", Error::from_windows_error(ret));
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_FREE_REUSE) && defined(MADV_FREE_REUSABLE)
    if (madvise(block, HeapBlock::BLOCK_SIZE, MADV_FREE_REUSABLE) < 0) {
        perror("madvise(MADV_FREE_REUSABLE)");
        VERIFY_NOT_REACHED();
    }
#elif defined(MADV_FREE)
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

    ASAN_POISON_MEMORY_REGION(block, HeapBlock::BLOCK_SIZE);
    LSAN_UNREGISTER_ROOT_REGION(block, HeapBlock::BLOCK_SIZE);
    m_blocks.append(block);
}

}
