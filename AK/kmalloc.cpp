/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/kmalloc.h>

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
// LeakSanitizer does not reliably trace references stored in mimalloc-managed
// AK containers, so sanitizer builds fall back to the system allocator.
#    define AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED 1
#else
#    include <mimalloc.h>
#endif

#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED

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

void* ak_krealloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

void* ak_krealloc(HeapPartition, void* ptr, size_t size)
{
    return ak_krealloc(ptr, size);
}

size_t ak_kmalloc_good_size(size_t size)
{
    return size;
}

void ak_kfree(void* ptr)
{
    free(ptr);
}

void ak_kmalloc_collect()
{
}

#else

#    ifdef AK_OS_LINUX
static struct MimallocConfiguration {
    MimallocConfiguration()
    {
        // mimalloc otherwise purges with MADV_DONTNEED, and every later reuse of a purged page takes a page fault.
        mi_option_set_default(mi_option_purge_decommits, 0);
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

static thread_local mi_heap_t* s_string_heap = nullptr;

static mi_heap_t* heap_for_partition(HeapPartition partition)
{
    switch (partition) {
    case HeapPartition::General:
        return mi_heap_get_default();
    case HeapPartition::ArrayBuffer:
        static mi_heap_t* array_buffer_heap = mi_heap_new();
        return array_buffer_heap;
    case HeapPartition::JSObjectStorage:
        static mi_heap_t* js_object_storage_heap = mi_heap_new();
        return js_object_storage_heap;
    case HeapPartition::Layout:
        static mi_heap_t* layout_heap = mi_heap_new();
        return layout_heap;
    case HeapPartition::String:
        if (!s_string_heap)
            s_string_heap = mi_heap_new();
        return s_string_heap;
    }
    VERIFY_NOT_REACHED();
}

void* ak_kmalloc(HeapPartition partition, size_t size)
{
    return mi_heap_malloc(heap_for_partition(partition), size);
}

void* ak_krealloc(void* ptr, size_t size)
{
    return mi_realloc(ptr, size);
}

void* ak_krealloc(HeapPartition partition, void* ptr, size_t size)
{
    return mi_heap_realloc(heap_for_partition(partition), ptr, size);
}

size_t ak_kmalloc_good_size(size_t size)
{
    return mi_good_size(size);
}

void ak_kfree(void* ptr)
{
    mi_free(ptr);
}

void ak_kmalloc_collect()
{
    // mi_collect() only visits the calling thread's default heap, so the string heap has to be collected separately.
    // The remaining partitions are shared between threads and are left to their owners.
    if (s_string_heap)
        mi_heap_collect(s_string_heap, true);

    mi_collect(true);
}

#endif
