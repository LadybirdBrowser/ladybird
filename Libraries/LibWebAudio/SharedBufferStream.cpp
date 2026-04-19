/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/SharedBufferStream.h>

namespace Web::WebAudio {

using Audio::SharedCircularBuffer;
using Core::AnonymousBuffer;

static constexpr u32 POOL_MAGIC = 0x504F4F4Cu; // "POOL"

ErrorOr<SharedBufferStreamCreateResult> SharedBufferStream::create(u32 block_size, u32 block_count)
{
    if (block_size == 0 || block_count == 0)
        return Error::from_string_literal("SharedBufferStream: invalid block geometry");

    auto pool_buffer = TRY(AnonymousBuffer::create_with_size(pool_buffer_size_bytes(block_size, block_count)));
    auto* header = pool_buffer.data<PoolHeader>();
    VERIFY(header);

    __builtin_memset(header, 0, sizeof(*header));
    header->magic = POOL_MAGIC;
    header->block_size = block_size;
    header->block_count = block_count;

    size_t ring_capacity_bytes = 64;
    size_t required_capacity = static_cast<size_t>(block_count) * sizeof(Descriptor);
    while (ring_capacity_bytes < required_capacity)
        ring_capacity_bytes <<= 1;

    auto ready_ring = TRY(SharedCircularBuffer::create(ring_capacity_bytes));
    auto free_ring = TRY(SharedCircularBuffer::create(ring_capacity_bytes));

    for (u32 i = 0; i < block_count; ++i) {
        if (!TRY(try_write_descriptor(free_ring, Descriptor { i, 0 })))
            return Error::from_string_literal("SharedBufferStream: failed to seed free ring");
    }

    SharedBufferStreamBuffers buffers {
        .pool_buffer = pool_buffer,
        .ready_ring_buffer = ready_ring.anonymous_buffer(),
        .free_ring_buffer = free_ring.anonymous_buffer(),
    };

    return SharedBufferStreamCreateResult {
        .stream = TRY(attach(buffers.pool_buffer, buffers.ready_ring_buffer, buffers.free_ring_buffer)),
        .buffers = move(buffers),
    };
}

size_t SharedBufferStream::pool_buffer_size_bytes(u32 block_size, u32 block_count)
{
    return sizeof(PoolHeader) + (static_cast<size_t>(block_size) * static_cast<size_t>(block_count));
}

ErrorOr<SharedBufferStream> SharedBufferStream::attach(AnonymousBuffer pool_buffer,
    AnonymousBuffer ready_ring_buffer,
    AnonymousBuffer free_ring_buffer)
{
    SharedBufferStream stream;

    if (!pool_buffer.is_valid() || pool_buffer.size() < sizeof(PoolHeader))
        return Error::from_string_literal("SharedBufferStream: invalid pool buffer");

    PoolHeader* header = pool_buffer.data<PoolHeader>();
    if (!header || header->magic != POOL_MAGIC)
        return Error::from_string_literal("SharedBufferStream: invalid pool magic");

    size_t expected_size = pool_buffer_size_bytes(header->block_size, header->block_count);
    if (pool_buffer.size() < expected_size)
        return Error::from_string_literal("SharedBufferStream: pool buffer too small");

    stream.m_pool_buffer = move(pool_buffer);
    stream.m_pool_header = header;
    stream.m_pool_data = reinterpret_cast<u8*>(header + 1);
    stream.m_ready_ring = TRY(SharedCircularBuffer::attach(move(ready_ring_buffer)));
    stream.m_free_ring = TRY(SharedCircularBuffer::attach(move(free_ring_buffer)));

    return stream;
}

bool SharedBufferStream::is_valid() const
{
    return m_pool_header != nullptr && m_ready_ring.is_valid() && m_free_ring.is_valid();
}

u32 SharedBufferStream::block_size() const
{
    VERIFY(is_valid());
    return m_pool_header->block_size;
}

u32 SharedBufferStream::block_count() const
{
    VERIFY(is_valid());
    return m_pool_header->block_count;
}

Bytes SharedBufferStream::block_bytes(u32 block_index)
{
    VERIFY(is_valid());
    if (block_index >= block_count())
        return {};

    size_t offset = static_cast<size_t>(block_index) * static_cast<size_t>(block_size());
    return Bytes { m_pool_data + offset, block_size() };
}

ReadonlyBytes SharedBufferStream::block_bytes(u32 block_index) const
{
    VERIFY(is_valid());
    if (block_index >= block_count())
        return {};

    size_t offset = static_cast<size_t>(block_index) * static_cast<size_t>(block_size());
    return ReadonlyBytes { m_pool_data + offset, block_size() };
}

template<typename T>
static T TRY_OR_IGNORE(ErrorOr<T> value_or_error)
{
    if (value_or_error.is_error())
        return {};
    return value_or_error.release_value();
}

Optional<u32> SharedBufferStream::try_acquire_block_index()
{
    Optional<Descriptor> desc = TRY_OR_IGNORE(try_read_descriptor(m_free_ring));
    if (!desc.has_value() || desc->block_index >= block_count())
        return {};
    return desc->block_index;
}

bool SharedBufferStream::try_submit_ready_block(u32 block_index, u32 used_size)
{
    if (block_index >= block_count() || used_size > block_size())
        return false;

    return TRY_OR_IGNORE(try_write_descriptor(m_ready_ring, Descriptor { block_index, used_size }));
}

Optional<SharedBufferStream::Descriptor> SharedBufferStream::try_receive_ready_block()
{
    return TRY_OR_IGNORE(try_read_descriptor(m_ready_ring));
}

bool SharedBufferStream::try_release_block_index(u32 block_index)
{
    if (block_index >= block_count())
        return false;

    return TRY_OR_IGNORE(try_write_descriptor(m_free_ring, Descriptor { block_index, 0 }));
}

ErrorOr<bool> SharedBufferStream::try_write_descriptor(SharedCircularBuffer& ring, Descriptor descriptor)
{
    ReadonlyBytes bytes { reinterpret_cast<u8 const*>(&descriptor), sizeof(Descriptor) };
    if (ring.available_to_write() < sizeof(Descriptor))
        return false;

    return ring.try_write(bytes) == sizeof(Descriptor);
}

ErrorOr<Optional<SharedBufferStream::Descriptor>>
SharedBufferStream::try_read_descriptor(SharedCircularBuffer& ring)
{
    if (ring.available_to_read() < sizeof(Descriptor))
        return Optional<Descriptor> {};

    Descriptor descriptor;
    Bytes bytes { reinterpret_cast<u8*>(&descriptor), sizeof(Descriptor) };
    size_t read = ring.try_read(bytes);
    if (read != sizeof(Descriptor))
        return Optional<Descriptor> {};

    return Optional<Descriptor> { descriptor };
}

} // namespace Web::WebAudio
