/*
 * Copyright (c) 2026, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Concepts.h>
#include <AK/CpuBackoff.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/StdLibExtraDetails.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

// This is a multiple producer single consumer lockfree bounded ring buffer.
// To make construction in shared memory simpler the data is stored inline.
// With each element a u32 sequence number is stored so sizeof(T) should be big to minimize space overhead.
template<typename T, u32 Size>
requires(is_power_of_two(Size) && IsMoveConstructible<T> && IsMoveAssignable<T> && !IsLvalueReference<T>)
class MPSCRingBuffer {
    AK_MAKE_NONCOPYABLE(MPSCRingBuffer);
    AK_MAKE_NONMOVABLE(MPSCRingBuffer);

public:
    MPSCRingBuffer()
    {
        for (u64 i = 0; i < Size; i++) {
            m_data[i].sequence.store(i, MemoryOrder::memory_order_relaxed);
        }
    }

    ~MPSCRingBuffer()
    {
        T item;
        while (try_pop(item)) { }
    }

    [[nodiscard]] ALWAYS_INLINE bool try_push(T value)
    {
        return try_emplace(move(value));
    }

    template<typename U>
    requires(IsConvertible<U, T> && !IsSame<RemoveCVReference<U>, T>)
    [[nodiscard]] ALWAYS_INLINE bool try_push(U&& value)
    {
        return try_emplace(forward<U>(value));
    }

    template<typename... Args>
    requires(IsConstructible<T, Args...>)
    [[nodiscard]] ALWAYS_INLINE bool try_emplace(Args&&... args)
    {
        u32 head = m_head.load(MemoryOrder::memory_order_relaxed);

        while (true) {
            Node& slot = m_data[get_offset(head)];
            u32 sequence = slot.sequence.load(MemoryOrder::memory_order_acquire);
            i32 diff = static_cast<i32>(sequence - head);

            if (diff == 0) {
                // Slot is free
                if (m_head.compare_exchange_weak(head, head + 1, MemoryOrder::memory_order_acq_rel)) {
                    // We now own the slot
                    // No need to launder here as nothing accesses the pointer given to new
                    new (slot.data) T(forward<Args>(args)...);
                    slot.sequence.store(head + 1, MemoryOrder::memory_order_release);
                    return true;
                }
                // The head was updated by another thread, try again
                cpu_pause();
            } else if (diff < 0) {
                // Buffer full
                return false;
            } else {
                //  Our head is stale
                head = m_head.load(MemoryOrder::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] ALWAYS_INLINE bool try_pop(T& value)
    {
        Node& slot = m_data[get_offset(m_tail)];

        u32 sequence = slot.sequence.load(MemoryOrder::memory_order_acquire);

        i32 diff = static_cast<i32>(sequence - (m_tail + 1));

        if (diff == 0) {
            // The slot is ready for reading
            T* ptr = reinterpret_cast<T*>(slot.data);
            value = move(*ptr);
            ptr->~T();
            slot.sequence.store(m_tail + Size, MemoryOrder::memory_order_release);
            m_tail += 1;
            return true;
        }
        // In this case either the write wasn't finished ( sequence == read_index ) so diff == -1,
        // or the queue is empty and no write is happening, so diff == -Size
        return false;
    }

private:
    ALWAYS_INLINE constexpr u32 get_offset(u32 index) const
    {
        return index & (Size - 1);
    }

    struct Node {
        Atomic<u32> sequence;
        alignas(T) unsigned char data[sizeof(T)];
    };

    AK_CACHE_ALIGNED Atomic<u32> m_head = 0;
    AK_CACHE_ALIGNED u32 m_tail = 0;
    AK_CACHE_ALIGNED Node m_data[Size];
};

// This is a single producer single consumer lockfree bounded ring buffer.
// To make construction in shared memory simpler the data is stored inline.
template<typename T, u64 Size>
requires(is_power_of_two(Size) && IsMoveConstructible<T> && IsMoveAssignable<T> && !IsLvalueReference<T>)
class SPSCRingBuffer {
    AK_MAKE_NONCOPYABLE(SPSCRingBuffer);
    AK_MAKE_NONMOVABLE(SPSCRingBuffer);

public:
    SPSCRingBuffer() = default;

    ~SPSCRingBuffer()
    {
        T item;
        while (try_pop(item)) { }
    }

    // Handle initializer list and copy construction
    [[nodiscard]] ALWAYS_INLINE bool try_push(T value)
    {

        return try_emplace(move(value));
    }

    template<typename U>
    requires(IsConvertible<U, T> && !IsSame<RemoveCVReference<U>, T>)
    [[nodiscard]] ALWAYS_INLINE bool try_push(U&& value)
    {
        return try_emplace(forward<U>(value));
    }

    template<typename... Args>
    requires(IsConstructible<T, Args...>)
    [[nodiscard]] ALWAYS_INLINE bool try_emplace(Args&&... args)
    {
        u64 head = m_head.load(MemoryOrder::memory_order_relaxed);
        if (head - m_cached_tail == Size) {
            m_cached_tail = m_tail.load(MemoryOrder::memory_order_acquire);
            if (head - m_cached_tail == Size)
                return false;
        }
        // No need to launder here as nothing accesses the pointer given to new
        new (m_data[get_offset(head)]) T(forward<Args>(args)...);
        m_head.store(head + 1, MemoryOrder::memory_order_release);
        return true;
    }

    [[nodiscard]] ALWAYS_INLINE bool try_pop(T& value)
    {
        u64 tail = m_tail.load(MemoryOrder::memory_order_relaxed);
        if (tail == m_cached_head) {
            m_cached_head = m_head.load(MemoryOrder::memory_order_acquire);
            if (tail == m_cached_head)
                return false;
        }
        T* ptr = reinterpret_cast<T*>(m_data[get_offset(tail)]);
        value = move(*ptr);
        ptr->~T();
        m_tail.store(tail + 1, MemoryOrder::memory_order_release);
        return true;
    }

    [[nodiscard]] ALWAYS_INLINE bool is_empty() const
    {
        u64 tail = m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        u64 head = m_head.load(AK::MemoryOrder::memory_order_acquire);
        return tail == head;
    }

private:
    [[nodiscard]] ALWAYS_INLINE constexpr u64 get_offset(u64 index) const
    {
        return index & (Size - 1);
    }

    AK_CACHE_ALIGNED Atomic<u64> m_head { 0 };
    AK_CACHE_ALIGNED Atomic<u64> m_tail { 0 };
    AK_CACHE_ALIGNED u64 m_cached_head = 0;
    AK_CACHE_ALIGNED u64 m_cached_tail = 0;
    // Aligned to prevent false sharing the beginning of m_data
    AK_CACHE_ALIGNED alignas(T) unsigned char m_data[Size][sizeof(T)];
};

}

#ifdef USING_AK_GLOBALLY
using AK::MPSCRingBuffer;
using AK::SPSCRingBuffer;
#endif
