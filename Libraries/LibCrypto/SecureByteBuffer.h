/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Memory.h>
#include <AK/Noncopyable.h>

namespace Crypto {

// A ByteBuffer wrapper that securely zeros its contents on destruction.
// Use for key material, passwords, and other secrets that must not persist
// in memory after use.
class SecureByteBuffer {
    AK_MAKE_NONCOPYABLE(SecureByteBuffer);

public:
    SecureByteBuffer() = default;

    static ErrorOr<SecureByteBuffer> create_uninitialized(size_t size)
    {
        SecureByteBuffer buf;
        buf.m_buffer = TRY(ByteBuffer::create_uninitialized(size));
        return buf;
    }

    static ErrorOr<SecureByteBuffer> copy(ReadonlyBytes bytes)
    {
        SecureByteBuffer buf;
        if (bytes.size() > 0) {
            buf.m_buffer = TRY(ByteBuffer::create_uninitialized(bytes.size()));
            __builtin_memcpy(buf.m_buffer.data(), bytes.data(), bytes.size());
        }
        return buf;
    }

    SecureByteBuffer(SecureByteBuffer&& other)
        : m_buffer(move(other.m_buffer))
    {
        // ByteBuffer::move_from() memcpy's inline data, leaving the stale
        // secret in the moved-from inline buffer (m_size=0, m_inline=true).
        // Zero the full inline capacity. Safe for outline buffers too —
        // the pointer was transferred and the inline area is irrelevant.
        secure_zero(other.m_buffer.data(), other.m_buffer.capacity());
    }

    SecureByteBuffer& operator=(SecureByteBuffer&& other)
    {
        if (this != &other) {
            secure_clear();
            m_buffer = move(other.m_buffer);
            secure_zero(other.m_buffer.data(), other.m_buffer.capacity());
        }
        return *this;
    }

    ~SecureByteBuffer()
    {
        secure_clear();
    }

    [[nodiscard]] u8* data() { return m_buffer.data(); }
    [[nodiscard]] u8 const* data() const { return m_buffer.data(); }
    [[nodiscard]] size_t size() const { return m_buffer.size(); }
    [[nodiscard]] bool is_empty() const { return m_buffer.is_empty(); }

    [[nodiscard]] ReadonlyBytes bytes() const { return m_buffer.bytes(); }
    [[nodiscard]] Bytes bytes() { return m_buffer.bytes(); }

    operator ReadonlyBytes() const { return bytes(); }

private:
    void secure_clear()
    {
        if (m_buffer.size() > 0)
            secure_zero(m_buffer.data(), m_buffer.size());
    }

    ByteBuffer m_buffer;
};

}
