/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/ImageDecoderStream.h>

namespace Gfx {

ErrorOr<Bytes> ImageDecoderStream::read_some(Bytes bytes)
{
    Threading::MutexLocker locker(m_mutex);

    size_t read_bytes = 0;

    if (m_chunks.is_empty()) {
        VERIFY(m_chunk_index == 0);
        m_waiting_for_more_data.wait_while([this] {
            return !m_closed && m_chunks.is_empty();
        });

        if (m_closed && m_chunks.is_empty())
            return Bytes {};
    }

    while (read_bytes < bytes.size()) {
        VERIFY(m_chunk_index < m_chunks.size());
        auto const* chunk = m_chunks.find(m_chunk_index);

        VERIFY(m_offset_inside_chunk <= chunk->size());
        auto destination_span = bytes.slice(read_bytes);
        auto const chunk_span = chunk->span().slice(m_offset_inside_chunk);

        auto copied_bytes = chunk_span.copy_trimmed_to(destination_span);
        read_bytes += copied_bytes;

        if (copied_bytes == chunk_span.size()) {
            auto next_chunk_index = m_chunk_index + 1;

            m_waiting_for_more_data.wait_while([this, next_chunk_index] {
                return !m_closed && next_chunk_index == m_chunks.size();
            });

            if (m_closed && next_chunk_index == m_chunks.size()) {
                m_offset_inside_chunk = chunk->size();
                break;
            }

            ++m_chunk_index;
            m_offset_inside_chunk = 0;
        } else {
            m_offset_inside_chunk += copied_bytes;
        }
    }

    return bytes.trim(read_bytes);
}

ErrorOr<size_t> ImageDecoderStream::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> ImageDecoderStream::seek(i64 offset, SeekMode seek_mode)
{
    Threading::MutexLocker locker(m_mutex);

    switch (seek_mode) {
    case SeekMode::SetPosition: {
        size_t remaining_bytes_to_seek = offset;
        size_t new_chunk_index = 0;

        if (remaining_bytes_to_seek == 0) {
            m_chunk_index = 0;
            m_offset_inside_chunk = 0;
            return 0;
        }

        while (remaining_bytes_to_seek > 0) {
            m_waiting_for_more_data.wait_while([this, new_chunk_index] {
                return !m_closed && new_chunk_index >= m_chunks.size();
            });

            if (m_closed && new_chunk_index >= m_chunks.size())
                return Error::from_string_literal("Offset past the end of the stream memory");

            auto const* chunk = m_chunks.find(new_chunk_index);
            if (remaining_bytes_to_seek <= chunk->size()) {
                m_chunk_index = new_chunk_index;
                m_offset_inside_chunk = remaining_bytes_to_seek;
                remaining_bytes_to_seek = 0;
            } else {
                ++new_chunk_index;
                remaining_bytes_to_seek -= chunk->size();
            }
        }

        return offset;
    }
    case SeekMode::FromCurrentPosition: {
        size_t current_offset = 0;
        for (size_t chunk_index = 0; chunk_index < m_chunk_index; ++chunk_index)
            current_offset += m_chunks.find(chunk_index)->size();

        current_offset += m_offset_inside_chunk;

        if (offset == 0)
            return current_offset;

        size_t target_offset = current_offset + offset;
        size_t new_chunk_index = m_chunk_index;
        size_t new_offset_inside_chunk = m_offset_inside_chunk;

        if (target_offset > current_offset) {
            size_t remaining_bytes_to_seek = target_offset - current_offset;

            while (remaining_bytes_to_seek > 0) {
                m_waiting_for_more_data.wait_while([this, new_chunk_index] {
                    return !m_closed && new_chunk_index >= m_chunks.size();
                });

                if (m_closed && new_chunk_index >= m_chunks.size())
                    return Error::from_string_literal("Offset past the end of the stream memory");

                auto const chunk = m_chunks.find(new_chunk_index)->span().slice(new_offset_inside_chunk);
                if (remaining_bytes_to_seek <= chunk.size()) {
                    new_offset_inside_chunk += remaining_bytes_to_seek;
                    remaining_bytes_to_seek = 0;
                } else {
                    ++new_chunk_index;
                    new_offset_inside_chunk = 0;
                    remaining_bytes_to_seek -= chunk.size();
                }
            }
        } else {
            size_t remaining_bytes_to_seek = current_offset - target_offset;

            // NOTE: This is going backwards into chunks we already have, so we don't have to perform anything related to
            //       waiting for chunks.
            while (remaining_bytes_to_seek > 0) {
                size_t bytes_to_go_back = min(new_offset_inside_chunk, remaining_bytes_to_seek);
                new_offset_inside_chunk -= bytes_to_go_back;
                remaining_bytes_to_seek -= bytes_to_go_back;

                if (remaining_bytes_to_seek > 0) {
                    if (new_chunk_index == 0)
                        return Error::from_string_literal("Offset before the beginning of the stream memory");

                    --new_chunk_index;
                    new_offset_inside_chunk = m_chunks.find(new_chunk_index)->size();
                }
            }
        }

        m_chunk_index = new_chunk_index;
        m_offset_inside_chunk = new_offset_inside_chunk;
        return target_offset;
    }
    case SeekMode::FromEndPosition:
        if (!m_closed)
            return Error::from_errno(EAGAIN);

        size_t total_bytes = 0;
        for (auto& chunk : m_chunks)
            total_bytes += chunk.size();

        if (offset == 0)
            return total_bytes;

        if (offset > 0)
            return Error::from_string_literal("Offset past the end of the stream memory");

        size_t target_offset = total_bytes + offset;
        size_t remaining_bytes_to_seek = total_bytes - target_offset;
        size_t new_chunk_index = m_chunks.size() - 1;
        size_t new_offset_inside_chunk = m_chunks.find(new_chunk_index)->size();

        // NOTE: This is going backwards into chunks we already have, so we don't have to perform anything related to
        //       waiting for chunks.
        while (remaining_bytes_to_seek > 0) {
            size_t bytes_to_go_back = min(new_offset_inside_chunk, remaining_bytes_to_seek);
            new_offset_inside_chunk -= bytes_to_go_back;
            remaining_bytes_to_seek -= bytes_to_go_back;

            if (remaining_bytes_to_seek > 0) {
                if (new_chunk_index == 0)
                    return Error::from_string_literal("Offset before the beginning of the stream memory");

                --new_chunk_index;
                new_offset_inside_chunk = m_chunks.find(new_chunk_index)->size();
            }
        }

        return target_offset;
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<void> ImageDecoderStream::truncate(size_t)
{
    return Error::from_errno(EBADF);
}

bool ImageDecoderStream::is_eof() const
{
    Threading::MutexLocker locker(m_mutex);

    if (!m_closed)
        return false;

    if (m_chunks.is_empty())
        return true;

    return m_chunk_index == m_chunks.size() - 1
        && m_offset_inside_chunk == m_chunks.find(m_chunk_index)->size();
}

bool ImageDecoderStream::is_open() const
{
    Threading::MutexLocker locker(m_mutex);
    return !m_closed;
}

void ImageDecoderStream::close()
{
    Threading::MutexLocker locker(m_mutex);
    m_closed = true;
    m_waiting_for_more_data.broadcast();
}

void ImageDecoderStream::append_chunk(ByteBuffer&& chunk)
{
    Threading::MutexLocker locker(m_mutex);

    if (m_closed)
        return;

    size_t index = m_chunks.size();
    m_chunks.insert(index, move(chunk));
    m_waiting_for_more_data.broadcast();
}

}
