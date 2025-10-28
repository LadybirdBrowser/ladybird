/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Track.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Forward.h>
#include <LibThreading/Mutex.h>

namespace Media {

// Retrieves coded data from a demuxer and decodes it asynchronously into audio samples to push to an AudioSink.
class MEDIA_API AudioDataProvider : public AtomicRefCounted<AudioDataProvider> {
    class ThreadData;

public:
    static constexpr size_t QUEUE_CAPACITY = 16;
    using AudioQueue = Queue<AudioBlock, QUEUE_CAPACITY>;

    using ErrorHandler = Function<void(DecoderError&&)>;
    using SeekCompletionHandler = Function<void()>;

    static DecoderErrorOr<NonnullRefPtr<AudioDataProvider>> try_create(NonnullRefPtr<MutexedDemuxer> const& demuxer, Track const& track);
    AudioDataProvider(NonnullRefPtr<ThreadData> const&);
    ~AudioDataProvider();

    void set_error_handler(ErrorHandler&&);

    AudioBlock retrieve_block();

    void seek(AK::Duration timestamp, SeekCompletionHandler&& = nullptr);

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(NonnullRefPtr<MutexedDemuxer> const&, Track const&, NonnullOwnPtr<AudioDecoder>&&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);

        bool should_thread_exit() const;
        bool handle_seek();
        template<typename T>
        void process_seek_on_main_thread(u32 seek_id, T&&);
        void resolve_seek(u32 seek_id);
        void push_data_and_decode_a_block();

        void exit();
        void set_stopped(bool);
        bool is_stopped() const;
        void seek(AK::Duration timestamp, SeekCompletionHandler&&);

        [[nodiscard]] Threading::MutexLocker take_lock() { return Threading::MutexLocker(m_mutex); }
        void wake() { m_wait_condition.broadcast(); }

        AudioDecoder const& decoder() const { return *m_decoder; }
        AudioQueue& queue() { return m_queue; }

    private:
        Core::EventLoop& m_main_thread_event_loop;

        Threading::Mutex m_mutex;
        Threading::ConditionVariable m_wait_condition { m_mutex };
        Atomic<bool> m_exit { false };

        NonnullRefPtr<MutexedDemuxer> m_demuxer;
        Track m_track;
        NonnullOwnPtr<AudioDecoder> m_decoder;

        size_t m_queue_max_size { 8 };
        AudioQueue m_queue;
        ErrorHandler m_error_handler;
        bool m_is_in_error_state { false };

        u32 m_last_processed_seek_id { 0 };
        Atomic<u32> m_seek_id { 0 };
        SeekCompletionHandler m_seek_completion_handler;
        AK::Duration m_seek_timestamp;
    };

    NonnullRefPtr<ThreadData> m_thread_data;
};

}
