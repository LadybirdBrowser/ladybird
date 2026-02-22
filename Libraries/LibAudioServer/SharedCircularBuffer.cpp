/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <AK/NumericLimits.h>
#include <LibAudioServer/SharedCircularBuffer.h>

namespace AudioServer {

ErrorOr<SharedCircularBuffer> SharedCircularBuffer::create(size_t capacity)
{
    if (!AK::is_power_of_two(capacity))
        return Error::from_string_literal("SharedCircularBuffer capacity must be a power of two");

    if (capacity > static_cast<size_t>(NumericLimits<u32>::max()))
        return Error::from_string_literal("SharedCircularBuffer capacity too large");

    size_t total_size = sizeof(SharedMemorySPCB) + capacity;
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(total_size));
    secure_zero(buffer.data<void>(), total_size);
    auto* shared = new (buffer.data<void>()) SharedMemorySPCB;
    shared->magic = s_magic;
    shared->capacity = static_cast<u32>(capacity);
    shared->write_index.store(0, AK::MemoryOrder::memory_order_relaxed);
    shared->read_index.store(0, AK::MemoryOrder::memory_order_relaxed);

    return create_internal(move(buffer), shared);
}

ErrorOr<SharedCircularBuffer> SharedCircularBuffer::attach(Core::AnonymousBuffer buffer)
{
    if (!buffer.is_valid())
        return Error::from_string_literal("SharedCircularBuffer: buffer is invalid");

    auto* shared = reinterpret_cast<SharedMemorySPCB*>(buffer.data<void>());
    if (!shared)
        return Error::from_string_literal("SharedCircularBuffer: buffer had null mapping");

    if (shared->magic != s_magic)
        return Error::from_string_literal("SharedCircularBuffer: invalid magic");

    if (shared->capacity == 0 || !AK::is_power_of_two(shared->capacity))
        return Error::from_string_literal("SharedCircularBuffer: invalid capacity");

    size_t expected_total_size = sizeof(SharedMemorySPCB) + shared->capacity;
    if (buffer.size() < expected_total_size)
        return Error::from_string_literal("SharedCircularBuffer: buffer too small");

    return create_internal(move(buffer), shared);
}

ErrorOr<SharedCircularBuffer> SharedCircularBuffer::create_internal(Core::AnonymousBuffer buffer, SharedMemorySPCB* shared)
{
    auto ref_counted = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RefCountedSharedMemory(move(buffer), shared)));
    return SharedCircularBuffer { move(ref_counted) };
}

size_t SharedCircularBuffer::try_write(ReadonlyBytes src)
{
    if (!is_valid() || src.is_empty())
        return 0;

    u32 const capacity_value = m_shared->m_shared->capacity;
    u32 const mask_value = capacity_value - 1;

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

size_t SharedCircularBuffer::try_read(Bytes dst)
{
    if (!is_valid() || dst.is_empty())
        return 0;

    u32 const capacity_value = m_shared->m_shared->capacity;
    u32 const mask_value = capacity_value - 1;

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

size_t SharedCircularBuffer::available_to_read() const
{
    if (!is_valid())
        return 0;

    u32 const capacity_value = m_shared->m_shared->capacity;
    auto read = m_shared->m_shared->read_index.load(AK::MemoryOrder::memory_order_acquire);
    auto write = m_shared->m_shared->write_index.load(AK::MemoryOrder::memory_order_acquire);

    return AK::min<u32>(write - read, capacity_value);
}

size_t SharedCircularBuffer::available_to_write() const
{
    if (!is_valid())
        return 0;

    u32 const capacity_value = m_shared->m_shared->capacity;
    u32 available = static_cast<u32>(available_to_read());
    return capacity_value - AK::min<u32>(available, capacity_value);
}

void SharedCircularBuffer::discard_all()
{
    if (!is_valid())
        return;

    auto write = m_shared->m_shared->write_index.load(AK::MemoryOrder::memory_order_acquire);
    m_shared->m_shared->read_index.store(write, AK::MemoryOrder::memory_order_release);
}

}
