/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

namespace Media {

static constexpr u64 PRECEDING_DATA_SIZE = 1 * KiB;
static constexpr u64 FORWARD_REQUEST_THRESHOLD = 1 * MiB;
static constexpr AK::Duration CURSOR_ACTIVE_TIME = AK::Duration::from_milliseconds(50);

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_empty()
{
    return adopt_ref(*new IncrementallyPopulatedStream());
}

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_from_data(ReadonlyBytes data)
{
    auto stream = create_empty();
    stream->add_chunk_at(0, data);
    stream->reached_end_of_body();
    VERIFY(stream->size() == data.size());
    return stream;
}

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_from_buffer(ByteBuffer const& buffer)
{
    return create_from_data(buffer.bytes());
}

IncrementallyPopulatedStream::IncrementallyPopulatedStream() = default;

IncrementallyPopulatedStream::~IncrementallyPopulatedStream() = default;

void IncrementallyPopulatedStream::set_data_request_callback(DataRequestCallback callback)
{
    Threading::MutexLocker locker { m_mutex };
    m_callback_event_loop = Core::EventLoop::current_weak();
    m_data_request_callback = move(callback);
}

void IncrementallyPopulatedStream::add_chunk_at(u64 offset, ReadonlyBytes data)
{
    VERIFY(!data.is_null());
    VERIFY(!data.is_empty());
    auto new_chunk_end = offset + data.size();
    m_last_chunk_end = new_chunk_end;

    Threading::MutexLocker locker { m_mutex };

    auto previous_chunk_iter = m_chunks.find_largest_not_above_iterator(offset);

    // Add a new chunk to the collection if there are none.
    if (previous_chunk_iter.is_end() || previous_chunk_iter->end() < offset) {
        DataChunk new_chunk { offset, MUST(ByteBuffer::copy(data)) };
        m_chunks.insert(offset, move(new_chunk));
        m_state_changed.broadcast();
        return;
    }

    auto& chunk = *previous_chunk_iter;
    auto& buffer = chunk.data();

    if (chunk.size() >= new_chunk_end) {
        // The chunk is fully covered by the existing chunk, skip until after it.
        begin_new_request_while_locked(chunk.end());
        return;
    }

    // Expand the existing chunk to contain this new data.
    buffer.resize(new_chunk_end - chunk.offset());
    data.copy_to(buffer.bytes().slice(offset - chunk.offset()));

    // Join the chunk to the next one if they intersect.
    auto next_chunk_iter = previous_chunk_iter;
    ++next_chunk_iter;

    if (!next_chunk_iter.is_end() && next_chunk_iter->offset() <= previous_chunk_iter->end()) {
        auto& next_chunk = *next_chunk_iter;

        buffer.resize(next_chunk.end() - chunk.offset());
        next_chunk.data().bytes().copy_to(buffer.bytes().slice(next_chunk.offset() - chunk.offset()));

        VERIFY(m_chunks.remove(next_chunk.offset()));

        begin_new_request_while_locked(chunk.end());
    }

    m_state_changed.broadcast();
}

void IncrementallyPopulatedStream::reached_end_of_body()
{
    Threading::MutexLocker locker { m_mutex };
    m_expected_size = m_last_chunk_end;
    m_closed = true;
    m_state_changed.broadcast();
}

u64 IncrementallyPopulatedStream::size()
{
    Threading::MutexLocker locker { m_mutex };
    while (!m_expected_size.has_value())
        m_state_changed.wait();
    return m_expected_size.value();
}

void IncrementallyPopulatedStream::set_expected_size(u64 expected_size)
{
    Threading::MutexLocker locker { m_mutex };
    m_expected_size = expected_size;
    m_state_changed.broadcast();
}

Optional<u64> IncrementallyPopulatedStream::expected_size() const
{
    Threading::MutexLocker locker { m_mutex };
    return m_expected_size;
}

void IncrementallyPopulatedStream::begin_new_request_while_locked(u64 position)
{
    if (position == m_currently_requested_position)
        return;

    m_currently_requested_position = position;
    m_last_chunk_end = position;

    if (m_expected_size.has_value() && position >= m_expected_size.value())
        return;

    auto event_loop = m_callback_event_loop->take();
    if (!event_loop)
        return;
    event_loop->deferred_invoke([stream = NonnullRefPtr(*this), position] {
        if (stream->m_data_request_callback)
            stream->m_data_request_callback(position);
    });
}

static u64 adjust_request_position(u64 position)
{
    if (position > PRECEDING_DATA_SIZE)
        return position - PRECEDING_DATA_SIZE;
    return 0;
}

bool IncrementallyPopulatedStream::check_if_data_is_available_or_begin_request_while_locked(MonotonicTime now, u64 position, u64 length)
{
    auto* chunk = m_chunks.find_largest_not_above(position);
    if (!chunk)
        return m_closed;

    VERIFY(position >= chunk->offset());

    auto potential_request_position = adjust_request_position(position);
    potential_request_position = max(chunk->end(), position);
    for (size_t i = 0; i < m_cursors.size(); i++) {
        auto const& other_cursor = m_cursors[i];
        if (now >= other_cursor.m_active_timeout && !other_cursor.m_blocked)
            continue;
        if (other_cursor.m_position < potential_request_position) {
            auto* other_cursor_chunk = m_chunks.find_largest_not_above(other_cursor.m_position);
            if (other_cursor_chunk && other_cursor_chunk->end() >= other_cursor.m_position) {
                potential_request_position = other_cursor_chunk->end();
                continue;
            }
            potential_request_position = other_cursor.m_position;
        }
    }
    if (m_currently_requested_position > potential_request_position || potential_request_position > m_last_chunk_end + FORWARD_REQUEST_THRESHOLD)
        begin_new_request_while_locked(potential_request_position);

    u64 end = position + length;
    if (m_closed && end > m_expected_size.value())
        end = m_expected_size.value();
    return end <= chunk->end();
}

size_t IncrementallyPopulatedStream::read_from_chunks_while_locked(u64 position, Bytes& bytes) const
{
    auto chunk_iterator = m_chunks.find_largest_not_above_iterator(position);
    VERIFY(!chunk_iterator.is_end());
    auto const& chunk = *chunk_iterator;
    VERIFY(position >= chunk.offset());
    auto end = position + bytes.size();
    auto copy_size = bytes.size();
    if (end > chunk.end()) {
        VERIFY(m_expected_size.has_value());
        VERIFY(chunk.end() == m_expected_size.value());
        end = chunk.end();
        copy_size = end - position;
    }

    u64 offset_in_chunk = position - chunk.offset();
    auto source = chunk.data().span().slice(offset_in_chunk, copy_size);
    source.copy_to(bytes);
    return copy_size;
}

DecoderErrorOr<size_t> IncrementallyPopulatedStream::read_at(Cursor& cursor, size_t position, Bytes& bytes)
{
    Threading::MutexLocker locker { m_mutex };

    auto now = MonotonicTime::now_coarse();
    cursor.m_active_timeout = now + CURSOR_ACTIVE_TIME;

    while (!cursor.m_aborted) {
        if (check_if_data_is_available_or_begin_request_while_locked(now, position, bytes.size()))
            break;

        cursor.m_blocked = true;
        m_state_changed.wait();
        cursor.m_blocked = false;
    }

    if (cursor.m_aborted)
        return DecoderError::with_description(DecoderErrorCategory::Aborted, "Blocking read was aborted"sv);

    if (m_closed && position >= m_expected_size.value())
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Blocking read reached end of stream"sv);

    if (bytes.size() == 0)
        return 0;

    return read_from_chunks_while_locked(position, bytes);
}

NonnullRefPtr<MediaStreamCursor> IncrementallyPopulatedStream::create_cursor()
{
    return adopt_ref(*new Cursor(NonnullRefPtr { *this }));
}

IncrementallyPopulatedStream::Cursor::Cursor(NonnullRefPtr<IncrementallyPopulatedStream> const& stream)
    : m_stream(stream)
{
    Threading::MutexLocker locker { m_stream->m_mutex };
    m_stream->m_cursors.append(*this);
}

IncrementallyPopulatedStream::Cursor::~Cursor()
{
    Threading::MutexLocker locker { m_stream->m_mutex };
    VERIFY(m_stream->m_cursors.remove_first_matching([&](Cursor const& cursor) { return this == &cursor; }));
}

DecoderErrorOr<void> IncrementallyPopulatedStream::Cursor::seek(i64 offset, SeekMode mode)
{
    switch (mode) {
    case SeekMode::SetPosition:
        m_position = offset;
        break;
    case SeekMode::FromCurrentPosition:
        m_position += offset;
        break;
    case SeekMode::FromEndPosition:
        m_position = this->size() + offset;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    m_active_timeout = MonotonicTime::now_coarse() + CURSOR_ACTIVE_TIME;
    return {};
}

DecoderErrorOr<size_t> IncrementallyPopulatedStream::Cursor::read_into(Bytes bytes)
{
    auto read_count = TRY(m_stream->read_at(*this, m_position, bytes));
    m_position += read_count;
    return read_count;
}

void IncrementallyPopulatedStream::Cursor::abort()
{
    Threading::MutexLocker locker { m_stream->m_mutex };
    m_aborted = true;
    m_stream->m_state_changed.broadcast();
}

}
