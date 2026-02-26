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
#include <LibCore/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/SeekMode.h>
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
    using FrameEndTimeHandler = Function<void(AK::Duration)>;
    using SeekCompletionHandler = Function<void(AK::Duration)>;
    using FramesQueueIsFullHandler = Function<void()>;

    static DecoderErrorOr<NonnullRefPtr<VideoDataProvider>> try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, RefPtr<MediaTimeProvider> const& = nullptr);

    VideoDataProvider(NonnullRefPtr<ThreadData> const&);
    ~VideoDataProvider();

    void set_error_handler(ErrorHandler&&);
    void set_frame_end_time_handler(FrameEndTimeHandler&&);
    void set_frames_queue_is_full_handler(FramesQueueIsFullHandler&&);

    void start();
    void suspend();
    void resume();

    TimedImage retrieve_frame();

    void seek(AK::Duration timestamp, SeekMode, SeekCompletionHandler&& = nullptr);

    bool is_blocked() const;

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, RefPtr<MediaTimeProvider> const&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);
        void set_frame_end_time_handler(FrameEndTimeHandler&&);
        void set_frames_queue_is_full_handler(FramesQueueIsFullHandler&&);

        void start();
        DecoderErrorOr<void> create_decoder();
        void suspend();
        void resume();
        void exit();

        ImageQueue& queue();
        TimedImage take_frame();

        void seek(AK::Duration timestamp, SeekMode, SeekCompletionHandler&&);

        void wait_for_start();
        bool should_thread_exit_while_locked() const;
        bool should_thread_exit() const;
        bool handle_suspension();
        template<typename Invokee>
        void invoke_on_main_thread_while_locked(Invokee);
        template<typename Invokee>
        void invoke_on_main_thread(Invokee);
        void dispatch_frame_end_time(CodedFrame const&);
        void queue_frame(NonnullOwnPtr<VideoFrame> const&);
        void dispatch_error(DecoderError&&);
        bool handle_seek();
        template<typename Callback>
        void process_seek_on_main_thread(u32 seek_id, Callback);
        void resolve_seek(u32 seek_id, AK::Duration const& timestamp);
        void push_data_and_decode_some_frames();
        bool is_blocked() const;

        [[nodiscard]] Threading::MutexLocker take_lock() const { return Threading::MutexLocker(m_mutex); }
        void wake() const { m_wait_condition.broadcast(); }

    private:
        enum class RequestedState : u8 {
            None,
            Running,
            Suspended,
            Exit,
        };

        NonnullRefPtr<Core::WeakEventLoopReference> m_main_thread_event_loop;

        mutable Threading::Mutex m_mutex;
        mutable Threading::ConditionVariable m_wait_condition { m_mutex };
        RequestedState m_requested_state { RequestedState::None };

        NonnullRefPtr<Demuxer> m_demuxer;
        Track m_track;
        OwnPtr<VideoDecoder> m_decoder;
        bool m_decoder_needs_keyframe_next_seek { false };

        RefPtr<MediaTimeProvider> m_time_provider;

        size_t m_queue_max_size { 4 };
        ImageQueue m_queue;
        FrameEndTimeHandler m_frame_end_time_handler;
        ErrorHandler m_error_handler;
        bool m_is_in_error_state { false };
        FramesQueueIsFullHandler m_frames_queue_is_full_handler;

        u32 m_last_processed_seek_id { 0 };
        Atomic<u32> m_seek_id { 0 };
        SeekCompletionHandler m_seek_completion_handler;
        AK::Duration m_seek_timestamp;
        SeekMode m_seek_mode { SeekMode::Accurate };
    };

    NonnullRefPtr<ThreadData> m_thread_data;
};

}
