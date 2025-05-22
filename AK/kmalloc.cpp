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

#endif
