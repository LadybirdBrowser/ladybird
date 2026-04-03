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

#if defined(AK_OS_SERENITY)
#    define kcalloc calloc
#    define kfree free
#    define kmalloc malloc
#    define krealloc realloc
#    define kmalloc_good_size malloc_good_size
#else
[[nodiscard]] void* ak_kcalloc(size_t count, size_t size);
void ak_kfree(void* ptr);
[[nodiscard]] void* ak_kmalloc(size_t size);
[[nodiscard]] void* ak_krealloc(void* ptr, size_t size);
[[nodiscard]] size_t ak_kmalloc_good_size(size_t size);

[[nodiscard]] inline void* kcalloc(size_t count, size_t size)
{
    return ak_kcalloc(count, size);
}

inline void kfree(void* ptr)
{
    ak_kfree(ptr);
}

[[nodiscard]] inline void* kmalloc(size_t size)
{
    return ak_kmalloc(size);
}

[[nodiscard]] inline void* krealloc(void* ptr, size_t size)
{
    return ak_krealloc(ptr, size);
}

[[nodiscard]] inline size_t kmalloc_good_size(size_t size)
{
    return ak_kmalloc_good_size(size);
}
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
