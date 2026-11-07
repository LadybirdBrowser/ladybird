/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/FFIHelpers.h>
#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <AK/kmalloc.h>

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__) || defined(HAS_THREAD_SANITIZER)
// LeakSanitizer does not reliably trace references stored in mimalloc-managed
// AK containers, and ThreadSanitizer cannot see memory being freed on one thread
// and reused on another inside mimalloc, so sanitizer builds fall back to the
// system allocator.
#    define AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED 1
#else
#    include <mimalloc.h>
#    ifdef AK_OS_MACOS
#        include <mach/vm_statistics.h>
#    endif
#endif

#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED

#    ifdef AK_OS_MACOS
#        include <malloc/malloc.h>
#    else
#        include <malloc.h>
#    endif

void* ak_kcalloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void* ak_kmalloc(size_t size)
{
    return malloc(size);
}

void* ak_kmalloc(HeapPartition, size_t size)
{
    return ak_kmalloc(size);
}

void* ak_kmalloc_aligned(size_t size, size_t alignment)
{
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0)
        return nullptr;
    return ptr;
}

void* ak_kmalloc_aligned(HeapPartition, size_t size, size_t alignment)
{
    return ak_kmalloc_aligned(size, alignment);
}

void* ak_krealloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

void* ak_krealloc(HeapPartition, void* ptr, size_t size)
{
    return ak_krealloc(ptr, size);
}

size_t ak_kmalloc_usable_size(void const* ptr)
{
#    ifdef AK_OS_MACOS
    return malloc_size(ptr);
#    else
    return malloc_usable_size(const_cast<void*>(ptr));
#    endif
}

void ak_kfree(void* ptr)
{
    free(ptr);
}

void ak_kmalloc_collect()
{
}

void* ak_enter_scratch_heap()
{
    return nullptr;
}

void ak_leave_scratch_heap(void*)
{
}

extern "C" void ladybird_did_start_short_lived_thread()
{
}

extern "C" void ladybird_did_end_short_lived_thread()
{
}

#else

#    if defined(AK_OS_LINUX) || defined(AK_OS_MACOS)
static struct MimallocConfiguration {
    MimallocConfiguration()
    {
#        ifdef AK_OS_MACOS
        // Keep mimalloc allocations separate from IOAccelerator, which uses mimalloc's default tag of 100.
        mi_option_set_default(mi_option_os_tag, VM_MEMORY_APPLICATION_SPECIFIC_1);
#        else
        // mimalloc otherwise purges with MADV_DONTNEED, and every later reuse of a purged page takes a page fault.
        mi_option_set_default(mi_option_purge_decommits, 0);
#        endif
    }
} s_mimalloc_configuration;
#    endif

void* ak_kcalloc(size_t count, size_t size)
{
    return mi_calloc(count, size);
}

void* ak_kmalloc(size_t size)
{
    return mi_malloc(size);
}

// A mimalloc heap belongs to the thread that made it: only that thread may allocate from it, but any thread may free
// its memory. When the thread exits, mimalloc deletes its heaps, and what is still allocated stays valid until freed.
static constexpr size_t PARTITION_COUNT = to_underlying(HeapPartition::String) + 1;
AK_TLS_INITIAL_EXEC static thread_local constinit mi_heap_t* s_partition_heaps[PARTITION_COUNT] {};
static thread_local mi_heap_t* s_scratch_heap = nullptr;

static mi_heap_t* heap_for_partition(HeapPartition partition)
{
    if (partition == HeapPartition::General)
        return mi_heap_get_default();
    auto*& heap = s_partition_heaps[to_underlying(partition)];
    if (!heap) [[unlikely]]
        heap = mi_heap_new();
    return heap;
}

void* ak_kmalloc(HeapPartition partition, size_t size)
{
    return mi_heap_malloc(heap_for_partition(partition), size);
}

void* ak_kmalloc_aligned(size_t size, size_t alignment)
{
    return mi_malloc_aligned(size, alignment);
}

void* ak_kmalloc_aligned(HeapPartition partition, size_t size, size_t alignment)
{
    return mi_heap_malloc_aligned(heap_for_partition(partition), size, alignment);
}

void* ak_krealloc(void* ptr, size_t size)
{
    return mi_realloc(ptr, size);
}

void* ak_krealloc(HeapPartition partition, void* ptr, size_t size)
{
    return mi_heap_realloc(heap_for_partition(partition), ptr, size);
}

size_t ak_kmalloc_usable_size(void const* ptr)
{
    return mi_usable_size(ptr);
}

void ak_kfree(void* ptr)
{
    mi_free(ptr);
}

void ak_kmalloc_collect()
{
    // NB: In a scratch heap, mi_collect() would collect that heap again instead of the thread's own.
    VERIFY(!s_scratch_heap || mi_heap_get_default() != s_scratch_heap);

    // mi_collect() only visits the calling thread's default heap, so its partitions have to be collected separately.
    for (auto* heap : s_partition_heaps) {
        if (heap)
            mi_heap_collect(heap, true);
    }
    if (s_scratch_heap)
        mi_heap_collect(s_scratch_heap, true);

    mi_collect(true);
}

void* ak_enter_scratch_heap()
{
    if (!s_scratch_heap)
        s_scratch_heap = mi_heap_new();
    // NB: Without a heap of its own, the work allocates from the thread's heap as usual.
    if (!s_scratch_heap)
        return mi_heap_get_default();
    return mi_heap_set_default(s_scratch_heap);
}

void ak_leave_scratch_heap(void* previous_heap)
{
    mi_heap_set_default(static_cast<mi_heap_t*>(previous_heap));
}

// MADV_FREE leaves the pages in this process until the system runs short of memory. That pays off when they get reused
// soon, which is not what happens to the memory of a thread that ended. mimalloc reads this option each time it purges.
// NB: A thread that starts while another ends can leave the wrong value behind, until the next one of them. All that
//     costs is the choice between two ways to purge.
#    ifdef AK_OS_LINUX
static AK::Atomic<size_t> s_short_lived_thread_count { 0 };
#    endif

extern "C" void ladybird_did_start_short_lived_thread()
{
#    ifdef AK_OS_LINUX
    if (s_short_lived_thread_count.fetch_add(1) == 0)
        mi_option_set(mi_option_purge_decommits, 1);
#    endif
}

extern "C" void ladybird_did_end_short_lived_thread()
{
#    ifdef AK_OS_LINUX
    if (s_short_lived_thread_count.fetch_sub(1) == 1)
        mi_option_set(mi_option_purge_decommits, 0);
#    endif
}

#endif
