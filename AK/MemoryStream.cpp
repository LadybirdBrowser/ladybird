/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 * Copyright (c) 2022, Tim Schumacher <timschumi@gmx.de>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
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

ErrorOr<Bytes> FixedMemoryStream::read_some(Bytes bytes)
{
    auto to_read = min(remaining(), bytes.size());
    if (to_read == 0)
        return Bytes {};

    m_bytes.slice(m_offset, to_read).copy_to(bytes);
    m_offset += to_read;
    return bytes.trim(to_read);
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

void AllocatingMemoryStream::peek_some(Bytes bytes) const
{
    size_t read_bytes = 0;

    auto peek_offset = m_read_offset;
    while (read_bytes < bytes.size()) {
        auto range = MUST(next_read_range(peek_offset));
        if (range.size() == 0)
            break;

        auto copied_bytes = range.copy_trimmed_to(bytes.slice(read_bytes));
        read_bytes += copied_bytes;
        peek_offset += copied_bytes;
    }
}

ErrorOr<Bytes> AllocatingMemoryStream::read_some(Bytes bytes)
{
    size_t read_bytes = 0;

    while (read_bytes < bytes.size()) {
        VERIFY(m_write_offset >= m_read_offset);

        auto range = TRY(next_read_range(m_read_offset));
        if (range.size() == 0)
            break;

        auto copied_bytes = range.copy_trimmed_to(bytes.slice(read_bytes));

        read_bytes += copied_bytes;
        m_read_offset += copied_bytes;
    }

    cleanup_unused_chunks();

    return bytes.trim(read_bytes);
}

ErrorOr<size_t> AllocatingMemoryStream::write_some(ReadonlyBytes bytes)
{
    size_t written_bytes = 0;

    while (written_bytes < bytes.size()) {
        VERIFY(m_write_offset >= m_read_offset);

        auto range = TRY(next_write_range());

        auto copied_bytes = bytes.slice(written_bytes).copy_trimmed_to(range);

        written_bytes += copied_bytes;
        m_write_offset += copied_bytes;
    }

    return written_bytes;
}

ErrorOr<void> AllocatingMemoryStream::discard(size_t count)
{
    VERIFY(m_write_offset >= m_read_offset);

    if (count > used_buffer_size())
        return Error::from_string_literal("Number of discarded bytes is higher than the number of allocated bytes");

    m_read_offset += count;

    cleanup_unused_chunks();

    return {};
}

bool AllocatingMemoryStream::is_eof() const
{
    return used_buffer_size() == 0;
}

bool AllocatingMemoryStream::is_open() const
{
    return true;
}

void AllocatingMemoryStream::close()
{
}

size_t AllocatingMemoryStream::used_buffer_size() const
{
    return m_write_offset - m_read_offset;
}

ErrorOr<Optional<size_t>> AllocatingMemoryStream::offset_of(ReadonlyBytes needle) const
{
    VERIFY(m_write_offset >= m_read_offset);

    if (m_chunks.size() == 0)
        return Optional<size_t> {};

    // Ensure that we don't have empty chunks at the beginning of the stream. Our trimming implementation
    // assumes this to be the case, since this should be held up by `cleanup_unused_chunks()` at all times.
    VERIFY(m_read_offset < CHUNK_SIZE);

    auto empty_chunks_at_end = ((m_chunks.size() * CHUNK_SIZE - m_write_offset) / CHUNK_SIZE);
    auto chunk_count = m_chunks.size() - empty_chunks_at_end;
    auto search_spans = TRY(FixedArray<ReadonlyBytes>::create(chunk_count));

    for (size_t i = 0; i < chunk_count; i++) {
        search_spans[i] = m_chunks[i].span();
    }

    auto used_size_of_last_chunk = m_write_offset % CHUNK_SIZE;

    // The case where the stored write offset is actually the used space is the only case where a result of zero
    // actually is zero. In other cases (i.e. our write offset is beyond the size of a chunk) the write offset
    // already points to the beginning of the next chunk, in that case a result of zero indicates "use the last chunk in full".
    if (m_write_offset >= CHUNK_SIZE && used_size_of_last_chunk == 0)
        used_size_of_last_chunk = CHUNK_SIZE;

    // Trimming is done first to ensure that we don't unintentionally shift around if the first and last chunks are the same.
    search_spans[chunk_count - 1] = search_spans[chunk_count - 1].trim(used_size_of_last_chunk);
    search_spans[0] = search_spans[0].slice(m_read_offset);

    return AK::memmem(search_spans.begin(), search_spans.end(), needle);
}

ErrorOr<ReadonlyBytes> AllocatingMemoryStream::next_read_range(size_t read_offset) const
{
    VERIFY(m_write_offset >= read_offset);

    size_t const chunk_index = read_offset / CHUNK_SIZE;
    size_t const chunk_offset = read_offset % CHUNK_SIZE;
    size_t const read_size = min(CHUNK_SIZE - read_offset % CHUNK_SIZE, m_write_offset - read_offset);

    if (read_size == 0)
        return ReadonlyBytes { static_cast<u8*>(nullptr), 0 };

    VERIFY(chunk_index < m_chunks.size());

    return ReadonlyBytes { m_chunks[chunk_index].data() + chunk_offset, read_size };
}

ErrorOr<Bytes> AllocatingMemoryStream::next_write_range()
{
    VERIFY(m_write_offset >= m_read_offset);

    size_t const chunk_index = m_write_offset / CHUNK_SIZE;
    size_t const chunk_offset = m_write_offset % CHUNK_SIZE;
    size_t const write_size = CHUNK_SIZE - m_write_offset % CHUNK_SIZE;

    if (chunk_index >= m_chunks.size())
        TRY(m_chunks.try_append(TRY(Chunk::create_uninitialized(CHUNK_SIZE))));

    VERIFY(chunk_index < m_chunks.size());

    return Bytes { m_chunks[chunk_index].data() + chunk_offset, write_size };
}

void AllocatingMemoryStream::cleanup_unused_chunks()
{
    VERIFY(m_write_offset >= m_read_offset);

    auto const chunks_to_remove = m_read_offset / CHUNK_SIZE;

    m_chunks.remove(0, chunks_to_remove);

    m_read_offset -= CHUNK_SIZE * chunks_to_remove;
    m_write_offset -= CHUNK_SIZE * chunks_to_remove;
}

}
