/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CountingStream.h>

namespace AK {

CountingStream::CountingStream(MaybeOwned<Stream> stream, size_t offset)
    : m_stream(move(stream))
    , m_read_bytes(offset)
{
}

u64 CountingStream::read_bytes() const
{
    return m_read_bytes;
}

ErrorOr<Bytes> CountingStream::read_some(Bytes bytes)
{
    auto result = TRY(m_stream->read_some(bytes));

    m_read_bytes += result.size();

    return result;
}

ErrorOr<void> CountingStream::discard(size_t discarded_bytes)
{
    TRY(m_stream->discard(discarded_bytes));

    m_read_bytes += discarded_bytes;

    return {};
}

ErrorOr<size_t> CountingStream::write_some(ReadonlyBytes bytes)
{
    return m_stream->write_some(bytes);
}

bool CountingStream::is_eof() const
{
    return m_stream->is_eof();
}

bool CountingStream::is_open() const
{
    return m_stream->is_open();
}

void CountingStream::close()
{
}

}
