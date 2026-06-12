/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/BuiltinWrappers.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibCore/AnonymousBuffer.h>

namespace Core {

// A circular lock-free queue (or a buffer) with a single producer and a single consumer,
// residing in shared memory and designed to be accessible to multiple processes.
//
// This implementation makes use of the fact that the producer and the consumer are each a
// single thread of execution, so head is only ever advanced by the consumer and tail only
// ever advanced by the producer, which simplifies the synchronization down to a pair of
// monotonically increasing atomic counters.
//
// This class is designed to be transferred over IPC and mmap()ed into multiple processes' memory.
// It is a synthetic pointer to the actual shared memory, which is abstracted away from the user.
// FIXME: Make this independent of shared memory, so that we can move it to AK.
template<typename T, size_t Size = 32>
// Size must be a power of two, which speeds up the modulus operations for indexing.
requires(popcount(Size) == 1)
class SharedSingleProducerCircularQueue final {

public:
    using ValueType = T;

    enum class QueueStatus : u8 {
        Invalid = 0,
        Full,
        Empty,
    };

    SharedSingleProducerCircularQueue() = default;
    SharedSingleProducerCircularQueue(SharedSingleProducerCircularQueue<ValueType, Size>& queue) = default;

    SharedSingleProducerCircularQueue(SharedSingleProducerCircularQueue&& queue) = default;
    SharedSingleProducerCircularQueue& operator=(SharedSingleProducerCircularQueue&& queue) = default;

    // Allocates a new circular queue in shared memory.
    static ErrorOr<SharedSingleProducerCircularQueue<T, Size>> create()
    {
        auto anon_buf = TRY(AnonymousBuffer::create_with_size(sizeof(SharedMemorySPCQ)));
        auto shared_queue = new (anon_buf.data<void>()) SharedMemorySPCQ;
        return create_internal(anon_buf, shared_queue);
    }

    // Uses an existing circular queue from given shared memory.
    static ErrorOr<SharedSingleProducerCircularQueue<T, Size>> create(int fd)
    {
        auto anon_buf = TRY(AnonymousBuffer::create_from_anon_fd(fd, sizeof(SharedMemorySPCQ)));
        auto shared_queue = (SharedMemorySPCQ*)anon_buf.data<void>();
        return create_internal(anon_buf, shared_queue);
    }

    constexpr size_t size() const { return Size; }
    // These functions are provably inconsistent and should only be used as hints to the actual capacity and used count.
    ALWAYS_INLINE size_t weak_remaining_capacity() const { return Size - weak_used(); }
    ALWAYS_INLINE size_t weak_used() const
    {
        auto volatile tail = m_queue->m_queue->m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        auto volatile head = m_queue->m_queue->m_head.load(AK::MemoryOrder::memory_order_relaxed);
        return tail - head;
    }

    ALWAYS_INLINE constexpr int fd() const { return m_queue->fd(); }
    ALWAYS_INLINE constexpr bool is_valid() const { return !m_queue.is_null(); }

    ALWAYS_INLINE constexpr size_t weak_head() const { return m_queue->m_queue->m_head.load(AK::MemoryOrder::memory_order_relaxed); }
    ALWAYS_INLINE constexpr size_t weak_tail() const { return m_queue->m_queue->m_tail.load(AK::MemoryOrder::memory_order_relaxed); }

    template<typename WriteToSlot>
    ErrorOr<void, QueueStatus> enqueue_in_place(WriteToSlot write_to_slot)
    {
        VERIFY(!m_queue.is_null());
        auto tail = m_queue->m_queue->m_tail.load();
        if (tail - m_queue->m_queue->m_head.load() == Size)
            return QueueStatus::Full;
        write_to_slot(m_queue->m_queue->m_data[tail % Size]);
        m_queue->m_queue->m_tail.store(tail + 1);

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
        return m_queue->m_queue->m_tail.load() - m_queue->m_queue->m_head.load() < Size;
    }

    // Repeatedly try to enqueue, using the wait_function to wait if it's not possible
    ErrorOr<void> blocking_enqueue(ValueType to_insert, Function<void()> wait_function)
    {
        ErrorOr<void, QueueStatus> result;
        while (true) {
            result = enqueue(to_insert);
            if (!result.is_error())
                break;
            if (result.error() != QueueStatus::Full)
                return Error::from_string_literal("Unexpected error while enqueuing");

            wait_function();
        }
        return {};
    }

    ErrorOr<ValueType, QueueStatus> dequeue()
    {
        VERIFY(!m_queue.is_null());
        auto head = m_queue->m_queue->m_head.load();
        if (head == m_queue->m_queue->m_tail.load())
            return QueueStatus::Empty;
        auto data = move(m_queue->m_queue->m_data[head % Size]);
        m_queue->m_queue->m_head.store(head + 1);
        return { move(data) };
    }

    // Reads the head without consuming it. Like dequeue(), valid only on the single consumer thread.
    Optional<ValueType> peek() const
    {
        VERIFY(!m_queue.is_null());
        auto head = m_queue->m_queue->m_head.load();
        if (head == m_queue->m_queue->m_tail.load())
            return {};
        return m_queue->m_queue->m_data[head % Size];
    }

    size_t head() const
    {
        return m_queue->m_queue->m_head.load();
    }

private:
    struct SharedMemorySPCQ {
        SharedMemorySPCQ() = default;
        SharedMemorySPCQ(SharedMemorySPCQ const&) = delete;
        SharedMemorySPCQ(SharedMemorySPCQ&&) = delete;
        ~SharedMemorySPCQ() = default;

        // Invariant: tail >= head
        // Invariant: head and tail are monotonically increasing
        // Invariant: tail always points to the next free location where an enqueue can happen.
        // Invariant: head always points to the element to be dequeued next.
        // Invariant: tail is only modified by enqueue functions.
        // Invariant: head is only modified by dequeue functions.
        // An empty queue is signalled with:  tail = head
        // A full queue is signalled with:  tail - head = size
        AK_CACHE_ALIGNED Atomic<size_t, AK::MemoryOrder::memory_order_seq_cst> m_tail { 0 };
        AK_CACHE_ALIGNED Atomic<size_t, AK::MemoryOrder::memory_order_seq_cst> m_head { 0 };

        alignas(ValueType) Array<ValueType, Size> m_data;
    };

    class RefCountedSharedMemorySPCQ
        : public AtomicRefCounted<RefCountedSharedMemorySPCQ>
        , public AnonymousBuffer {
        friend class SharedSingleProducerCircularQueue;

    public:
        SharedMemorySPCQ* m_queue;
        ByteString m_name;

        ~RefCountedSharedMemorySPCQ()
        {
            dbgln_if(SHARED_QUEUE_DEBUG, "destructed SSPCQ at {:p} named {}, shared mem: {:p}", this, m_name, m_queue);
        }

    private:
        RefCountedSharedMemorySPCQ(AnonymousBuffer anon_buf, SharedMemorySPCQ* shared_queue, ByteString name)
            : AnonymousBuffer(anon_buf)
            , m_queue(shared_queue)
            , m_name(move(name))
        {
        }
    };

    static ErrorOr<SharedSingleProducerCircularQueue<T, Size>> create_internal(AnonymousBuffer anon_buf, SharedMemorySPCQ* shared_queue)
    {
        if (!shared_queue)
            return Error::from_string_literal("Unexpected error when creating shared queue from raw memory");
        auto name = ByteString::formatted("SharedSingleProducerCircularQueue@{:x}", anon_buf.fd());
        dbgln_if(SHARED_QUEUE_DEBUG, "successfully mmapped {} at {:p}", name, shared_queue);
        auto ref_counted = new (nothrow) RefCountedSharedMemorySPCQ(anon_buf, shared_queue, move(name));
        return SharedSingleProducerCircularQueue<T, Size> { adopt_ref(*ref_counted) };
    }

    SharedSingleProducerCircularQueue(RefPtr<RefCountedSharedMemorySPCQ> queue)
        : m_queue(queue)
    {
    }

    RefPtr<RefCountedSharedMemorySPCQ> m_queue;
};

}
