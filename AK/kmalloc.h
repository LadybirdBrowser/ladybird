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

#define kcalloc calloc
#define kmalloc malloc
#define kmalloc_good_size malloc_good_size

inline void kfree_sized(void* ptr, size_t)
{
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
