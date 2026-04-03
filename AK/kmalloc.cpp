/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/kmalloc.h>

#if defined(AK_OS_SERENITY)

#    include <AK/Assertions.h>

// However deceptively simple these functions look, they must not be inlined.
// Memory allocated in one translation unit has to be deallocatable in another
// translation unit, so these functions must be the same everywhere.
// By making these functions global, this invariant is enforced.

void* operator new(size_t size)
{
    void* ptr = malloc(size);
    VERIFY(ptr);
    return ptr;
}

void* operator new(size_t size, std::nothrow_t const&) noexcept
{
    return malloc(size);
}

void operator delete(void* ptr) noexcept
{
    return free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
    return free(ptr);
}

void* operator new[](size_t size)
{
    void* ptr = malloc(size);
    VERIFY(ptr);
    return ptr;
}

void* operator new[](size_t size, std::nothrow_t const&) noexcept
{
    return malloc(size);
}

void operator delete[](void* ptr) noexcept
{
    return free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    return free(ptr);
}

// This is usually provided by libstdc++ in most cases, and the kernel has its own definition in
// Kernel/Heap/kmalloc.cpp. If neither of those apply, the following should suffice to not fail during linking.
namespace AK_REPLACED_STD_NAMESPACE {

nothrow_t const nothrow;

}

#else

#    include <cstddef>
#    include <cstring>

#    if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
// LeakSanitizer does not reliably trace references stored in mimalloc-managed
// AK containers, so sanitizer builds fall back to the system allocator.
#        define AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED 1
#    else
#        include <mimalloc.h>
#    endif

static bool allocation_needs_explicit_alignment(size_t alignment)
{
    return alignment > alignof(std::max_align_t);
}

#    ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED

static void* aligned_alloc_with_system_allocator(size_t size, size_t alignment, bool zeroed)
{
    void* ptr = nullptr;
    auto actual_size = size == 0 ? static_cast<size_t>(1) : size;
    if (auto result = posix_memalign(&ptr, alignment, actual_size); result != 0)
        return nullptr;
    if (zeroed)
        __builtin_memset(ptr, 0, actual_size);
    return ptr;
}

void* ak_kcalloc(size_t count, size_t size)
{
    return calloc(count, size);
}

void* ak_kmalloc(size_t size)
{
    return malloc(size);
}

void* ak_krealloc(void* ptr, size_t size)
{
    return realloc(ptr, size);
}

size_t ak_kmalloc_good_size(size_t size)
{
    return size;
}

void ak_kfree(void* ptr)
{
    free(ptr);
}

extern "C" {
void* ladybird_rust_alloc(size_t size, size_t alignment);
void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment);
void ladybird_rust_dealloc(void* ptr, size_t alignment);
void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment);
}

extern "C" void* ladybird_rust_alloc(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment))
        return aligned_alloc_with_system_allocator(size, alignment, false);
    return malloc(size);
}

extern "C" void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment))
        return aligned_alloc_with_system_allocator(size, alignment, true);
    return calloc(1, size);
}

extern "C" void ladybird_rust_dealloc(void* ptr, size_t)
{
    free(ptr);
}

extern "C" void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment)
{
    if (!allocation_needs_explicit_alignment(alignment))
        return realloc(ptr, new_size);

    auto* new_ptr = aligned_alloc_with_system_allocator(new_size, alignment, false);
    if (!new_ptr)
        return nullptr;
    if (ptr)
        __builtin_memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    free(ptr);
    return new_ptr;
}

#    else

void* ak_kcalloc(size_t count, size_t size)
{
    return mi_calloc(count, size);
}

void* ak_kmalloc(size_t size)
{
    return mi_malloc(size);
}

void* ak_krealloc(void* ptr, size_t size)
{
    return mi_realloc(ptr, size);
}

size_t ak_kmalloc_good_size(size_t size)
{
    return mi_good_size(size);
}

void ak_kfree(void* ptr)
{
    mi_free(ptr);
}

extern "C" {
void* ladybird_rust_alloc(size_t size, size_t alignment);
void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment);
void ladybird_rust_dealloc(void* ptr, size_t alignment);
void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment);
}

extern "C" void* ladybird_rust_alloc(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment))
        return mi_malloc_aligned(size, alignment);
    return mi_malloc(size);
}

extern "C" void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment))
        return mi_zalloc_aligned(size, alignment);
    return mi_zalloc(size);
}

extern "C" void ladybird_rust_dealloc(void* ptr, size_t)
{
    mi_free(ptr);
}

extern "C" void* ladybird_rust_realloc(void* ptr, size_t, size_t new_size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment))
        return mi_realloc_aligned(ptr, new_size, alignment);
    return mi_realloc(ptr, new_size);
}

#    endif

#endif
