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
    Buffer,
    JSObjectStorage,
    Layout,
    String,
};

[[nodiscard]] void* ak_kcalloc(size_t count, size_t size);
void ak_kfree(void* ptr);
[[nodiscard]] void* ak_kmalloc(size_t size);
[[nodiscard]] void* ak_kmalloc(HeapPartition, size_t size);
[[nodiscard]] void* ak_kmalloc_aligned(size_t size, size_t alignment);
[[nodiscard]] void* ak_kmalloc_aligned(HeapPartition, size_t size, size_t alignment);
[[nodiscard]] void* ak_krealloc(void* ptr, size_t size);
[[nodiscard]] void* ak_krealloc(HeapPartition, void* ptr, size_t size);
[[nodiscard]] size_t ak_kmalloc_usable_size(void const* ptr);
void ak_kmalloc_collect();

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

[[nodiscard]] inline void* kmalloc_aligned(size_t size, size_t alignment)
{
    return ak_kmalloc_aligned(size, alignment);
}

[[nodiscard]] inline void* kmalloc_aligned(HeapPartition partition, size_t size, size_t alignment)
{
    return ak_kmalloc_aligned(partition, size, alignment);
}

[[nodiscard]] inline void* krealloc(void* ptr, size_t size)
{
    return ak_krealloc(ptr, size);
}

[[nodiscard]] inline void* krealloc(HeapPartition partition, void* ptr, size_t size)
{
    return ak_krealloc(partition, ptr, size);
}

[[nodiscard]] inline size_t kmalloc_usable_size(void const* ptr)
{
    return ak_kmalloc_usable_size(ptr);
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

#define AK_ALLOC_WITH_KMALLOC_PARTITION(partition)                                                         \
public:                                                                                                    \
    using AllocatedWithKmallocTag [[maybe_unused]] = int;                                                  \
                                                                                                           \
    static void* operator new(size_t size)                                                                 \
    {                                                                                                      \
        auto* ptr = ak_kmalloc(partition, size);                                                           \
        VERIFY(ptr);                                                                                       \
        return ptr;                                                                                        \
    }                                                                                                      \
                                                                                                           \
    static void* operator new(size_t size, std::nothrow_t const&) noexcept                                 \
    {                                                                                                      \
        return ak_kmalloc(partition, size);                                                                \
    }                                                                                                      \
                                                                                                           \
    static void* operator new(size_t size, std::align_val_t alignment)                                     \
    {                                                                                                      \
        auto* ptr = ak_kmalloc_aligned(partition, size, static_cast<size_t>(alignment));                   \
        VERIFY(ptr);                                                                                       \
        return ptr;                                                                                        \
    }                                                                                                      \
                                                                                                           \
    static void* operator new(size_t size, std::align_val_t alignment, std::nothrow_t const&) noexcept     \
    {                                                                                                      \
        return ak_kmalloc_aligned(partition, size, static_cast<size_t>(alignment));                        \
    }                                                                                                      \
                                                                                                           \
    static void* operator new(size_t, void* location) noexcept                                             \
    {                                                                                                      \
        return location;                                                                                   \
    }                                                                                                      \
                                                                                                           \
    static void operator delete(void* ptr) noexcept                                                        \
    {                                                                                                      \
        ak_kfree(ptr);                                                                                     \
    }                                                                                                      \
                                                                                                           \
    static void operator delete(void* ptr, std::nothrow_t const&) noexcept                                 \
    {                                                                                                      \
        ak_kfree(ptr);                                                                                     \
    }                                                                                                      \
                                                                                                           \
    static void operator delete(void* ptr, std::align_val_t) noexcept                                      \
    {                                                                                                      \
        ak_kfree(ptr);                                                                                     \
    }                                                                                                      \
                                                                                                           \
    static void operator delete(void* ptr, std::align_val_t, std::nothrow_t const&) noexcept               \
    {                                                                                                      \
        ak_kfree(ptr);                                                                                     \
    }                                                                                                      \
                                                                                                           \
    static constexpr auto kmalloc_operator_new() { return static_cast<void* (*)(size_t)>(&operator new); } \
    static constexpr auto kmalloc_operator_delete() { return static_cast<void (*)(void*) noexcept>(&operator delete); }

#define AK_ALLOC_WITH_KMALLOC AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::General)

template<typename T>
inline constexpr bool AllocatedWithSystemAllocator = false;

template<typename T>
inline constexpr bool AllocatedWithCustomAllocator = false;

template<typename T>
concept AllocatedWithKmalloc = requires {
    typename RemoveCV<T>::AllocatedWithKmallocTag;
    requires static_cast<void* (*)(size_t)>(&RemoveCV<T>::operator new) == RemoveCV<T>::kmalloc_operator_new();
    requires static_cast<void (*)(void*) noexcept>(&RemoveCV<T>::operator delete) == RemoveCV<T>::kmalloc_operator_delete();
} || AllocatedWithSystemAllocator<RemoveCV<T>> || AllocatedWithCustomAllocator<RemoveCV<T>>;
