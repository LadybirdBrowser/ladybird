/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_byte_buffer(ByteBuffer&&);

    void append_chunk(ByteBuffer&& data);

    bool is_complete() const { return m_complete; }
    void mark_complete();

    auto buffered_size() const { return m_data.size(); }

    u64 total_size() const;
    void set_total_size(u64 size) { m_total_size = size; }

    void abort_blocking_reads();
    bool has_pending_blocking_reads();

    DecoderErrorOr<size_t> read_bytes_at_position_blocking(size_t position, Bytes&);

    class Seekable : public AtomicRefCounted<Seekable> {
    public:
        Seekable(NonnullRefPtr<IncrementallyPopulatedStream> stream)
            : m_stream(move(stream))
        {
        }

        enum class SeekMode : u8 {
            SetPosition,
            FromCurrentPosition,
            FromEndPosition,
        };

        DecoderErrorOr<void> seek(size_t position, SeekMode mode);
        DecoderErrorOr<size_t> read_bytes(Bytes& bytes);

        auto position() const { return m_position; }
        auto total_size() const { return m_stream->total_size(); }

    private:
        NonnullRefPtr<IncrementallyPopulatedStream> m_stream;
        size_t m_position { 0 };
    };

    auto create_seekable()
    {
        return adopt_ref(*new Seekable(NonnullRefPtr { *this }));
    }

private:
    IncrementallyPopulatedStream(ByteBuffer data)
        : m_data(move(data))
    {
    }

    Threading::Mutex m_mutex;
    Threading::ConditionVariable m_data_available { m_mutex };
    ByteBuffer m_data;
    bool m_complete { false };
    Optional<u64> m_total_size;
    size_t m_blocking_read_generation { 0 };
    size_t m_pending_blocking_reads { 0 };
};

}
