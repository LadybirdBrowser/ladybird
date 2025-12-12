/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

namespace Media {

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_empty()
{
    return adopt_ref(*new IncrementallyPopulatedStream({}, false));
}

NonnullRefPtr<IncrementallyPopulatedStream> IncrementallyPopulatedStream::create_from_buffer(ByteBuffer&& buffer)
{
    return adopt_ref(*new IncrementallyPopulatedStream(move(buffer), true));
}

void IncrementallyPopulatedStream::append(ByteBuffer&& buffer)
{
    Threading::MutexLocker locker { m_mutex };
    m_buffer.append(buffer);
    m_state_changed.broadcast();
}

void IncrementallyPopulatedStream::close()
{
    Threading::MutexLocker locker { m_mutex };
    m_closed = true;
    m_state_changed.broadcast();
}

u64 IncrementallyPopulatedStream::size()
{
    Threading::MutexLocker locker { m_mutex };
    while (!m_closed)
        m_state_changed.wait();
    return m_buffer.size();
}

DecoderErrorOr<size_t> IncrementallyPopulatedStream::read_at(size_t position, Bytes& bytes)
{
    Threading::MutexLocker locker { m_mutex };
    while (position + bytes.size() > m_buffer.size() && !m_closed) {
        m_state_changed.wait();
    }

    if (position >= m_buffer.size())
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Blocking read reached end of stream"sv);
    return m_buffer.bytes().slice(position).copy_trimmed_to(bytes);
}

DecoderErrorOr<void> IncrementallyPopulatedStream::Cursor::seek(size_t offset, SeekMode mode)
{
    size_t new_position = m_position;

    switch (mode) {
    case SeekMode::SetPosition:
        new_position = offset;
        break;
    case SeekMode::FromCurrentPosition:
        new_position += offset;
        break;
    case SeekMode::FromEndPosition:
        new_position = this->size() + offset;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    Bytes empty;
    TRY(m_stream->read_at(new_position, empty));

    m_position = new_position;
    return {};
}

DecoderErrorOr<size_t> IncrementallyPopulatedStream::Cursor::read_into(Bytes& bytes)
{
    auto read_count = TRY(m_stream->read_at(m_position, bytes));
    m_position += read_count;
    return read_count;
}

}
