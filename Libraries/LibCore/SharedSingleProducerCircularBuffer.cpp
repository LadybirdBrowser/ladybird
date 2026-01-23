/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>

namespace Core {

ErrorOr<SharedSingleProducerCircularBuffer> SharedSingleProducerCircularBuffer::create(size_t capacity)
{
    if (!is_power_of_two(capacity))
        return Error::from_string_literal("SharedSingleProducerCircularBuffer capacity must be a power of two");

    if (capacity > static_cast<size_t>(NumericLimits<u32>::max()))
        return Error::from_string_literal("SharedSingleProducerCircularBuffer capacity too large");

    size_t total_size = sizeof(SharedMemorySPCB) + capacity;
    auto buffer = TRY(AnonymousBuffer::create_with_size(total_size));

    auto* shared = new (buffer.data<void>()) SharedMemorySPCB;
    shared->magic = s_magic;
    shared->version = s_version;
    shared->capacity = static_cast<u32>(capacity);
    shared->mask = static_cast<u32>(capacity - 1);
    shared->write_index.store(0, AK::MemoryOrder::memory_order_relaxed);
    shared->read_index.store(0, AK::MemoryOrder::memory_order_relaxed);

    return create_internal(move(buffer), shared);
}

ErrorOr<SharedSingleProducerCircularBuffer> SharedSingleProducerCircularBuffer::attach(AnonymousBuffer buffer)
{
    if (!buffer.is_valid())
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: buffer is invalid");

    auto* shared = reinterpret_cast<SharedMemorySPCB*>(buffer.data<void>());
    if (!shared)
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: buffer had null mapping");

    if (shared->magic != s_magic)
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: invalid magic");
    if (shared->version != s_version)
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: unsupported version");

    if (shared->capacity == 0 || !is_power_of_two(shared->capacity))
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: invalid capacity");
    if (shared->mask != shared->capacity - 1)
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: invalid mask");

    size_t expected_total_size = sizeof(SharedMemorySPCB) + shared->capacity;
    if (buffer.size() < expected_total_size)
        return Error::from_string_literal("SharedSingleProducerCircularBuffer: buffer too small");

    return create_internal(move(buffer), shared);
}

ErrorOr<SharedSingleProducerCircularBuffer> SharedSingleProducerCircularBuffer::create_internal(AnonymousBuffer buffer, SharedMemorySPCB* shared)
{
    auto ref_counted = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RefCountedSharedMemory(move(buffer), shared)));
    return SharedSingleProducerCircularBuffer { move(ref_counted) };
}

size_t SharedSingleProducerCircularBuffer::try_write(ReadonlyBytes src)
{
    if (!is_valid() || src.is_empty())
        return 0;

    u32 const capacity_value = m_shared->m_shared->capacity;
    u32 const mask_value = m_shared->m_shared->mask;

    auto write = m_shared->m_shared->write_index.load(AK::MemoryOrder::memory_order_relaxed);
    auto read = m_shared->m_shared->read_index.load(AK::MemoryOrder::memory_order_acquire);

    u32 used = write - read;
    u32 free = capacity_value - used;
    if (free == 0)
        return 0;

    u32 to_write = min<u32>(static_cast<u32>(src.size()), free);
    u32 write_pos = write & mask_value;

    size_t first_chunk = min<size_t>(to_write, capacity_value - write_pos);
    __builtin_memcpy(data() + write_pos, src.data(), first_chunk);

    size_t remaining = to_write - first_chunk;
    if (remaining > 0)
        __builtin_memcpy(data(), src.data() + first_chunk, remaining);

    m_shared->m_shared->write_index.store(write + to_write, AK::MemoryOrder::memory_order_release);
    return to_write;
}

size_t SharedSingleProducerCircularBuffer::try_read(Bytes dst)
{
    if (!is_valid() || dst.is_empty())
        return 0;

    u32 const capacity_value = m_shared->m_shared->capacity;
    u32 const mask_value = m_shared->m_shared->mask;

    auto read = m_shared->m_shared->read_index.load(AK::MemoryOrder::memory_order_relaxed);
    auto write = m_shared->m_shared->write_index.load(AK::MemoryOrder::memory_order_acquire);

    u32 available = write - read;
    if (available == 0)
        return 0;

    u32 to_read = min<u32>(static_cast<u32>(dst.size()), available);
    u32 read_pos = read & mask_value;

    size_t first_chunk = min<size_t>(to_read, capacity_value - read_pos);
    __builtin_memcpy(dst.data(), data() + read_pos, first_chunk);

    size_t remaining = to_read - first_chunk;
    if (remaining > 0)
        __builtin_memcpy(dst.data() + first_chunk, data(), remaining);

    m_shared->m_shared->read_index.store(read + to_read, AK::MemoryOrder::memory_order_release);
    return to_read;
}

}
