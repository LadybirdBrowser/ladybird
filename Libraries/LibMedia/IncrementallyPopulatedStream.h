/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Media {

class MEDIA_API IncrementallyPopulatedStream : public AtomicRefCounted<IncrementallyPopulatedStream> {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create_empty();
    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_buffer(ByteBuffer&&);

    void append(ByteBuffer&&);
    void close();

    u64 size();

    class Cursor : public AtomicRefCounted<Cursor> {
    public:
        Cursor(NonnullRefPtr<IncrementallyPopulatedStream> stream)
            : m_stream(move(stream))
        {
        }

        enum class SeekMode : u8 {
            SetPosition,
            FromCurrentPosition,
            FromEndPosition,
        };

        DecoderErrorOr<void> seek(size_t position, SeekMode mode);
        DecoderErrorOr<size_t> read_into(Bytes bytes);

        auto position() const { return m_position; }
        auto size() const { return m_stream->size(); }

        void abort();
        void reset_abort() { m_aborted = false; }

    private:
        friend class IncrementallyPopulatedStream;

        NonnullRefPtr<IncrementallyPopulatedStream> m_stream;
        size_t m_position { 0 };
        bool m_aborted { false };
        Atomic<bool> m_blocked { false };
    };

    auto create_cursor()
    {
        return adopt_ref(*new Cursor(NonnullRefPtr { *this }));
    }

private:
    IncrementallyPopulatedStream(ByteBuffer buffer, bool is_complete)
        : m_buffer(move(buffer))
        , m_closed(is_complete)
    {
    }

    friend class Cursor;

    enum class AllowPositionAtEnd {
        Yes,
        No,
    };
    DecoderErrorOr<size_t> read_at(Cursor&, size_t position, Bytes&, AllowPositionAtEnd);

    Threading::Mutex m_mutex;
    Threading::ConditionVariable m_state_changed { m_mutex };
    ByteBuffer m_buffer;
    Atomic<bool> m_closed { false };
};

}
