/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Debug.h>
#include <LibCore/AnonymousBuffer.h>

namespace Core {

// A non-blocking single-producer/single-consumer circular byte buffer in shared memory
// for low-latency IPC paths (audio/control streams) where the producer must never block.
// The capacity must be a power of two.
class SharedSingleProducerCircularBuffer {
public:
    SharedSingleProducerCircularBuffer() = default;

    static ErrorOr<SharedSingleProducerCircularBuffer> create(size_t capacity);
    static ErrorOr<SharedSingleProducerCircularBuffer> attach(AnonymousBuffer);

    bool is_valid() const { return !m_shared.is_null(); }
    int fd() const
    {
        VERIFY(is_valid());
        return m_shared->fd();
    }

    size_t capacity() const
    {
        VERIFY(is_valid());
        return m_shared->m_shared->capacity;
    }

    // Best-effort: writes up to src.size() bytes. No block.
    size_t try_write(ReadonlyBytes src);

    // Best-effort: reads up to dst.size() bytes. No block.
    size_t try_read(Bytes dst);

    AnonymousBuffer const& anonymous_buffer() const
    {
        VERIFY(is_valid());
        return *m_shared;
    }

private:
    struct SharedMemorySPCB {
        u32 magic { 0 };
        u32 version { 0 };
        u32 capacity { 0 };
        u32 mask { 0 };

        AK_CACHE_ALIGNED Atomic<u32, AK::MemoryOrder::memory_order_seq_cst> write_index { 0 };
        AK_CACHE_ALIGNED Atomic<u32, AK::MemoryOrder::memory_order_seq_cst> read_index { 0 };

        u8 data[0];
    };

    class RefCountedSharedMemory
        : public AtomicRefCounted<RefCountedSharedMemory>
        , public AnonymousBuffer {
        friend class SharedSingleProducerCircularBuffer;

    public:
        ~RefCountedSharedMemory()
        {
            dbgln_if(SHARED_QUEUE_DEBUG, "destructed SharedSingleProducerCircularBuffer at {:p}, shared mem: {:p}", this, m_shared);
        }

    private:
        RefCountedSharedMemory(AnonymousBuffer buffer, SharedMemorySPCB* shared)
            : AnonymousBuffer(move(buffer))
            , m_shared(shared)
        {
        }
        SharedMemorySPCB* m_shared { nullptr };
    };

    static constexpr u32 s_magic = 0x53505342; // "SPSB" Single Producer Shared Buffer
    static constexpr u32 s_version = 1;

    static bool is_power_of_two(size_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    static ErrorOr<SharedSingleProducerCircularBuffer> create_internal(AnonymousBuffer, SharedMemorySPCB*);

    explicit SharedSingleProducerCircularBuffer(RefPtr<RefCountedSharedMemory> shared)
        : m_shared(move(shared))
    {
    }

    ALWAYS_INLINE u8* data() const
    {
        return const_cast<u8*>(reinterpret_cast<u8 const*>(m_shared->m_shared->data));
    }

    RefPtr<RefCountedSharedMemory> m_shared;
};

}
