/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/BuiltinWrappers.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Function.h>
#include <LibCore/AnonymousBuffer.h>

namespace Core {

// A circular lock-free queue (or a buffer) with a single producer,
// residing in shared memory and designed to be accessible to multiple processes.
// This implementation makes use of the fact that any producer-related code can be sure that
// it's the only producer-related code that is running, which simplifies a bunch of the synchronization code.
// The exclusivity and liveliness for critical sections in this class is proven to be correct
// under the assumption of correct synchronization primitives, i.e. atomics.
// In many circumstances, this is enough for cross-process queues.
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
        auto volatile head = m_queue->m_queue->m_tail.load(AK::MemoryOrder::memory_order_relaxed);
        auto volatile tail = m_queue->m_queue->m_head.load(AK::MemoryOrder::memory_order_relaxed);
        return head - tail;
    }

    ALWAYS_INLINE constexpr int fd() const { return m_queue->fd(); }
    ALWAYS_INLINE constexpr bool is_valid() const { return !m_queue.is_null(); }

    ALWAYS_INLINE constexpr size_t weak_head() const { return m_queue->m_queue->m_head.load(AK::MemoryOrder::memory_order_relaxed); }
    ALWAYS_INLINE constexpr size_t weak_tail() const { return m_queue->m_queue->m_tail.load(AK::MemoryOrder::memory_order_relaxed); }

    ErrorOr<void, QueueStatus> enqueue(ValueType to_insert)
    {
        VERIFY(!m_queue.is_null());
        if (!can_enqueue())
            return QueueStatus::Full;
        auto our_tail = m_queue->m_queue->m_tail.load() % Size;
        m_queue->m_queue->m_data[our_tail] = move(to_insert);
        m_queue->m_queue->m_tail.fetch_add(1);

        return {};
    }

    ALWAYS_INLINE bool can_enqueue() const
    {
        return ((head() - 1) % Size) != (m_queue->m_queue->m_tail.load() % Size);
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
        while (true) {
            // This CAS only succeeds if nobody is currently dequeuing.
            auto size_max = NumericLimits<size_t>::max();
            if (m_queue->m_queue->m_head_protector.compare_exchange_strong(size_max, m_queue->m_queue->m_head.load())) {
                auto old_head = m_queue->m_queue->m_head.load();
                // This check looks like it's in a weird place (especially since we have to roll back the protector), but it's actually protecting against a race between multiple dequeuers.
                if (old_head >= m_queue->m_queue->m_tail.load()) {
                    m_queue->m_queue->m_head_protector.store(NumericLimits<size_t>::max(), AK::MemoryOrder::memory_order_release);
                    return QueueStatus::Empty;
                }
                auto data = move(m_queue->m_queue->m_data[old_head % Size]);
                m_queue->m_queue->m_head.fetch_add(1);
                m_queue->m_queue->m_head_protector.store(NumericLimits<size_t>::max(), AK::MemoryOrder::memory_order_release);
                return { move(data) };
            }
        }
    }

    // The "real" head as seen by the outside world. Don't use m_head directly unless you know what you're doing.
    size_t head() const
    {
        return min(m_queue->m_queue->m_head.load(), m_queue->m_queue->m_head_protector.load());
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
        // A full queue is signalled with:  head - 1 mod size = tail mod size  (i.e. head and tail point to the same index in the data array)
        // FIXME: These invariants aren't proven to be correct after each successful completion of each operation where it is relevant.
        //        The work could be put in but for now I think the algorithmic correctness proofs of the functions are enough.
        AK_CACHE_ALIGNED Atomic<size_t, AK::MemoryOrder::memory_order_seq_cst> m_tail { 0 };
        AK_CACHE_ALIGNED Atomic<size_t, AK::MemoryOrder::memory_order_seq_cst> m_head { 0 };
        AK_CACHE_ALIGNED Atomic<size_t, AK::MemoryOrder::memory_order_seq_cst> m_head_protector { NumericLimits<size_t>::max() };

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
            : AnonymousBuffer(move(anon_buf))
            , m_queue(shared_queue)
            , m_name(move(name))
        {
        }
    };

    static ErrorOr<SharedSingleProducerCircularQueue<T, Size>> create_internal(AnonymousBuffer const& anon_buf, SharedMemorySPCQ* shared_queue)
    {
        if (!shared_queue)
            return Error::from_string_literal("Unexpected error when creating shared queue from raw memory");
        auto name = ByteString::formatted("SharedSingleProducerCircularQueue@{:x}", anon_buf.fd());
        dbgln_if(SHARED_QUEUE_DEBUG, "successfully mmapped {} at {:p}", name, shared_queue);
        auto ref_counted = new (nothrow) RefCountedSharedMemorySPCQ(anon_buf, shared_queue, move(name));
        return SharedSingleProducerCircularQueue<T, Size> { adopt_ref(*ref_counted) };
    }

    SharedSingleProducerCircularQueue(RefPtr<RefCountedSharedMemorySPCQ> queue)
        : m_queue(move(queue))
    {
    }

    RefPtr<RefCountedSharedMemorySPCQ> m_queue;
};

}
