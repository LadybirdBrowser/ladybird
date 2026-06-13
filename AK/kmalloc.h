/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <new>
#include <stdlib.h>

enum class HeapPartition {
    General,
    ArrayBuffer,
    JSObjectStorage,
    Layout,
    Painting,
    String,
};

[[nodiscard]] void* ak_kcalloc(size_t count, size_t size);
void ak_kfree(void* ptr);
[[nodiscard]] void* ak_kmalloc(size_t size);
[[nodiscard]] void* ak_kmalloc(HeapPartition, size_t size);
[[nodiscard]] void* ak_krealloc(void* ptr, size_t size);
[[nodiscard]] void* ak_krealloc(HeapPartition, void* ptr, size_t size);
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

[[nodiscard]] inline void* kmalloc(HeapPartition partition, size_t size)
{
    return ak_kmalloc(partition, size);
}

[[nodiscard]] inline void* krealloc(void* ptr, size_t size)
{
    return ak_krealloc(ptr, size);
}

[[nodiscard]] inline void* krealloc(HeapPartition partition, void* ptr, size_t size)
{
    return ak_krealloc(partition, ptr, size);
}

[[nodiscard]] inline size_t kmalloc_good_size(size_t size)
{
    return ak_kmalloc_good_size(size);
}

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

#define AK_ALLOC_WITH_KMALLOC_PARTITION(partition)                         \
public:                                                                    \
    static void* operator new(size_t size)                                 \
    {                                                                      \
        auto* ptr = ak_kmalloc(partition, size);                           \
        VERIFY(ptr);                                                       \
        return ptr;                                                        \
    }                                                                      \
                                                                           \
    static void* operator new(size_t size, std::nothrow_t const&) noexcept \
    {                                                                      \
        return ak_kmalloc(partition, size);                                \
    }                                                                      \
                                                                           \
    static void operator delete(void* ptr) noexcept                        \
    {                                                                      \
        ak_kfree(ptr);                                                     \
    }                                                                      \
                                                                           \
    static void operator delete(void* ptr, std::nothrow_t const&) noexcept \
    {                                                                      \
        ak_kfree(ptr);                                                     \
    }

#define AK_ALLOC_WITH_KMALLOC AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::General)
