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

BlockAllocator::~BlockAllocator()
{
    for (auto* block : m_blocks) {
        ASAN_UNPOISON_MEMORY_REGION(block, HeapBlock::BLOCK_SIZE);
#if defined(AK_OS_MACOS)
        kern_return_t kr = mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(block), HeapBlock::BLOCK_SIZE);
        VERIFY(kr == KERN_SUCCESS);
#elif defined(AK_OS_WINDOWS)
        if (!VirtualFree(block, 0, MEM_RELEASE)) {
            warnln("{}", Error::from_windows_error());
            VERIFY_NOT_REACHED();
        }
#else
        free(block);
#endif
    }
}

void* BlockAllocator::allocate_block([[maybe_unused]] char const* name)
{
    if (!m_blocks.is_empty()) {
        // To reduce predictability, take a random block from the cache.
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

#if defined(AK_OS_MACOS)
    mach_vm_address_t address = 0;
    kern_return_t kr = mach_vm_map(
        mach_task_self(),
        &address,
        HeapBlock::BLOCK_SIZE,
        HeapBlock::BLOCK_SIZE - 1,
        VM_FLAGS_ANYWHERE,
        MEMORY_OBJECT_NULL,
        0,
        false,
        VM_PROT_READ | VM_PROT_WRITE,
        VM_PROT_READ | VM_PROT_WRITE,
        VM_INHERIT_DEFAULT);
    VERIFY(kr == KERN_SUCCESS);
    auto* block = reinterpret_cast<void*>(address);
#elif defined(AK_OS_WINDOWS)
    auto* block = VirtualAlloc(NULL, HeapBlock::BLOCK_SIZE, MEM_COMMIT, PAGE_READWRITE);
    VERIFY(block);
#else
    void* block = nullptr;
    auto rc = posix_memalign(&block, HeapBlock::BLOCK_SIZE, HeapBlock::BLOCK_SIZE);
    VERIFY(rc == 0);
#endif
    LSAN_REGISTER_ROOT_REGION(block, HeapBlock::BLOCK_SIZE);
    return block;
}

void BlockAllocator::deallocate_block(void* block)
{
    VERIFY(block);

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
