/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Span.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>

namespace IPC {

// A value of C++ that Rust takes: C++ encodes it and never decodes it. As an argument of a message that C++ receives,
// it reaches the handler still encoded, for the handler to hand to Rust as its bytes, which are a
// `libipc_rust::cpp_connection::EncodedValue` there.
template<typename T>
class Encoded {
public:
    using ValueType = T;

    Encoded() = default;

    explicit Encoded(T const& value)
    {
        MessageBuffer buffer;
        Encoder encoder { buffer };
        MUST(encoder.encode(value));
        VERIFY(buffer.attachments().is_empty());
        m_bytes = MUST(ByteBuffer::copy(buffer.data().span()));
    }

    static Encoded from_bytes(ByteBuffer bytes)
    {
        Encoded encoded;
        encoded.m_bytes = move(bytes);
        return encoded;
    }

    ReadonlyBytes bytes() const { return m_bytes.bytes(); }

private:
    ByteBuffer m_bytes;
};

}
