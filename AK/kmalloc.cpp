/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tracy.h>
#include <AK/kmalloc.h>

#if defined(TRACY_ENABLE_MEMORY)

#    include <AK/Assertions.h>

// However deceptively simple these functions look, they must not be inlined.
// Memory allocated in one translation unit has to be deallocatable in another
// translation unit, so these functions must be the same everywhere.
// By making these functions global, this invariant is enforced.

void* operator new(size_t size)
{
    void* ptr = malloc(size);
    VERIFY(ptr);
    TRACY_ALLOCATED_MEMORY(ptr, size);
    return ptr;
}

void* operator new(size_t size, std::nothrow_t const&) noexcept
{
    auto* ptr = malloc(size);
    TRACY_ALLOCATED_MEMORY(ptr, size);
    return ptr;
}

void operator delete(void* ptr) noexcept
{
    TRACY_FREED_MEMORY(ptr);
    return free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
    TRACY_FREED_MEMORY(ptr);
    return free(ptr);
}

void* operator new[](size_t size)
{
    void* ptr = malloc(size);
    VERIFY(ptr);
    TRACY_ALLOCATED_MEMORY(ptr, size);
    return ptr;
}

void* operator new[](size_t size, std::nothrow_t const&) noexcept
{
    auto* ptr = malloc(size);
    TRACY_ALLOCATED_MEMORY(ptr, size);
    return ptr;
}

void operator delete[](void* ptr) noexcept
{
    TRACY_FREED_MEMORY(ptr);
    return free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
    TRACY_FREED_MEMORY(ptr);
    return free(ptr);
}

#endif
