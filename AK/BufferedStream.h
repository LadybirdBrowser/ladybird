/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CircularBuffer.h>
#include <AK/OwnPtr.h>
#include <AK/Stream.h>

namespace AK {

template<typename T>
concept StreamLike = IsBaseOf<Stream, T>;
template<typename T>
concept SeekableStreamLike = IsBaseOf<SeekableStream, T>;

template<typename T>
class BufferedHelper {
    AK_MAKE_NONCOPYABLE(BufferedHelper);
    AK_MAKE_DEFAULT_MOVABLE(BufferedHelper);

public:
    template<StreamLike U>
    BufferedHelper(Badge<U>, NonnullOwnPtr<T> stream, CircularBuffer buffer)
        : m_stream(move(stream))
        , m_buffer(move(buffer))
    {
    }

    template<template<typename> typename BufferedType>
    static ErrorOr<NonnullOwnPtr<BufferedType<T>>> create_buffered(NonnullOwnPtr<T> stream, size_t buffer_size)
    {
        if (!buffer_size)
            return Error::from_errno(EINVAL);
        if (!stream->is_open())
            return Error::from_errno(ENOTCONN);

        auto buffer = TRY(CircularBuffer::create_empty(buffer_size));

        return adopt_nonnull_own_or_enomem(new BufferedType<T>(move(stream), move(buffer)));
    }

    T& stream() { return *m_stream; }
    T const& stream() const { return *m_stream; }

    ErrorOr<Bytes> read(Bytes buffer)
    {
        if (!stream().is_open())
            return Error::from_errno(ENOTCONN);
        if (buffer.is_empty())
            return buffer;

        // Fill the internal buffer if it has run dry.
        if (m_buffer.used_space() == 0)
            TRY(populate_read_buffer());

        // Let's try to take all we can from the buffer first.
        return m_buffer.read(buffer);
    }

    // Reads into the buffer until \n is encountered.
    // The size of the Bytes object is the maximum amount of bytes that will be
    // read. Returns the bytes read as a StringView.
    ErrorOr<StringView> read_line(Bytes buffer)
    {
        return StringView { TRY(read_until(buffer, "\n"sv)) };
    }

    ErrorOr<Bytes> read_until(Bytes buffer, StringView candidate)
    {
        return read_until_any_of(buffer, Array { candidate });
    }

    template<size_t N>
    ErrorOr<Bytes> read_until_any_of(Bytes buffer, Array<StringView, N> candidates)
    {
        if (!stream().is_open())
            return Error::from_errno(ENOTCONN);

        if (buffer.is_empty())
            return buffer;

        auto const candidate = TRY(find_and_populate_until_any_of(candidates, buffer.size()));

        if (stream().is_eof()) {
            if ((candidate.has_value() && candidate->offset + candidate->size > buffer.size())
                || (!candidate.has_value() && buffer.size() < m_buffer.used_space())) {
                // Normally, reading from an EOFed stream and receiving bytes
                // would mean that the stream is no longer EOF. However, it's
                // possible with a buffered stream that the user is able to read
                // the buffer contents even when the underlying stream is EOF.
                // We already violate this invariant once by giving the user the
                // chance to read the remaining buffer contents, but if the user
                // doesn't give us a big enough buffer, then we would be
                // violating the invariant twice the next time the user attempts
                // to read, which is No Good. So let's give a descriptive error
                // to the caller about why it can't read.
                return Error::from_errno(EMSGSIZE);
            }
        }

        if (candidate.has_value()) {
            auto const read_bytes = m_buffer.read(buffer.trim(candidate->offset));
            TRY(m_buffer.discard(candidate->size));
            return read_bytes;
        }

        // If we still haven't found anything, then it's most likely the case
        // that the delimiter ends beyond the length of the caller-passed
        // buffer. Let's just fill the caller's buffer up.
        return m_buffer.read(buffer);
    }

    ErrorOr<StringView> read_line_with_resize(ByteBuffer& buffer)
    {
        return StringView { TRY(read_until_with_resize(buffer, "\n"sv)) };
    }

    ErrorOr<Bytes> read_until_with_resize(ByteBuffer& buffer, StringView candidate)
    {
        return read_until_any_of_with_resize(buffer, Array { candidate });
    }

    template<size_t N>
    ErrorOr<Bytes> read_until_any_of_with_resize(ByteBuffer& buffer, Array<StringView, N> candidates)
    {
        if (!stream().is_open())
            return Error::from_errno(ENOTCONN);

        auto candidate = TRY(find_and_populate_until_any_of(candidates));

        size_t bytes_read_to_user_buffer = 0;
        while (!candidate.has_value()) {
            if (m_buffer.used_space() == 0 && stream().is_eof()) {
                // If we read to the very end of the buffered and unbuffered data,
                // then treat the remainder as a full line (the last one), even if it
                // doesn't end in the delimiter.
                return buffer.span().trim(bytes_read_to_user_buffer);
            }

            if (buffer.size() - bytes_read_to_user_buffer < m_buffer.used_space()) {
                // Resize the user supplied buffer because it cannot fit
                // the contents of m_buffer.
                TRY(buffer.try_resize(buffer.size() + m_buffer.used_space()));
            }

            // Read bytes into the buffer starting from the offset of how many bytes have previously been read.
            bytes_read_to_user_buffer += m_buffer.read(buffer.span().slice(bytes_read_to_user_buffer)).size();
            candidate = TRY(find_and_populate_until_any_of(candidates));
        }

        // Once the candidate has been found, read the contents of m_buffer into the buffer,
        // offset by how many bytes have already been read in.
        TRY(buffer.try_resize(bytes_read_to_user_buffer + candidate->offset));
        m_buffer.read(buffer.span().slice(bytes_read_to_user_buffer));
        TRY(m_buffer.discard(candidate->size));
        return buffer.span();
    }

    struct Match {
        size_t offset {};
        size_t size {};
    };

    template<size_t N>
    ErrorOr<Optional<Match>> find_and_populate_until_any_of(Array<StringView, N> const& candidates, Optional<size_t> max_offset = {})
    {
        Optional<size_t> longest_candidate;
        for (auto& candidate : candidates) {
            if (candidate.length() >= longest_candidate.value_or(candidate.length()))
                longest_candidate = candidate.length();
        }

        // The intention here is to try to match all the possible
        // delimiter candidates and try to find the longest one we can
        // remove from the buffer after copying up to the delimiter to the
        // user buffer.

        auto const find_candidates = [this, &candidates, &longest_candidate](size_t min_offset, Optional<size_t> max_offset = {}) -> Optional<Match> {
            auto const corrected_minimum_offset = *longest_candidate > min_offset ? 0 : min_offset - *longest_candidate;
            max_offset = max_offset.value_or(m_buffer.used_space());

            Optional<size_t> longest_match;
            size_t match_size = 0;
            for (auto& candidate : candidates) {
                auto const result = m_buffer.offset_of(candidate, corrected_minimum_offset, *max_offset);

                if (result.has_value()) {
                    auto previous_match = longest_match.value_or(*result);
                    if ((previous_match < *result) || (previous_match == *result && match_size < candidate.length())) {
                        longest_match = result;
                        match_size = candidate.length();
                    }
                }
            }

            if (longest_match.has_value())
                return Match { *longest_match, match_size };

            return {};
        };

        if (auto first_find = find_candidates(0, max_offset); first_find.has_value())
            return first_find;

        auto last_size = m_buffer.used_space();

        while (m_buffer.used_space() < max_offset.value_or(m_buffer.capacity())) {
            auto const read_bytes = TRY(populate_read_buffer());
            if (read_bytes == 0)
                break;

            if (auto first_find = find_candidates(last_size, max_offset); first_find.has_value())
                return first_find;
            last_size = m_buffer.used_space();
        }

        return Optional<Match> {};
    }

    // Populates the buffer, and returns whether it is possible to read up to the given delimiter.
    ErrorOr<bool> can_read_up_to_delimiter(ReadonlyBytes delimiter)
    {
        if (stream().is_eof())
            return m_buffer.offset_of(delimiter).has_value();

        auto maybe_match = TRY(find_and_populate_until_any_of(Array { StringView { delimiter } }));
        if (maybe_match.has_value())
            return true;

        return stream().is_eof() && m_buffer.offset_of(delimiter).has_value();
    }

    bool is_eof_with_data_left_over() const
    {
        return stream().is_eof() && m_buffer.used_space() > 0;
    }

    bool is_eof() const
    {
        if (m_buffer.used_space() > 0) {
            return false;
        }

        return stream().is_eof();
    }

    size_t buffer_size() const
    {
        return m_buffer.capacity();
    }

    size_t buffered_data_size() const
    {
        return m_buffer.used_space();
    }

    void clear_buffer()
    {
        m_buffer.clear();
    }

    ErrorOr<void> discard_bytes(size_t count)
    {
        return m_buffer.discard(count);
    }

private:
    ErrorOr<size_t> populate_read_buffer()
    {
        if (m_buffer.empty_space() == 0)
            return 0;

        size_t nread = 0;

        while (true) {
            auto result = m_buffer.fill_from_stream(stream());

            if (result.is_error()) {
                if (!result.error().is_errno())
                    return result.release_error();
                if (result.error().code() == EINTR)
                    continue;
                if (result.error().code() == EAGAIN)
                    break;
                return result.release_error();
            }

            nread += result.value();
            break;
        }

        return nread;
    }

    NonnullOwnPtr<T> m_stream;
    CircularBuffer m_buffer;
};

// NOTE: A Buffered which accepts any Stream could be added here, but it is not
//       needed at the moment.

template<SeekableStreamLike T>
class InputBufferedSeekable final : public SeekableStream {
    friend BufferedHelper<T>;

public:
    static ErrorOr<NonnullOwnPtr<InputBufferedSeekable<T>>> create(NonnullOwnPtr<T> stream, size_t buffer_size = 16384)
    {
        return BufferedHelper<T>::template create_buffered<InputBufferedSeekable>(move(stream), buffer_size);
    }

    InputBufferedSeekable(InputBufferedSeekable&& other) = default;
    InputBufferedSeekable& operator=(InputBufferedSeekable&& other) = default;

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override { return m_helper.read(buffer); }
    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override { return m_helper.stream().write_some(buffer); }
    virtual bool is_eof() const override { return m_helper.is_eof(); }
    virtual bool is_open() const override { return m_helper.stream().is_open(); }
    virtual void close() override { m_helper.stream().close(); }
    virtual ErrorOr<size_t> seek(i64 offset, SeekMode mode) override
    {
        if (mode == SeekMode::FromCurrentPosition) {
            // If possible, seek using the buffer alone.
            if (0 <= offset && static_cast<u64>(offset) <= m_helper.buffered_data_size()) {
                MUST(m_helper.discard_bytes(offset));
                return TRY(m_helper.stream().tell()) - m_helper.buffered_data_size();
            }

            offset = offset - m_helper.buffered_data_size();
        }

        auto result = TRY(m_helper.stream().seek(offset, mode));
        m_helper.clear_buffer();

        return result;
    }
    virtual ErrorOr<void> truncate(size_t length) override
    {
        return m_helper.stream().truncate(length);
    }

    ErrorOr<StringView> read_line(Bytes buffer) { return m_helper.read_line(buffer); }
    ErrorOr<bool> can_read_line()
    {
        return TRY(m_helper.can_read_up_to_delimiter("\n"sv.bytes())) || m_helper.is_eof_with_data_left_over();
    }
    ErrorOr<Bytes> read_until(Bytes buffer, StringView candidate) { return m_helper.read_until(buffer, candidate); }
    template<size_t N>
    ErrorOr<Bytes> read_until_any_of(Bytes buffer, Array<StringView, N> candidates) { return m_helper.read_until_any_of(buffer, move(candidates)); }
    ErrorOr<bool> can_read_up_to_delimiter(ReadonlyBytes delimiter) { return m_helper.can_read_up_to_delimiter(delimiter); }

    // Methods for reading stream into an auto-adjusting buffer
    ErrorOr<StringView> read_line_with_resize(ByteBuffer& buffer) { return m_helper.read_line_with_resize(buffer); }
    ErrorOr<Bytes> read_until_with_resize(ByteBuffer& buffer, StringView candidate) { return m_helper.read_until_with_resize(buffer, candidate); }
    template<size_t N>
    ErrorOr<Bytes> read_until_any_of_with_resize(ByteBuffer& buffer, Array<StringView, N> candidates) { return m_helper.read_until_any_of_with_resize(buffer, move(candidates)); }

    size_t buffer_size() const { return m_helper.buffer_size(); }

    virtual ~InputBufferedSeekable() override = default;

private:
    InputBufferedSeekable(NonnullOwnPtr<T> stream, CircularBuffer buffer)
        : m_helper(Badge<InputBufferedSeekable<T>> {}, move(stream), move(buffer))
    {
    }

    BufferedHelper<T> m_helper;
};

template<SeekableStreamLike T>
class OutputBufferedSeekable : public SeekableStream {
public:
    static ErrorOr<NonnullOwnPtr<OutputBufferedSeekable<T>>> create(NonnullOwnPtr<T> stream, size_t buffer_size = 16 * KiB)
    {
        if (buffer_size == 0)
            return Error::from_errno(EINVAL);
        if (!stream->is_open())
            return Error::from_errno(ENOTCONN);

        auto buffer = TRY(CircularBuffer::create_empty(buffer_size));

        return adopt_nonnull_own_or_enomem(new OutputBufferedSeekable<T>(move(stream), move(buffer)));
    }

    OutputBufferedSeekable(OutputBufferedSeekable&& other) = default;
    OutputBufferedSeekable& operator=(OutputBufferedSeekable&& other) = default;

    virtual ErrorOr<Bytes> read_some(Bytes buffer) override
    {
        TRY(flush_buffer());
        return m_stream->read_some(buffer);
    }

    virtual ErrorOr<size_t> write_some(ReadonlyBytes buffer) override
    {
        if (!m_stream->is_open())
            return Error::from_errno(ENOTCONN);

        auto const written = m_buffer.write(buffer);

        if (m_buffer.empty_space() == 0)
            TRY(m_buffer.flush_to_stream(*m_stream));

        return written;
    }

    virtual bool is_eof() const override
    {
        MUST(flush_buffer());
        return m_stream->is_eof();
    }

    virtual bool is_open() const override { return m_stream->is_open(); }

    virtual void close() override
    {
        MUST(flush_buffer());
        m_stream->close();
    }

    ErrorOr<void> flush_buffer() const
    {
        while (m_buffer.used_space() > 0)
            TRY(m_buffer.flush_to_stream(*m_stream));
        return {};
    }

    // Since tell() doesn't involve moving the write offset, we can skip flushing the buffer here.
    virtual ErrorOr<size_t> tell() const override
    {
        return TRY(m_stream->tell()) + m_buffer.used_space();
    }

    virtual ErrorOr<size_t> seek(i64 offset, SeekMode mode) override
    {
        TRY(flush_buffer());
        return m_stream->seek(offset, mode);
    }

    virtual ErrorOr<void> truncate(size_t length) override
    {
        TRY(flush_buffer());
        return m_stream->truncate(length);
    }

    virtual ~OutputBufferedSeekable() override
    {
        MUST(flush_buffer());
    }

private:
    OutputBufferedSeekable(NonnullOwnPtr<T> stream, CircularBuffer buffer)
        : m_stream(move(stream))
        , m_buffer(move(buffer))
    {
    }

    mutable NonnullOwnPtr<T> m_stream;
    mutable CircularBuffer m_buffer;
};

}

#if USING_AK_GLOBALLY
using AK::BufferedHelper;
using AK::InputBufferedSeekable;
using AK::OutputBufferedSeekable;
#endif
