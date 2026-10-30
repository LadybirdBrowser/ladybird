/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>

namespace AK {

class CircularBuffer {
    AK_MAKE_NONCOPYABLE(CircularBuffer);
    AK_MAKE_DEFAULT_MOVABLE(CircularBuffer);

public:
    static ErrorOr<CircularBuffer> create_empty(size_t size);
    static ErrorOr<CircularBuffer> create_initialized(ByteBuffer);

    ~CircularBuffer() = default;

    size_t write(ReadonlyBytes bytes);
    Bytes read(Bytes bytes);
    ErrorOr<void> discard(size_t discarded_bytes);
    ErrorOr<size_t> fill_from_stream(Stream&);
    ErrorOr<size_t> flush_to_stream(Stream&);

    [[nodiscard]] size_t empty_space() const;
    [[nodiscard]] size_t used_space() const;
    [[nodiscard]] size_t capacity() const;

    Optional<size_t> offset_of(StringView needle, Optional<size_t> from = {}, Optional<size_t> until = {}) const;

    void clear();

private:
    CircularBuffer(ByteBuffer);

    [[nodiscard]] bool is_wrapping_around() const;

    [[nodiscard]] Bytes next_write_span();
    [[nodiscard]] ReadonlyBytes next_read_span(size_t offset = 0) const;

    ByteBuffer m_buffer {};

    size_t m_reading_head {};
    size_t m_used_space {};
};

}
