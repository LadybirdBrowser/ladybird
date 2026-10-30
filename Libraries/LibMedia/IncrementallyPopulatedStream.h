/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/RedBlackTree.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media {

class MEDIA_API IncrementallyPopulatedStream : public MediaStream {
public:
    static NonnullRefPtr<IncrementallyPopulatedStream> create_empty();
    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_data(ReadonlyBytes);
    static NonnullRefPtr<IncrementallyPopulatedStream> create_from_buffer(ByteBuffer const&);

    ~IncrementallyPopulatedStream();

    virtual NonnullRefPtr<MediaStreamCursor> create_cursor() override;

    // Invoked to request data at a particular offset. If the stream is idle and nothing is blocked on data, this will
    // be invoked without an offset. It is invoked on the originating event loop.
    using DataRequestCallback = Function<void(Optional<u64> offset)>;
    void set_data_request_callback(DataRequestCallback);
    void set_may_idle(bool);

    void add_chunk_at(u64 offset, ReadonlyBytes);
    void remove_byte_range(u64 start, u64 end);

    void close();
    virtual bool is_closed() const override;

    // Reads that would wait for missing data fail as permanently unavailable from then on.
    void make_missing_data_permanently_unavailable();

    virtual Availability availability() const override;

    virtual void set_available_ranges_change_observer(Function<void()>) override;

    u64 size();
    void set_expected_size(u64);
    virtual Optional<u64> expected_size() const override;

    class MEDIA_API Cursor : public MediaStreamCursor {
    public:
        ~Cursor();

        virtual void set_is_blocking(bool) override;

        virtual DecoderErrorOr<void> seek(i64 offset, AK::SeekMode mode) override;
        virtual DecoderErrorOr<size_t> read_into(Bytes bytes) override;
        virtual DecoderErrorOr<FixedArray<u8>> read_bytes(size_t size) override;
        virtual DecoderErrorOr<void> read_and_discard(size_t size) override;

        virtual size_t position() const override { return m_position; }
        virtual Optional<u64> size() const override { return m_stream->expected_size(); }
        virtual size_t blocking_size() const override { return m_stream->size(); }

        virtual void abort() override;
        virtual void reset_abort() override { m_aborted.store(false, AK::MemoryOrder::memory_order_relaxed); }
        virtual bool is_aborted() const override { return m_aborted.load(AK::MemoryOrder::memory_order_relaxed); }

        virtual void set_blocked_change_handler(ReadBlockedChangeHandler) override;

    private:
        friend class IncrementallyPopulatedStream;

        Cursor(NonnullRefPtr<IncrementallyPopulatedStream> const& stream);

        NonnullRefPtr<IncrementallyPopulatedStream> m_stream;
        bool m_is_blocking { true };
        size_t m_position { 0 };
        // Set from any thread under the stream's mutex, which orders it against the blocked read; only the reading
        // thread clears it, between its reads.
        Atomic<bool> m_aborted { false };
        bool m_blocked { false };
        ReadBlockedChangeHandler m_read_blocked_change_handler;
        MonotonicTime m_active_timeout;

        // The chunk and stream state that this cursor last chose a request position for. Reads within that chunk are
        // skipped until the state changes, since the choice would be the same.
        Optional<u64> m_request_decided_chunk_offset;
        u64 m_request_decided_generation { 0 };
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

    DecoderErrorOr<void> wait_for_data_while_locked(Cursor&, u64 position, u64 length);
    DecoderErrorOr<size_t> read_at(Cursor&, size_t position, Bytes&);
    DecoderErrorOr<void> discard_at(Cursor&, size_t position, size_t length);

    bool a_cursor_is_blocked_while_locked() const;
    u64 select_request_position_while_locked(u64 position);
    void begin_new_request_while_locked(u64 position);
    void stop_request_if_idle_while_locked();
    void dispatch_data_request_while_locked(Optional<u64> position);
    bool check_if_data_is_available_or_begin_request_while_locked(Cursor&, u64 position, u64 length);
    size_t read_from_chunks_while_locked(u64 position, Bytes& bytes) const;
    void notify_available_ranges_changed_while_locked();
    void note_request_state_changed_while_locked() { m_request_state_generation++; }

    mutable Mutex m_mutex;
    Vector<Cursor&> m_cursors;
    ConditionVariable m_state_changed { m_mutex };

    Chunks m_chunks;
    Optional<u64> m_expected_size;
    bool m_closed { false };
    bool m_missing_data_is_permanently_unavailable { false };

    Function<void()> m_available_ranges_change_observer;

    RefPtr<Core::WeakEventLoopReference> m_callback_event_loop;
    DataRequestCallback m_data_request_callback;
    Optional<u64> m_currently_requested_position { 0 };
    u64 m_current_append_head { 0 };
    u64 m_last_appended_chunk_end { 0 };
    bool m_may_idle { false };
    // Changes whenever anything that the choice of request position depends on changes.
    u64 m_request_state_generation { 1 };
};

}
