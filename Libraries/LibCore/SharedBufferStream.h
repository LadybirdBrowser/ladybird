/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>

namespace Core {

// A shared-memory buffer pool plus two SPSC descriptor rings.
//
// Intended use is high-rate data delivery where the data payload lives in a shared pool and the
// producer/consumer exchange only small descriptors (index + size) over SPSC rings.
//
// Typical topology:
// - Producer acquires a free block from free_ring, writes payload into pool block, then enqueues a
//   descriptor into ready_ring.
// - Consumer dequeues a descriptor from ready_ring, reads payload from pool block, then returns the
//   block index to free_ring.
//
// Both rings are Core::SharedSingleProducerCircularBuffer instances, used as a byte ring with fixed
// descriptor-sized records.
class SharedBufferStream {
public:
    struct Descriptor {
        u32 block_index { 0 };
        u32 used_size { 0 };
    };

    static constexpr u32 s_pool_magic = 0x504F4F4Cu; // "POOL"
    static constexpr u32 s_pool_version = 1;

    struct PoolHeader {
        u32 magic;
        u32 version;
        u32 block_size;
        u32 block_count;
        u32 reserved0;
        u32 reserved1;
        u32 reserved2;
        u32 reserved3;
    };

    SharedBufferStream() = default;

    static size_t pool_buffer_size_bytes(u32 block_size, u32 block_count)
    {
        return sizeof(PoolHeader) + (static_cast<size_t>(block_size) * static_cast<size_t>(block_count));
    }

    static ErrorOr<SharedBufferStream> attach(AnonymousBuffer pool_buffer, AnonymousBuffer ready_ring_buffer, AnonymousBuffer free_ring_buffer)
    {
        SharedBufferStream stream;

        if (!pool_buffer.is_valid() || pool_buffer.size() < sizeof(PoolHeader))
            return Error::from_string_literal("SharedBufferStream: invalid pool buffer");

        auto* header = reinterpret_cast<PoolHeader*>(pool_buffer.data<void>());
        if (!header)
            return Error::from_string_literal("SharedBufferStream: null pool mapping");
        if (header->magic != s_pool_magic)
            return Error::from_string_literal("SharedBufferStream: invalid pool magic");
        if (header->version != s_pool_version)
            return Error::from_string_literal("SharedBufferStream: unsupported pool version");

        auto expected_size = pool_buffer_size_bytes(header->block_size, header->block_count);
        if (pool_buffer.size() < expected_size)
            return Error::from_string_literal("SharedBufferStream: pool buffer too small");

        stream.m_pool_buffer = move(pool_buffer);
        stream.m_pool_header = header;
        stream.m_pool_data = reinterpret_cast<u8*>(header + 1);
        stream.m_ready_ring = TRY(SharedSingleProducerCircularBuffer::attach(move(ready_ring_buffer)));
        stream.m_free_ring = TRY(SharedSingleProducerCircularBuffer::attach(move(free_ring_buffer)));

        return stream;
    }

    bool is_valid() const { return m_pool_header != nullptr && m_ready_ring.is_valid() && m_free_ring.is_valid(); }

    u32 block_size() const
    {
        VERIFY(is_valid());
        return m_pool_header->block_size;
    }

    u32 block_count() const
    {
        VERIFY(is_valid());
        return m_pool_header->block_count;
    }

    Bytes block_bytes(u32 block_index)
    {
        VERIFY(is_valid());
        if (block_index >= block_count())
            return {};
        size_t offset = static_cast<size_t>(block_index) * static_cast<size_t>(block_size());
        return Bytes { m_pool_data + offset, block_size() };
    }

    ReadonlyBytes block_bytes(u32 block_index) const
    {
        VERIFY(is_valid());
        if (block_index >= block_count())
            return {};
        size_t offset = static_cast<size_t>(block_index) * static_cast<size_t>(block_size());
        return ReadonlyBytes { m_pool_data + offset, block_size() };
    }

    // Producer side: best-effort acquire a free block index.
    Optional<u32> try_acquire_block_index()
    {
        auto desc = TRY_OR_IGNORE(try_read_descriptor(m_free_ring));
        if (!desc.has_value())
            return {};
        if (desc->block_index >= block_count())
            return {};
        return desc->block_index;
    }

    // Producer side: best-effort enqueue a ready descriptor for the consumer.
    bool try_submit_ready_block(u32 block_index, u32 used_size)
    {
        if (block_index >= block_count())
            return false;
        if (used_size > block_size())
            return false;
        return TRY_OR_IGNORE(try_write_descriptor(m_ready_ring, Descriptor { block_index, used_size }));
    }

    // Consumer side: best-effort dequeue a ready descriptor.
    Optional<Descriptor> try_receive_ready_block()
    {
        return TRY_OR_IGNORE(try_read_descriptor(m_ready_ring));
    }

    // Consumer side: best-effort return a block index to the producer.
    bool try_release_block_index(u32 block_index)
    {
        if (block_index >= block_count())
            return false;
        return TRY_OR_IGNORE(try_write_descriptor(m_free_ring, Descriptor { block_index, 0 }));
    }

    SharedSingleProducerCircularBuffer& ready_ring() { return m_ready_ring; }
    SharedSingleProducerCircularBuffer& free_ring() { return m_free_ring; }

private:
    static ErrorOr<bool> try_write_descriptor(SharedSingleProducerCircularBuffer& ring, Descriptor descriptor)
    {
        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&descriptor), sizeof(Descriptor) };
        if (ring.available_to_write() < sizeof(Descriptor))
            return false;
        return ring.try_write(bytes) == sizeof(Descriptor);
    }

    static ErrorOr<Optional<Descriptor>> try_read_descriptor(SharedSingleProducerCircularBuffer& ring)
    {
        if (ring.available_to_read() < sizeof(Descriptor))
            return Optional<Descriptor> {};

        Descriptor descriptor;
        auto bytes = Bytes { reinterpret_cast<u8*>(&descriptor), sizeof(Descriptor) };
        auto read = ring.try_read(bytes);
        if (read != sizeof(Descriptor))
            return Optional<Descriptor> {};
        return Optional<Descriptor> { descriptor };
    }

    // Avoid pulling in ErrorOr helpers into all include sites; keep local.
    template<typename T>
    static T TRY_OR_IGNORE(ErrorOr<T> value_or_error)
    {
        if (value_or_error.is_error())
            return {};
        return value_or_error.release_value();
    }

    AnonymousBuffer m_pool_buffer;
    PoolHeader* m_pool_header { nullptr };
    u8* m_pool_data { nullptr };
    SharedSingleProducerCircularBuffer m_ready_ring;
    SharedSingleProducerCircularBuffer m_free_ring;
};

}
