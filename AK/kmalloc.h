/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <AK/Platform.h>
#include <new>
#include <stdlib.h>

// Allocation functions
#define kcalloc calloc
#define kfree free
#define kmalloc malloc
#define kmalloc_good_size malloc_good_size
#define krealloc realloc

// Allocating library functions
// Some allocator libraries provide their own versions of these. If they don't and we use a different free
// for kfree we need to be careful to use libc free when not overriding malloc
#define krealpath realpath
#define kstrdup strdup
#define kstrndup strndup

// Microsoft extensions
#if defined(AK_OS_WINDOWS)
#    define k_expand _expand
#    define k_msize _msize
#    define k_recalloc _recalloc
// Aligned versions
#    define kaligned_alloc(alignment, size) _aligned_malloc(size, alignment)
#    define kaligned_free _aligned_free
#else
#    define kaligned_alloc(alignment, size) aligned_alloc(alignment, size)
#    define kaligned_free free
#endif

// Posix functions
#if !defined(AK_OS_WINDOWS)

#else

#endif

inline void kfree_sized(void* ptr, size_t)
{
    kfree(ptr);
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
