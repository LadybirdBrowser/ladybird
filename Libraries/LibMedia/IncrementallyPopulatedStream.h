/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Media {

class IncrementallyPopulatedStream : public AtomicRefCounted<IncrementallyPopulatedStream> {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create_empty()
    {
        return adopt_ref(*new IncrementallyPopulatedStream({}));
    }

    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_byte_buffer(ByteBuffer&& data)
    {
        return adopt_ref(*new IncrementallyPopulatedStream(move(data)));
    }

    void append_chunk(ByteBuffer&& data)
    {
        Threading::MutexLocker locker { m_mutex };
        m_data.append(move(data));
        m_data_available.broadcast();
    }

    bool is_complete() const { return m_complete; }
    void mark_complete()
    {
        Threading::MutexLocker locker { m_mutex };
        m_complete = true;
        m_data_available.broadcast();
    }

    u64 size() const
    {
        if (m_complete)
            return m_data.size();
        VERIFY(m_full_size.has_value());
        return *m_full_size;
    }

    void set_full_size(u64 full_size)
    {
        m_full_size = full_size;
    }

    void cancel_blocking_reads()
    {
        Threading::MutexLocker locker { m_mutex };
        ++m_blocking_read_generation;
        m_data_available.broadcast();
    }

    DecoderErrorOr<u8> read_octet(size_t position)
    {
        Threading::MutexLocker locker { m_mutex };
        if (position >= m_data.size()) {
            if (m_complete)
                return DecoderError::corrupted("Tried to read past end of complete stream"sv);
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Not enough data in stream"sv);
        }
        return m_data[position];
    }

    DecoderErrorOr<ByteBuffer> read_octets(size_t position, size_t count)
    {
        Threading::MutexLocker locker { m_mutex };
        if (position + count > m_data.size()) {
            if (m_complete)
                return DecoderError::corrupted("Tried to read past end of complete stream"sv);
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Not enough data in stream"sv);
        }
        return MUST(m_data.slice(position, count));
    }

    DecoderErrorOr<ByteBuffer> read_octets_blocking(size_t position, size_t count)
    {
        Threading::MutexLocker locker { m_mutex };
        auto unblock_generation = m_blocking_read_generation;
        while (position + count > m_data.size() && !m_complete && unblock_generation == m_blocking_read_generation)
            m_data_available.wait();

        if (unblock_generation != m_blocking_read_generation)
            return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "Blocking read was unblocked"sv);

        if (position + count > m_data.size()) {
            // Stream is complete but still not enough data.
            return DecoderError::corrupted("Tried to read past end of complete stream"sv);
        }
        return MUST(m_data.slice(position, count));
    }

    class Seekable : public AtomicRefCounted<Seekable> {
    public:
        Seekable(NonnullRefPtr<IncrementallyPopulatedStream> stream, bool blocked_read)
            : m_stream(move(stream))
            , m_should_block_for_read(blocked_read)
        {
        }

        void seek(size_t position)
        {
            m_position = position;
        }

        DecoderErrorOr<ByteBuffer> read_bytes(size_t count)
        {
            auto buffer = TRY([&] {
                if (m_should_block_for_read)
                    return m_stream->read_octets_blocking(m_position, count);
                return m_stream->read_octets(m_position, count);
            }());
            m_position += count;
            return buffer;
        }

        auto size() const { return m_stream->size(); }

        void set_blocking_read(bool blocking) { m_should_block_for_read = blocking; }

    private:
        NonnullRefPtr<IncrementallyPopulatedStream> m_stream;
        size_t m_position { 0 };
        bool m_should_block_for_read { false };
    };

    auto create_seekable(bool blocked_read)
    {
        return adopt_ref(*new Seekable(NonnullRefPtr { *this }, blocked_read));
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
    Optional<u64> m_full_size;
    size_t m_blocking_read_generation { 0 };
};

}
