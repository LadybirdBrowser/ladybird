/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <AK/Platform.h>

#if defined(TRACY_ENABLE_MEMORY)
#    include <AK/Tracy.h>
#endif

#include <new>
#include <stdlib.h>

#if !defined(TRACY_ENABLE_MEMORY)
#    define kcalloc(count, size) calloc(count, size)
#    define kmalloc(size) malloc(size)
#    define kfree(ptr) free(ptr)
#else
#    define kcalloc(count, size)               \
        ({                                     \
            auto* ptr = calloc(count, size);   \
            TRACY_ALLOCATED_MEMORY(ptr, size); \
            ptr;                               \
        })

#    define kmalloc(size)                      \
        ({                                     \
            auto* ptr = malloc(size);          \
            TRACY_ALLOCATED_MEMORY(ptr, size); \
            ptr;                               \
        })

#    define kfree(ptr)               \
        ({                           \
            TRACY_FREED_MEMORY(ptr); \
            free(ptr);               \
        })
#endif

#define kmalloc_good_size(size) malloc_good_size(size)

inline void kfree_sized(void* ptr, size_t)
{
#if defined(TRACY_ENABLE_MEMORY)
    TRACY_FREED_MEMORY(ptr);
#endif
    free(ptr);
}

#ifndef AK_OS_SERENITY
#    include <AK/Types.h>

#    ifndef AK_OS_MACOS
extern "C" {
inline size_t malloc_good_size(size_t size) { return size; }
}
#    else
#        include <malloc/malloc.h>
#    endif
#endif

using std::nothrow;

inline void* kmalloc_array(AK::Checked<size_t> a, AK::Checked<size_t> b)
{
    auto size = a * b;
    VERIFY(!size.has_overflow());
    return kmalloc(size.value());
}

inline void* kmalloc_array(AK::Checked<size_t> a, AK::Checked<size_t> b, AK::Checked<size_t> c)
{
    auto size = a * b * c;
    VERIFY(!size.has_overflow());
    return kmalloc(size.value());
}
