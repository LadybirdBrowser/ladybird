/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CircularBuffer.h>
#include <AK/MemMem.h>
#include <AK/Stream.h>

namespace AK {

CircularBuffer::CircularBuffer(ByteBuffer buffer)
    : m_buffer(move(buffer))
{
}

ErrorOr<CircularBuffer> CircularBuffer::create_empty(size_t size)
{
    auto temporary_buffer = TRY(ByteBuffer::create_uninitialized(size));

    CircularBuffer circular_buffer { move(temporary_buffer) };

    return circular_buffer;
}

ErrorOr<CircularBuffer> CircularBuffer::create_initialized(ByteBuffer buffer)
{
    CircularBuffer circular_buffer { move(buffer) };

    circular_buffer.m_used_space = circular_buffer.m_buffer.size();

    return circular_buffer;
}

size_t CircularBuffer::empty_space() const
{
    return capacity() - m_used_space;
}

size_t CircularBuffer::used_space() const
{
    return m_used_space;
}

size_t CircularBuffer::capacity() const
{
    return m_buffer.size();
}

bool CircularBuffer::is_wrapping_around() const
{
    return capacity() <= m_reading_head + m_used_space;
}

Optional<size_t> CircularBuffer::offset_of(StringView needle, Optional<size_t> from, Optional<size_t> until) const
{
    auto const read_from = from.value_or(0);
    auto const read_until = until.value_or(m_used_space);
    VERIFY(read_from <= read_until);

    Array<ReadonlyBytes, 2> spans {};
    spans[0] = next_read_span();
    auto const original_span_0_size = spans[0].size();

    if (read_from > 0)
        spans[0] = spans[0].slice(min(spans[0].size(), read_from));

    if (spans[0].size() + read_from > read_until)
        spans[0] = spans[0].trim(read_until - read_from);
    else if (is_wrapping_around())
        spans[1] = m_buffer.span().slice(max(original_span_0_size, read_from) - original_span_0_size, min(read_until, m_used_space) - original_span_0_size);

    auto maybe_found = AK::memmem(spans.begin(), spans.end(), needle.bytes());
    if (maybe_found.has_value())
        *maybe_found += read_from;

    return maybe_found;
}

void CircularBuffer::clear()
{
    m_reading_head = 0;
    m_used_space = 0;
}

Bytes CircularBuffer::next_write_span()
{
    if (is_wrapping_around())
        return m_buffer.span().slice(m_reading_head + m_used_space - capacity(), capacity() - m_used_space);
    return m_buffer.span().slice(m_reading_head + m_used_space, capacity() - (m_reading_head + m_used_space));
}

ReadonlyBytes CircularBuffer::next_read_span(size_t offset) const
{
    auto reading_head = m_reading_head;
    auto used_space = m_used_space;

    if (offset > 0) {
        if (offset >= used_space)
            return Bytes {};

        reading_head = (reading_head + offset) % capacity();
        used_space -= offset;
    }

    return m_buffer.span().slice(reading_head, min(capacity() - reading_head, used_space));
}

size_t CircularBuffer::write(ReadonlyBytes bytes)
{
    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_write_span();
        if (next_span.size() == 0)
            break;

        auto const written_bytes = bytes.slice(bytes.size() - remaining).copy_trimmed_to(next_span);

        m_used_space += written_bytes;

        remaining -= written_bytes;
    }

    return bytes.size() - remaining;
}

Bytes CircularBuffer::read(Bytes bytes)
{
    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_read_span();
        if (next_span.size() == 0)
            break;

        auto written_bytes = next_span.copy_trimmed_to(bytes.slice(bytes.size() - remaining));

        m_used_space -= written_bytes;
        m_reading_head += written_bytes;

        if (m_reading_head >= capacity())
            m_reading_head -= capacity();

        remaining -= written_bytes;
    }

    return bytes.trim(bytes.size() - remaining);
}

ErrorOr<void> CircularBuffer::discard(size_t discarding_size)
{
    if (m_used_space < discarding_size)
        return Error::from_string_literal("Can not discard more data than what the buffer contains");
    m_used_space -= discarding_size;
    m_reading_head = (m_reading_head + discarding_size) % capacity();

    return {};
}

ErrorOr<size_t> CircularBuffer::fill_from_stream(Stream& stream)
{
    auto next_span = next_write_span();
    if (next_span.size() == 0)
        return 0;

    auto bytes = TRY(stream.read_some(next_span));
    m_used_space += bytes.size();

    return bytes.size();
}

ErrorOr<size_t> CircularBuffer::flush_to_stream(Stream& stream)
{
    auto next_span = next_read_span();
    if (next_span.size() == 0)
        return 0;

    auto written_bytes = TRY(stream.write_some(next_span));

    m_used_space -= written_bytes;
    m_reading_head += written_bytes;

    if (m_reading_head >= capacity())
        m_reading_head -= capacity();

    return written_bytes;
}

}
