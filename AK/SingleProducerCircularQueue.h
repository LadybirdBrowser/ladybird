/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

// The shared state of a single-producer single-consumer circular queue: the head/tail counters and
// the slot storage, owned by a SingleProducerCircularQueueBase subclass.
template<typename T, size_t Size>
// Size must be a power of two, which speeds up the modulus operations for indexing.
requires(popcount(Size) == 1)
struct SPCQData {
    SPCQData() = default;
    SPCQData(SPCQData const&) = delete;
    SPCQData(SPCQData&&) = delete;
    ~SPCQData() = default;

    // Invariant: tail >= head
    // Invariant: head and tail are monotonically increasing
    // Invariant: tail always points to the next free location where an enqueue can happen.
    // Invariant: head always points to the element to be dequeued next.
    // Invariant: tail is only modified by enqueue functions.
    // Invariant: head is only modified by dequeue functions.
    // An empty queue is signalled with:  tail = head
    // A full queue is signalled with:  tail - head = size
    AK_CACHE_ALIGNED Atomic<size_t> m_tail { 0 };
    AK_CACHE_ALIGNED Atomic<size_t> m_head { 0 };

    alignas(T) Array<T, Size> m_data;
};

template<typename T, size_t Size>
class SingleProducerCircularQueueBase {
public:
    using ValueType = T;

    enum class QueueStatus : u8 {
        Invalid = 0,
        Full,
        Empty,
    };

    constexpr size_t size() const { return Size; }

    // These are provably inconsistent and should only be used as hints to the actual capacity/used count.
    ALWAYS_INLINE size_t weak_remaining_capacity() const { return Size - weak_used(); }
    ALWAYS_INLINE size_t weak_used() const
    {
        auto volatile tail = m_data->m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        auto volatile head = m_data->m_head.load(AK::MemoryOrder::memory_order_relaxed);
        return tail - head;
    }
    ALWAYS_INLINE size_t weak_head() const { return m_data->m_head.load(AK::MemoryOrder::memory_order_relaxed); }
    ALWAYS_INLINE size_t weak_tail() const { return m_data->m_tail.load(AK::MemoryOrder::memory_order_relaxed); }

    template<typename WriteToSlot>
    ErrorOr<void, QueueStatus> enqueue_in_place(WriteToSlot write_to_slot)
    {
        VERIFY(m_data);
        auto tail = m_data->m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        if (tail - m_data->m_head.load(AK::MemoryOrder::memory_order_acquire) == Size)
            return QueueStatus::Full;
        write_to_slot(m_data->m_data[tail % Size]);
        m_data->m_tail.store(tail + 1, AK::MemoryOrder::memory_order_release);

        return {};
    }

    ErrorOr<void, QueueStatus> enqueue(ValueType to_insert)
    {
        return enqueue_in_place([&](ValueType& slot) {
            slot = move(to_insert);
        });
    }

    ALWAYS_INLINE bool can_enqueue() const
    {
        return m_data->m_tail.load(AK::MemoryOrder::memory_order_relaxed) - m_data->m_head.load(AK::MemoryOrder::memory_order_relaxed) < Size;
    }

    // Repeatedly try to enqueue, calling wait_function to block while the queue is full.
    ErrorOr<void> blocking_enqueue(ValueType to_insert, Function<void()> wait_function)
    {
        while (true) {
            auto result = enqueue(to_insert);
            if (!result.is_error())
                return {};
            if (result.error() != QueueStatus::Full)
                return Error::from_string_literal("Unexpected error while enqueuing");

            wait_function();
        }
    }

    Optional<ValueType> peek() const
    {
        VERIFY(m_data);
        auto head = m_data->m_head.load(AK::MemoryOrder::memory_order_relaxed);
        if (head == m_data->m_tail.load(AK::MemoryOrder::memory_order_acquire))
            return {};
        return m_data->m_data[head % Size];
    }

    ErrorOr<ValueType, QueueStatus> dequeue()
    {
        VERIFY(m_data);
        auto head = m_data->m_head.load(AK::MemoryOrder::memory_order_relaxed);
        if (head == m_data->m_tail.load(AK::MemoryOrder::memory_order_acquire))
            return QueueStatus::Empty;
        auto data = move(m_data->m_data[head % Size]);
        m_data->m_head.store(head + 1, AK::MemoryOrder::memory_order_release);
        return { move(data) };
    }

    size_t head() const { return m_data->m_head.load(AK::MemoryOrder::memory_order_relaxed); }

protected:
    SingleProducerCircularQueueBase() = default;
    explicit SingleProducerCircularQueueBase(SPCQData<T, Size>& data)
        : m_data(&data)
    {
    }

    void set_data(SPCQData<T, Size>* data) { m_data = data; }

private:
    SPCQData<T, Size>* m_data { nullptr };
};

// An lock-free SPSC queue that owns its slot storage inline.
template<typename T, size_t Size = 32>
class SingleProducerCircularQueue final : public SingleProducerCircularQueueBase<T, Size> {
    AK_MAKE_NONCOPYABLE(SingleProducerCircularQueue);
    AK_MAKE_NONMOVABLE(SingleProducerCircularQueue);

public:
    SingleProducerCircularQueue()
        : SingleProducerCircularQueueBase<T, Size>(m_storage)
    {
    }

private:
    SPCQData<T, Size> m_storage;
};

}

#if USING_AK_GLOBALLY
using AK::SingleProducerCircularQueue;
using AK::SingleProducerCircularQueueBase;
using AK::SPCQData;
#endif
