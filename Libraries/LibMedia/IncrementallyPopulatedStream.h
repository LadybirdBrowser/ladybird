/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/RedBlackTree.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Media {

class MEDIA_API IncrementallyPopulatedStream : public MediaStream {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create_empty();
    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_data(ReadonlyBytes);
    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_buffer(ByteBuffer const&);

    ~IncrementallyPopulatedStream();

    virtual NonnullRefPtr<MediaStreamCursor> create_cursor() override;

    // Callback invoked when data at a specific offset is needed but not available.
    // The callback receives the desired offset position and is invoked on the provided event loop.
    using DataRequestCallback = Function<void(u64 offset)>;
    void set_data_request_callback(DataRequestCallback);

    void add_chunk_at(u64 offset, ReadonlyBytes);

    void close();

    u64 size();
    void set_expected_size(u64);
    Optional<u64> expected_size() const;

    class MEDIA_API Cursor : public MediaStreamCursor {
    public:
        ~Cursor();

        virtual DecoderErrorOr<void> seek(i64 offset, SeekMode mode) override;
        virtual DecoderErrorOr<size_t> read_into(Bytes bytes) override;

        virtual size_t position() const override { return m_position; }
        virtual size_t size() const override { return m_stream->size(); }

        virtual void abort() override;
        virtual void reset_abort() override { m_aborted = false; }
        virtual bool is_aborted() const override { return m_aborted; }

        virtual bool is_blocked() const override { return m_blocked; }

    private:
        friend class IncrementallyPopulatedStream;

        Cursor(NonnullRefPtr<IncrementallyPopulatedStream> const& stream);

        NonnullRefPtr<IncrementallyPopulatedStream> m_stream;
        size_t m_position { 0 };
        bool m_aborted { false };
        Atomic<bool> m_blocked { false };
        MonotonicTime m_active_timeout { MonotonicTime::now_coarse() };
    };

private:
    class DataChunk {
    public:
        DataChunk(u64 offset, ByteBuffer&& data)
            : m_offset(offset)
            , m_data(move(data))
        {
        }

        u64 offset() const { return m_offset; }
        u64 size() const { return m_data.size(); }
        u64 end() const { return offset() + size(); }
        ByteBuffer& data() { return m_data; }
        ByteBuffer const& data() const { return m_data; }
        bool contains(u64 position) const { return position >= m_offset && position < end(); }
        bool overlaps(DataChunk const& chunk) const { return offset() < chunk.end() && chunk.offset() < end(); }

    private:
        size_t m_offset { 0 };
        ByteBuffer m_data;
    };

    IncrementallyPopulatedStream();

    friend class Cursor;

    using Chunks = AK::RedBlackTree<u64, DataChunk>;

    DecoderErrorOr<size_t> read_at(Cursor&, size_t position, Bytes&);

    void begin_new_request_while_locked(u64 position);
    bool check_if_data_is_available_or_begin_request_while_locked(MonotonicTime now, u64 position, u64 length);
    size_t read_from_chunks_while_locked(u64 position, Bytes& bytes) const;

    mutable Threading::Mutex m_mutex;
    Vector<Cursor&> m_cursors;
    Threading::ConditionVariable m_state_changed { m_mutex };

    Chunks m_chunks;
    Optional<u64> m_expected_size;
    bool m_closed { false };

    RefPtr<Core::WeakEventLoopReference> m_callback_event_loop;
    DataRequestCallback m_data_request_callback;
    u64 m_currently_requested_position { 0 };
    u64 m_last_chunk_end { 0 };
};

}
