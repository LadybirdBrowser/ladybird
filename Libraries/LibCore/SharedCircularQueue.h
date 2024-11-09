/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/BuiltinWrappers.h>
#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/NumericLimits.h>
#include <AK/Platform.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Weakable.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>

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
        auto fd = TRY(System::anon_create(sizeof(SharedMemorySPCQ), O_CLOEXEC));
        return create_internal(fd, true);
    }

    // Uses an existing circular queue from given shared memory.
    static ErrorOr<SharedSingleProducerCircularQueue<T, Size>> create(int fd)
    {
        return create_internal(fd, false);
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

    ALWAYS_INLINE constexpr int fd() const { return m_queue->m_fd; }
    ALWAYS_INLINE constexpr bool is_valid() const { return !m_queue.is_null(); }

    ALWAYS_INLINE constexpr size_t weak_head() const { return m_queue->m_queue->m_head.load(AK::MemoryOrder::memory_order_relaxed); }
    ALWAYS_INLINE constexpr size_t weak_tail() const { return m_queue->m_queue->m_tail.load(AK::MemoryOrder::memory_order_relaxed); }

    ErrorOr<void, QueueStatus> enqueue(ValueType to_insert)
    {
        VERIFY(!m_queue.is_null());
        if (!can_enqueue())
            return QueueStatus::Full;
        auto our_tail = m_queue->m_queue->m_tail.load() % Size;
        m_queue->m_queue->m_data[our_tail] = to_insert;
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

    class RefCountedSharedMemorySPCQ : public RefCounted<RefCountedSharedMemorySPCQ> {
        friend class SharedSingleProducerCircularQueue;

    public:
        SharedMemorySPCQ* m_queue;
        void* m_raw;
        int m_fd;

        ~RefCountedSharedMemorySPCQ()
        {
            MUST(System::close(m_fd));
            MUST(System::munmap(m_raw, sizeof(SharedMemorySPCQ)));
            dbgln_if(SHARED_QUEUE_DEBUG, "destructed SSPCQ at {:p}, shared mem: {:p}", this, this->m_raw);
        }

    private:
        RefCountedSharedMemorySPCQ(SharedMemorySPCQ* queue, int fd)
            : m_queue(queue)
            , m_raw(reinterpret_cast<void*>(queue))
            , m_fd(fd)
        {
        }
    };

    static ErrorOr<SharedSingleProducerCircularQueue<T, Size>> create_internal(int fd, bool is_new)
    {
        auto name = ByteString::formatted("SharedSingleProducerCircularQueue@{:x}", fd);
        auto* raw_mapping = TRY(System::mmap(nullptr, sizeof(SharedMemorySPCQ), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0, 0, name));
        dbgln_if(SHARED_QUEUE_DEBUG, "successfully mmapped {} at {:p}", name, raw_mapping);

        SharedMemorySPCQ* shared_queue = is_new ? new (raw_mapping) SharedMemorySPCQ() : reinterpret_cast<SharedMemorySPCQ*>(raw_mapping);

        if (!shared_queue)
            return Error::from_string_literal("Unexpected error when creating shared queue from raw memory");

        return SharedSingleProducerCircularQueue<T, Size> { move(name), adopt_ref(*new (nothrow) RefCountedSharedMemorySPCQ(shared_queue, fd)) };
    }

    SharedSingleProducerCircularQueue(ByteString name, RefPtr<RefCountedSharedMemorySPCQ> queue)
        : m_queue(queue)
        , m_name(move(name))
    {
    }

    RefPtr<RefCountedSharedMemorySPCQ> m_queue;

    ByteString m_name {};
};

}
