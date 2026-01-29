/*
 * Copyright (c) 2026, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Concepts.h>
#include <AK/Noncopyable.h>
#include <AK/Platform.h>
#include <AK/StdLibExtraDetails.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {

// This is a multiple producer single consumer lockfree ring buffer.
// To make construction in shared memory simpler the data is stored inline.
// With each element a u32 sequence number is stored so sizeof(T) should be big to minimize space overhead.
template<typename T, u32 Size>
requires(is_power_of_two(Size) && Detail::IsMoveConstructible<T>)
class MPSCRingBuffer {
    AK_MAKE_NONCOPYABLE(MPSCRingBuffer);
    AK_MAKE_NONMOVABLE(MPSCRingBuffer);

public:
    MPSCRingBuffer()
    {
        for (u64 i = 0; i < Size; i++) {
            m_array[i].sequence.store(i, MemoryOrder::memory_order_relaxed);
        }
    }

    ~MPSCRingBuffer() = default;

    ALWAYS_INLINE bool try_pop(T& value)
    {
        Node& slot = m_array[calculate_offset(m_read_index)];

        u32 sequence = slot.sequence.load(MemoryOrder::memory_order_acquire);

        i32 diff = static_cast<i32>(sequence - (m_read_index + 1));

        if (diff == 0) {
            // The slot is ready for reading
            value = move(slot.data);
            slot.sequence.store(m_read_index + Size, MemoryOrder::memory_order_release);
            m_read_index += 1;
            return true;
        }
        // In this case either the write wasn't finished ( sequence == read_index ) so diff == -1,
        // or the queue is empty and no write is happening, so diff == -Size
        return false;
    }

    ALWAYS_INLINE bool try_push(T value)
    {
        u32 write_index = m_write_index.load(MemoryOrder::memory_order_relaxed);

        while (true) {
            Node& slot = m_array[calculate_offset(write_index)];
            u32 sequence = slot.sequence.load(MemoryOrder::memory_order_acquire);
            i32 diff = static_cast<i32>(sequence - write_index);

            if (diff == 0) {
                // Slot is free
                if (m_write_index.compare_exchange_weak(write_index, write_index + 1, MemoryOrder::memory_order_acq_rel)) {
                    // We now own the slot
                    slot.data = move(value);
                    slot.sequence.store(write_index + 1, MemoryOrder::memory_order_release);
                    return true;
                }
                // The index was updated by another thread, try again
            } else if (diff < 0) {
                // Buffer full
                return false;
            } else {
                //  Our write index is stale
                write_index = m_write_index.load(MemoryOrder::memory_order_relaxed);
            }
        }
    }

private:
    ALWAYS_INLINE constexpr u32 calculate_offset(u32 index)
    {
        return index & (Size - 1);
    }

    struct AK_CACHE_ALIGNED Node {
        T data;
        Atomic<u32> sequence;
    };

    AK_CACHE_ALIGNED u32 m_read_index = 0;
    AK_CACHE_ALIGNED Atomic<u32> m_write_index = 0;
    // Aligning the array not to false share the first elements with the previous memeber
    Node m_array[Size];
};

}

#ifdef USING_AK_GLOBALLY
using AK::MPSCRingBuffer;
#endif
