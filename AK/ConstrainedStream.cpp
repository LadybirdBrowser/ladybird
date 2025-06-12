/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConstrainedStream.h>

namespace AK {

ConstrainedStream::ConstrainedStream(MaybeOwned<Stream> stream, u64 limit)
    : m_stream(move(stream))
    , m_limit(limit)
{
}

ErrorOr<Bytes> ConstrainedStream::read_some(Bytes bytes)
{
    if (bytes.size() >= m_limit)
        bytes = bytes.trim(m_limit);

    auto result = TRY(m_stream->read_some(bytes));

    m_limit -= result.size();

    return result;
}

ErrorOr<void> ConstrainedStream::discard(size_t discarded_bytes)
{
    if (discarded_bytes >= m_limit)
        return Error::from_string_literal("Trying to discard more bytes than allowed");

    // Note: This will remove more bytes from the limit than intended if a failing discard yields partial results.
    //       However, stopping early is most likely better than running over the end of the stream.
    m_limit -= discarded_bytes;
    TRY(m_stream->discard(discarded_bytes));

    return {};
}

ErrorOr<size_t> ConstrainedStream::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool ConstrainedStream::is_eof() const
{
    return m_limit == 0 || m_stream->is_eof();
}

bool ConstrainedStream::is_open() const
{
    return m_stream->is_open();
}

void ConstrainedStream::close()
{
}

}
