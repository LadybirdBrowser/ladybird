/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Optional.h>
#include <LibCore/AnonymousBuffer.h>

namespace Compositing::WebGL {

// WebContent records commands into the data region and publishes ranges via async IPC;
// the Compositor stores each executed range's flush sequence number into the header.
// The write cursor may rewind to the start only once that counter covers everything
// published, so published bytes are never overwritten while still being read.
class WebGLSharedCommandBuffer {
public:
    static constexpr size_t data_region_offset = 128;
    static constexpr size_t default_data_region_capacity = 32 * MiB;

    WebGLSharedCommandBuffer() = default;

    static Optional<WebGLSharedCommandBuffer> adopt_received_buffer(Core::AnonymousBuffer buffer)
    {
        if (!buffer.is_valid() || buffer.size() <= data_region_offset)
            return {};
        return WebGLSharedCommandBuffer { move(buffer) };
    }

    bool is_valid() const { return m_buffer.is_valid(); }

    void store_executed_flush_sequence_number(u64 flush_sequence_number)
    {
        reinterpret_cast<Atomic<u64>*>(m_buffer.data<u8>())->store(flush_sequence_number, AK::MemoryOrder::memory_order_release);
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
