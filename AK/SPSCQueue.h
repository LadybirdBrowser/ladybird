/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Atomic.h>

namespace AK {

// Single-producer single-consumer fixed-capacity queue.
template<typename T, size_t Capacity>
class SPSCQueue {
public:
    bool try_push(T const& value)
    {
        u32 head = m_head.load(AK::MemoryOrder::memory_order_relaxed);
        u32 tail = m_tail.load(AK::MemoryOrder::memory_order_acquire);
        if ((head - tail) >= Capacity)
            return false;
        m_storage[head % Capacity] = value;
        m_head.store(head + 1, AK::MemoryOrder::memory_order_release);
        return true;
    }

    bool try_pop(T& out)
    {
        u32 tail = m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        u32 head = m_head.load(AK::MemoryOrder::memory_order_acquire);
        if (tail == head)
            return false;
        out = m_storage[tail % Capacity];
        m_tail.store(tail + 1, AK::MemoryOrder::memory_order_release);
        return true;
    }

    bool is_empty() const
    {
        u32 tail = m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        u32 head = m_head.load(AK::MemoryOrder::memory_order_acquire);
        return tail == head;
    }

private:
    Array<T, Capacity> m_storage {};
    Atomic<u32> m_head { 0 };
    Atomic<u32> m_tail { 0 };
};

}
