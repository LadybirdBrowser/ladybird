/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/IncrementallyPopulatedStream.h>

namespace Media {

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_empty()
{
    return adopt_ref(*new IncrementallyPopulatedStream({}));
}

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_from_byte_buffer(ByteBuffer&& data)
{
    return adopt_ref(*new IncrementallyPopulatedStream(move(data)));
}

void IncrementallyPopulatedStream::append_chunk(ByteBuffer&& data)
{
    Threading::MutexLocker locker { m_mutex };
    m_data.append(move(data));
    m_data_available.broadcast();
}

void IncrementallyPopulatedStream::mark_complete()
{
    Threading::MutexLocker locker { m_mutex };
    m_complete = true;
    m_data_available.broadcast();
}

u64 IncrementallyPopulatedStream::total_size() const
{
    if (m_complete)
        return m_data.size();
    return *m_total_size;
}

void IncrementallyPopulatedStream::abort_blocking_reads()
{
    Threading::MutexLocker locker { m_mutex };
    ++m_blocking_read_generation;
    m_data_available.broadcast();
}

bool IncrementallyPopulatedStream::has_pending_blocking_reads()
{
    Threading::MutexLocker locker { m_mutex };
    return m_pending_blocking_reads > 0;
}

DecoderErrorOr<size_t> IncrementallyPopulatedStream::read_bytes_at_position_blocking(size_t position, Bytes& bytes)
{
    Threading::MutexLocker locker { m_mutex };
    auto unblock_generation = m_blocking_read_generation;
    while (position + bytes.size() > m_data.size() && !m_complete && unblock_generation == m_blocking_read_generation) {
        ++m_pending_blocking_reads;
        m_data_available.wait();
        --m_pending_blocking_reads;
    }

    if (unblock_generation != m_blocking_read_generation) {
        dbgln(">unlocked reading bytes.size={}", bytes.size());
        return DecoderError::with_description(DecoderErrorCategory::AbortedOperation, "Blocking read was aborted"sv);
    }
    if (position >= m_data.size())
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Blocking read reached end of stream"sv);
    return m_data.bytes().slice(position).copy_trimmed_to(bytes);
}

DecoderErrorOr<void> IncrementallyPopulatedStream::Seekable::seek(size_t position, SeekMode mode)
{
    size_t new_position = m_position;

    switch (mode) {
    case SeekMode::SetPosition:
        new_position = position;
        break;
    case SeekMode::FromCurrentPosition:
        new_position += position;
        break;
    case SeekMode::FromEndPosition:
        new_position = m_stream->total_size() + position;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    Bytes empty;
    TRY(m_stream->read_bytes_at_position_blocking(new_position, empty));

    m_position = new_position;
    return {};
}

DecoderErrorOr<size_t> IncrementallyPopulatedStream::Seekable::read_bytes(Bytes& bytes)
{
    auto read_count = TRY(m_stream->read_bytes_at_position_blocking(m_position, bytes));
    m_position += read_count;
    return read_count;
}

}
