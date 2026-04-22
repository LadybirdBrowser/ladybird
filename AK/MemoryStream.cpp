/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 * Copyright (c) 2022, Tim Schumacher <timschumi@gmx.de>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FixedArray.h>
#include <AK/MemMem.h>
#include <AK/MemoryStream.h>

namespace AK {

FixedMemoryStream::FixedMemoryStream(Bytes bytes, Mode mode)
    : m_bytes(bytes)
    , m_writing_enabled(mode == Mode::ReadWrite)
{
}

FixedMemoryStream::FixedMemoryStream(ReadonlyBytes bytes)
    : m_bytes({ const_cast<u8*>(bytes.data()), bytes.size() })
    , m_writing_enabled(false)
{
}

bool FixedMemoryStream::is_eof() const
{
    return m_offset >= m_bytes.size();
}

bool FixedMemoryStream::is_open() const
{
    return true;
}

void FixedMemoryStream::close()
{
    // FIXME: It doesn't make sense to close a memory stream. Therefore, we don't do anything here. Is that fine?
}

ErrorOr<void> FixedMemoryStream::truncate(size_t)
{
    return Error::from_errno(EBADF);
}

ErrorOr<void> FixedMemoryStream::read_until_filled(AK::Bytes bytes)
{
    if (remaining() < bytes.size())
        return Error::from_string_literal("Can't read past the end of the stream memory");

    m_bytes.slice(m_offset).copy_trimmed_to(bytes);
    m_offset += bytes.size();

    return {};
}

ErrorOr<size_t> FixedMemoryStream::seek(i64 offset, SeekMode seek_mode)
{
    switch (seek_mode) {
    case SeekMode::SetPosition:
        if (offset > static_cast<i64>(m_bytes.size()))
            return Error::from_string_literal("Offset past the end of the stream memory");

        m_offset = offset;
        break;
    case SeekMode::FromCurrentPosition:
        if (offset + static_cast<i64>(m_offset) > static_cast<i64>(m_bytes.size()))
            return Error::from_string_literal("Offset past the end of the stream memory");

        m_offset += offset;
        break;
    case SeekMode::FromEndPosition:
        if (-offset > static_cast<i64>(m_bytes.size()))
            return Error::from_string_literal("Offset past the start of the stream memory");

        m_offset = m_bytes.size() + offset;
        break;
    }
    return m_offset;
}

ErrorOr<size_t> FixedMemoryStream::write_some(ReadonlyBytes bytes)
{
    // MemoryStream isn't based on file-descriptors, but since most other
    // Stream implementations are, the interface specifies EBADF as the
    // "we don't support this particular operation" error code.
    if (!m_writing_enabled)
        return Error::from_errno(EBADF);

    // FIXME: Can this not error?
    auto const nwritten = bytes.copy_trimmed_to(m_bytes.slice(m_offset));
    m_offset += nwritten;
    return nwritten;
}

ErrorOr<void> FixedMemoryStream::write_until_depleted(ReadonlyBytes bytes)
{
    if (remaining() < bytes.size())
        return Error::from_string_literal("Write of entire buffer ends past the memory area");

    TRY(write_some(bytes));
    return {};
}

size_t FixedMemoryStream::offset() const
{
    return m_offset;
}

size_t FixedMemoryStream::remaining() const
{
    return m_bytes.size() - m_offset;
}

AllocatingMemoryStream::~AllocatingMemoryStream()
{
    // Iterative teardown to avoid blowing the stack on long chunk chains.
    while (m_head)
        m_head = move(m_head->next);
}

size_t AllocatingMemoryStream::used_buffer_size() const
{
    return m_used_buffer_size;
}

bool AllocatingMemoryStream::is_eof() const
{
    return m_used_buffer_size == 0;
}

bool AllocatingMemoryStream::is_open() const
{
    return true;
}

void AllocatingMemoryStream::close()
{
}

ErrorOr<void> AllocatingMemoryStream::append_new_chunk()
{
    auto new_chunk = adopt_own_if_nonnull(new (nothrow) Chunk);
    if (!new_chunk)
        return Error::from_errno(ENOMEM);

    if (m_tail) {
        m_tail->next = new_chunk.release_nonnull();
        m_tail = m_tail->next.ptr();
    } else {
        m_head = new_chunk.release_nonnull();
        m_tail = m_head.ptr();
    }
    m_tail_write_offset = 0;
    return {};
}

void AllocatingMemoryStream::pop_head_chunk()
{
    VERIFY(m_head);
    if (m_head.ptr() == m_tail) {
        m_head = nullptr;
        m_tail = nullptr;
        m_tail_write_offset = 0;
    } else {
        m_head = move(m_head->next);
    }
    m_head_read_offset = 0;
}

ReadonlyBytes AllocatingMemoryStream::peek_some_contiguous() const
{
    if (!m_head)
        return {};
    auto const end = (m_head.ptr() == m_tail) ? m_tail_write_offset : CHUNK_SIZE;
    return ReadonlyBytes { m_head->data + m_head_read_offset, end - m_head_read_offset };
}

void AllocatingMemoryStream::peek_some(Bytes bytes) const
{
    size_t read_bytes = 0;
    auto const* chunk = m_head.ptr();
    auto chunk_offset = m_head_read_offset;

    while (chunk && read_bytes < bytes.size()) {
        auto const end = (chunk == m_tail) ? m_tail_write_offset : CHUNK_SIZE;
        ReadonlyBytes available { chunk->data + chunk_offset, end - chunk_offset };

        auto copied = available.copy_trimmed_to(bytes.slice(read_bytes));
        read_bytes += copied;

        if (copied < available.size())
            break;

        chunk = chunk->next.ptr();
        chunk_offset = 0;
    }
}

ErrorOr<Bytes> AllocatingMemoryStream::read_some(Bytes bytes)
{
    size_t read_bytes = 0;

    while (read_bytes < bytes.size() && m_head) {
        auto const end = (m_head.ptr() == m_tail) ? m_tail_write_offset : CHUNK_SIZE;
        ReadonlyBytes available { m_head->data + m_head_read_offset, end - m_head_read_offset };

        auto copied = available.copy_trimmed_to(bytes.slice(read_bytes));
        read_bytes += copied;
        m_head_read_offset += copied;
        m_used_buffer_size -= copied;

        if (m_head_read_offset == end)
            pop_head_chunk();
    }

    return bytes.trim(read_bytes);
}

ErrorOr<size_t> AllocatingMemoryStream::write_some(ReadonlyBytes bytes)
{
    size_t written_bytes = 0;

    while (written_bytes < bytes.size()) {
        if (!m_tail || m_tail_write_offset == CHUNK_SIZE)
            TRY(append_new_chunk());

        Bytes tail_remaining { m_tail->data + m_tail_write_offset, CHUNK_SIZE - m_tail_write_offset };
        auto copied = bytes.slice(written_bytes).copy_trimmed_to(tail_remaining);

        m_tail_write_offset += copied;
        written_bytes += copied;
        m_used_buffer_size += copied;
    }

    return written_bytes;
}

ErrorOr<void> AllocatingMemoryStream::discard(size_t count)
{
    if (count > m_used_buffer_size)
        return Error::from_string_literal("Number of discarded bytes is higher than the number of allocated bytes");

    m_used_buffer_size -= count;

    while (count > 0) {
        VERIFY(m_head);
        auto const end = (m_head.ptr() == m_tail) ? m_tail_write_offset : CHUNK_SIZE;
        auto const available = end - m_head_read_offset;
        auto const to_consume = min(available, count);

        m_head_read_offset += to_consume;
        count -= to_consume;

        if (m_head_read_offset == end)
            pop_head_chunk();
    }

    return {};
}

ErrorOr<Optional<size_t>> AllocatingMemoryStream::offset_of(ReadonlyBytes needle) const
{
    if (!m_head)
        return Optional<size_t> {};

    size_t chunk_count = 0;
    for (auto const* chunk = m_head.ptr(); chunk; chunk = chunk->next.ptr())
        ++chunk_count;

    auto search_spans = TRY(FixedArray<ReadonlyBytes>::create(chunk_count));
    size_t i = 0;
    for (auto const* chunk = m_head.ptr(); chunk; chunk = chunk->next.ptr(), ++i) {
        auto const start = (chunk == m_head.ptr()) ? m_head_read_offset : 0;
        auto const end = (chunk == m_tail) ? m_tail_write_offset : CHUNK_SIZE;
        search_spans[i] = ReadonlyBytes { chunk->data + start, end - start };
    }

    return AK::memmem(search_spans.begin(), search_spans.end(), needle);
}

}
