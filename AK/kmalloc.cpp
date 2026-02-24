/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Tracy.h>
#include <AK/kmalloc.h>

#include <cstddef>
#include <cstring>

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
// LeakSanitizer does not reliably trace references stored in mimalloc-managed
// AK containers, so sanitizer builds fall back to the system allocator.
#    define AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED 1
#else
#    include <mimalloc.h>
#endif

static bool allocation_needs_explicit_alignment(size_t alignment)
{
    return alignment > alignof(std::max_align_t);
}

#ifdef AK_USE_SYSTEM_ALLOCATOR_INSTRUMENTED

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
    void* ptr = calloc(count, size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, count * size);
    }
    return ptr;
}

void* ak_kmalloc(size_t size)
{
    void* ptr = malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, size);
    }
    return ptr;
}

void* ak_krealloc(void* ptr, size_t size)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
    void* new_ptr = realloc(ptr, size);
    if (new_ptr) {
        TRACY_ALLOCATED_MEMORY(new_ptr, size);
    }
    return new_ptr;
}

size_t ak_kmalloc_good_size(size_t size)
{
    return size;
}

void ak_kfree(void* ptr)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
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
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = aligned_alloc_with_system_allocator(size, alignment, false);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = aligned_alloc_with_system_allocator(size, alignment, true);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = calloc(1, size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void ladybird_rust_dealloc(void* ptr, size_t)
{
    if (ptr) {
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    free(ptr);
}

extern "C" void* ladybird_rust_realloc(void* ptr, size_t old_size, size_t new_size, size_t alignment)
{
    if (!allocation_needs_explicit_alignment(alignment)) {
        if (ptr) {
            TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
        }
        void* new_ptr = realloc(ptr, new_size);
        if (new_ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
        }
        return new_ptr;
    }

    auto* new_ptr = aligned_alloc_with_system_allocator(new_size, alignment, false);
    if (!new_ptr)
        return nullptr;
    if (ptr) {
        __builtin_memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    free(ptr);
    TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
    return new_ptr;
}

#else

void* ak_kcalloc(size_t count, size_t size)
{
    void* ptr = mi_calloc(count, size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, count * size);
    }
    return ptr;
}

void* ak_kmalloc(size_t size)
{
    void* ptr = mi_malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY(ptr, size);
    }
    return ptr;
}

void* ak_krealloc(void* ptr, size_t size)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
    void* new_ptr = mi_realloc(ptr, size);
    if (new_ptr) {
        TRACY_ALLOCATED_MEMORY(new_ptr, size);
    }
    return new_ptr;
}

size_t ak_kmalloc_good_size(size_t size)
{
    return mi_good_size(size);
}

void ak_kfree(void* ptr)
{
    if (ptr) {
        TRACY_FREED_MEMORY(ptr);
    }
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
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = mi_malloc_aligned(size, alignment);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = mi_malloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void* ladybird_rust_alloc_zeroed(size_t size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        void* ptr = mi_zalloc_aligned(size, alignment);
        if (ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
        }
        return ptr;
    }

    void* ptr = mi_zalloc(size);
    if (ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(ptr, size, "Rust");
    }
    return ptr;
}

extern "C" void ladybird_rust_dealloc(void* ptr, size_t)
{
    if (ptr) {
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    mi_free(ptr);
}

extern "C" void* ladybird_rust_realloc(void* ptr, size_t, size_t new_size, size_t alignment)
{
    if (allocation_needs_explicit_alignment(alignment)) {
        if (ptr) {
            TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
        }
        void* new_ptr = mi_realloc_aligned(ptr, new_size, alignment);
        if (new_ptr) {
            TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
        }
        return new_ptr;
    }

    if (ptr) {
        TRACY_FREED_MEMORY_NAMED(ptr, "Rust");
    }
    void* new_ptr = mi_realloc(ptr, new_size);
    if (new_ptr) {
        TRACY_ALLOCATED_MEMORY_NAMED(new_ptr, new_size, "Rust");
    }
    return new_ptr;
}

#endif

// However deceptively simple these functions look, they must not be inlined.
// Memory allocated in one translation unit has to be deallocatable in another
// translation unit, so these functions must be the same everywhere.
// By making these functions global, this invariant is enforced.

void* operator new(size_t size)
{
    void* ptr = ak_kmalloc(size);
    VERIFY(ptr);
    return ptr;
}

void* operator new(size_t size, std::nothrow_t const&) noexcept
{
    return ak_kmalloc(size);
}

void operator delete(void* ptr) noexcept
{
    return ak_kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
    return ak_kfree(ptr);
}

void* operator new[](size_t size)
{
    void* ptr = ak_kmalloc(size);
    VERIFY(ptr);
    return ptr;
}

void* operator new[](size_t size, std::nothrow_t const&) noexcept
{
    return ak_kmalloc(size);
}

void operator delete[](void* ptr) noexcept
{
    return ak_kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    return ak_kfree(ptr);
}
