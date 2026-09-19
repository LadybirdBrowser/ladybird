/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Stream.h>

namespace Fuzzing {

class BoundedInput {
public:
    explicit BoundedInput(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    u8 byte()
    {
        if (m_bytes.is_empty())
            return 0;
        auto result = m_bytes[0];
        m_bytes = m_bytes.slice(1);
        return result;
    }

    ReadonlyBytes take(size_t count)
    {
        auto result = m_bytes.slice(0, min(count, m_bytes.size()));
        m_bytes = m_bytes.slice(result.size());
        return result;
    }

    ReadonlyBytes remaining() const { return m_bytes; }

private:
    ReadonlyBytes m_bytes;
};

// A short read is not EOF. Never return zero while readable input remains.
// This models fragmentation, not a nonblocking stream returning EAGAIN.
class ChunkedInputStream final : public Stream {
public:
    ChunkedInputStream(ReadonlyBytes bytes, size_t chunk_size)
        : m_bytes(bytes)
        , m_chunk_size(max(size_t { 1 }, chunk_size))
    {
    }

    ErrorOr<Bytes> read_some(Bytes output) override
    {
        if (!m_open)
            return Error::from_string_literal("Read from closed fuzz input");
        auto count = min(output.size(), min(m_bytes.size(), m_chunk_size));
        m_bytes.slice(0, count).copy_to(output);
        m_bytes = m_bytes.slice(count);
        return output.slice(0, count);
    }

    ErrorOr<size_t> write_some(ReadonlyBytes) override { return Error::from_string_literal("Read-only fuzz input"); }
    bool is_eof() const override { return !m_open || m_bytes.is_empty(); }
    bool is_open() const override { return m_open; }
    void close() override { m_open = false; }

private:
    ReadonlyBytes m_bytes;
    size_t m_chunk_size;
    bool m_open { true };
};

}
