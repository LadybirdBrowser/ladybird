/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Optional.h>
#include <LibCore/AnonymousBuffer.h>

namespace Web::WebGL {

// WebContent records commands into the data region and publishes ranges via async IPC;
// the Compositor stores each executed range's flush sequence number into the header.
// The write cursor may rewind to the start only once that counter covers everything
// published, so published bytes are never overwritten while still being read.
class WebGLSharedCommandBuffer {
public:
    static constexpr size_t data_region_offset = 128;
    static constexpr size_t default_data_region_capacity = 32 * MiB;

    WebGLSharedCommandBuffer() = default;

    static ErrorOr<WebGLSharedCommandBuffer> create(size_t data_region_capacity = default_data_region_capacity)
    {
        auto buffer = TRY(Core::AnonymousBuffer::create_with_size(data_region_offset + data_region_capacity));
        return WebGLSharedCommandBuffer { move(buffer) };
    }

    static Optional<WebGLSharedCommandBuffer> adopt_received_buffer(Core::AnonymousBuffer buffer)
    {
        if (!buffer.is_valid() || buffer.size() <= data_region_offset)
            return {};
        return WebGLSharedCommandBuffer { move(buffer) };
    }

    bool is_valid() const { return m_buffer.is_valid(); }
    Core::AnonymousBuffer const& buffer() const { return m_buffer; }

    u64 executed_flush_sequence_number() const
    {
        return reinterpret_cast<Atomic<u64> const*>(m_buffer.data<u8>())->load(AK::MemoryOrder::memory_order_acquire);
    }

    void store_executed_flush_sequence_number(u64 flush_sequence_number)
    {
        reinterpret_cast<Atomic<u64>*>(m_buffer.data<u8>())->store(flush_sequence_number, AK::MemoryOrder::memory_order_release);
    }

    Bytes data_region()
    {
        return { m_buffer.data<u8>() + data_region_offset, m_buffer.size() - data_region_offset };
    }

    ReadonlyBytes data_region() const
    {
        return { m_buffer.data<u8>() + data_region_offset, m_buffer.size() - data_region_offset };
    }

private:
    explicit WebGLSharedCommandBuffer(Core::AnonymousBuffer buffer)
        : m_buffer(move(buffer))
    {
    }

    Core::AnonymousBuffer m_buffer;
};

}
