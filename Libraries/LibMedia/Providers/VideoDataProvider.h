/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibGfx/Bitmap.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/TimedImage.h>
#include <LibMedia/Track.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Media {

// Retrieves coded data from a demuxer and decodes it asynchronously into video frames ready for display.
class MEDIA_API VideoDataProvider final : public AtomicRefCounted<VideoDataProvider> {
    class ThreadData;

public:
    static constexpr size_t QUEUE_CAPACITY = 8;
    using ImageQueue = Queue<TimedImage, QUEUE_CAPACITY>;

    using ErrorHandler = Function<void(DecoderError&&)>;

    static DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> try_create(NonnullRefPtr<MutexedDemuxer> const&, Track const&, RefPtr<MediaTimeProvider> const& = nullptr);
    static DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> try_create(NonnullRefPtr<Demuxer> const&, Track const&, RefPtr<MediaTimeProvider> const& = nullptr);

    VideoDataProvider(NonnullRefPtr<ThreadData> const&);
    ~VideoDataProvider();

    void set_error_handler(ErrorHandler&&);

    TimedImage retrieve_frame();

    void seek(AK::Duration timestamp);

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(NonnullRefPtr<MutexedDemuxer> const&, Track const&, NonnullOwnPtr<VideoDecoder>&&, RefPtr<MediaTimeProvider> const&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);

        void exit();

        ImageQueue& queue();
        TimedImage take_frame();

        void seek(AK::Duration timestamp);

        bool should_thread_exit() const;
        static void set_cicp_values(VideoFrame&, CodedFrame const&);
        void queue_frame(TimedImage&&);
        void push_data_and_decode_some_frames();

        [[nodiscard]] Threading::MutexLocker take_lock() { return Threading::MutexLocker(m_mutex); }
        void wake() { m_wait_condition.broadcast(); }

    private:
        Core::EventLoop& m_main_thread_event_loop;

        Threading::Mutex m_mutex;
        Threading::ConditionVariable m_wait_condition { m_mutex };
        Atomic<bool> m_exit { false };

        NonnullRefPtr<MutexedDemuxer> m_demuxer;
        Track m_track;
        NonnullOwnPtr<VideoDecoder> m_decoder;

        RefPtr<MediaTimeProvider> m_time_provider;

        size_t m_queue_max_size { 4 };
        ImageQueue m_queue;
        ErrorHandler m_error_handler;
        bool m_is_in_error_state { false };
    };

    NonnullRefPtr<ThreadData> m_thread_data;
};

}
