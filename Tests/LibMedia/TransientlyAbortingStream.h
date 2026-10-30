/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/ReadonlyBytesCursor.h>

// Fails the first blocking read that starts at or after a position the way an abort would, but leaves no abort flag
// set behind it, as when the abort is reset before the reading thread could look at it.
class TransientlyAbortingStream final : public Media::MediaStream {
public:
    static NonnullRefPtr<TransientlyAbortingStream> create(ByteBuffer data, size_t aborted_read_position)
    {
        return adopt_ref(*new TransientlyAbortingStream(move(data), aborted_read_position));
    }

    virtual NonnullRefPtr<Media::MediaStreamCursor> create_cursor() override { return make_ref_counted<Cursor>(*this); }
    virtual Availability availability() const override { return { { { 0, m_data.size() } }, m_data.size(), true }; }
    virtual Optional<u64> expected_size() const override { return m_data.size(); }
    virtual bool is_closed() const override { return true; }

    bool has_aborted_a_read() const { return m_has_aborted_a_read.load(); }

private:
    class Cursor final : public Media::MediaStreamCursor {
    public:
        explicit Cursor(TransientlyAbortingStream& stream)
            : m_stream(stream)
            , m_bytes(make_ref_counted<Media::ReadonlyBytesCursor>(stream.m_data.bytes()))
        {
        }

        virtual void set_is_blocking(bool is_blocking) override { m_is_blocking = is_blocking; }
        virtual Media::DecoderErrorOr<void> seek(i64 offset, AK::SeekMode mode) override { return m_bytes->seek(offset, mode); }

        virtual Media::DecoderErrorOr<size_t> read_into(Bytes bytes) override
        {
            TRY(abort_if_reading_the_aborted_position());
            return m_bytes->read_into(bytes);
        }

        virtual Media::DecoderErrorOr<FixedArray<u8>> read_bytes(size_t size) override
        {
            TRY(abort_if_reading_the_aborted_position());
            return m_bytes->read_bytes(size);
        }

        virtual Media::DecoderErrorOr<void> read_and_discard(size_t size) override
        {
            TRY(abort_if_reading_the_aborted_position());
            return m_bytes->read_and_discard(size);
        }

        virtual size_t position() const override { return m_bytes->position(); }
        virtual Optional<u64> size() const override { return m_bytes->size(); }
        virtual size_t blocking_size() const override { return m_bytes->blocking_size(); }

    private:
        Media::DecoderErrorOr<void> abort_if_reading_the_aborted_position()
        {
            if (!m_is_blocking || position() < m_stream->m_aborted_read_position || m_stream->m_has_aborted_a_read.exchange(true))
                return {};
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Aborted, "Read aborted"sv);
        }

        NonnullRefPtr<TransientlyAbortingStream> m_stream;
        NonnullRefPtr<Media::ReadonlyBytesCursor> m_bytes;
        bool m_is_blocking { true };
    };

    TransientlyAbortingStream(ByteBuffer data, size_t aborted_read_position)
        : m_data(move(data))
        , m_aborted_read_position(aborted_read_position)
    {
    }

    ByteBuffer m_data;
    size_t m_aborted_read_position { 0 };
    Atomic<bool> m_has_aborted_a_read { false };
};
