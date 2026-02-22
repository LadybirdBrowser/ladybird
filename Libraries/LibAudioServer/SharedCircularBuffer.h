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

namespace AudioServer {

// A non-blocking single-producer/single-consumer circular byte buffer in shared memory
// for low-latency IPC paths (audio/control streams) where the producer must never block.
// The capacity must be a power of two.
class SharedCircularBuffer {
public:
    SharedCircularBuffer() = default;

    static ErrorOr<SharedCircularBuffer> create(size_t capacity);
    static ErrorOr<SharedCircularBuffer> attach(Core::AnonymousBuffer);

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

    // Number of bytes currently stored (best-effort snapshot).
    size_t available_to_read() const;

    // Number of bytes that can be written without overwriting unread data (best-effort snapshot).
    size_t available_to_write() const;

    // Discard all unread bytes currently queued in the buffer.
    // This is for producer-side timeline resets, where stale buffered data must be dropped.
    void discard_all();

    Core::AnonymousBuffer const& anonymous_buffer() const
    {
        VERIFY(is_valid());
        return *m_shared;
    }

private:
    struct SharedMemorySPCB {
        u32 magic { 0 };
        u32 capacity { 0 };

        AK_CACHE_ALIGNED Atomic<u32, AK::MemoryOrder::memory_order_seq_cst> write_index { 0 };
        AK_CACHE_ALIGNED Atomic<u32, AK::MemoryOrder::memory_order_seq_cst> read_index { 0 };

        u8 data[0];
    };

    class RefCountedSharedMemory
        : public AtomicRefCounted<RefCountedSharedMemory>
        , public Core::AnonymousBuffer {
        friend class SharedCircularBuffer;

    public:
        ~RefCountedSharedMemory()
        {
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
    static ErrorOr<SharedCircularBuffer> create_internal(Core::AnonymousBuffer, SharedMemorySPCB*);

    explicit SharedCircularBuffer(RefPtr<RefCountedSharedMemory> shared)
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
