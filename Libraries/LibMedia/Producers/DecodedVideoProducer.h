/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/SeekMode.h>
#include <LibMedia/TimeRanges.h>
#include <LibMedia/Track.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>

namespace Media {

// Retrieves coded data from a demuxer and decodes it asynchronously into video frames ready for display.
class MEDIA_API DecodedVideoProducer : public VideoProducer {
    class ThreadData;

public:
    static constexpr size_t QUEUE_CAPACITY = 8;
    using FrameQueue = Queue<NonnullRefPtr<VideoFrame>, QUEUE_CAPACITY>;

    using ErrorHandler = Function<void(DecoderError&&)>;
    using FrameEndTimeHandler = Function<void(AK::Duration)>;

    static DecoderErrorOr<NonnullRefPtr<DecodedVideoProducer>> try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, RefPtr<MediaTimeProvider> const& = nullptr);

    DecodedVideoProducer(NonnullRefPtr<ThreadData> const&);
    ~DecodedVideoProducer();

    void set_error_handler(ErrorHandler&&);
    void set_duration_change_handler(FrameEndTimeHandler&&);

    void start();
    void suspend();
    void resume();

    virtual PipelineStatus pull(RefPtr<VideoFrame>& into) override;
    virtual void set_state_changed_handler(PipelineStateChangeHandler) override;

    virtual void seek(AK::Duration timestamp) override;

    TimeRanges buffered_time_ranges() const;

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, AK::Duration, RefPtr<MediaTimeProvider> const&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);
        void set_duration_change_handler(FrameEndTimeHandler&&);
        void set_state_changed_handler(PipelineStateChangeHandler);

        void start();
        DecoderErrorOr<void> create_decoder();
        void suspend();
        void resume();
        void exit();

        FrameQueue& queue();

        PipelineStatus pull(RefPtr<VideoFrame>& into);

        void seek(AK::Duration timestamp);

        void wait_for_start();
        bool should_thread_exit_while_locked() const;
        bool should_thread_exit() const;
        bool handle_suspension();
        template<typename Invokee>
        void invoke_on_main_thread_while_locked(Invokee);
        template<typename Invokee>
        void invoke_on_main_thread(Invokee);
        void dispatch_frame_end_time(CodedFrame const&);
        void queue_frame(NonnullRefPtr<VideoFrame> const&);
        void dispatch_error(DecoderError&&);
        bool handle_seek();
        void resolve_seek(u32 seek_id);
        void push_data_and_decode_some_frames();

        void enter_halting_state(PipelineStatus, Optional<DecoderError>);

        void dispatch_state_if_changed_while_locked(PipelineStatus);

        TimeRanges buffered_time_ranges() const;

        [[nodiscard]] Sync::MutexLocker<Sync::Mutex> take_lock() const { return Sync::MutexLocker(m_mutex); }
        void wake() const { m_wait_condition.broadcast(); }

    private:
        enum class RequestedState : u8 {
            None,
            Running,
            Suspended,
            Exit,
        };

        NonnullRefPtr<Core::WeakEventLoopReference> m_main_thread_event_loop;

        mutable Sync::Mutex m_mutex;
        mutable Sync::ConditionVariable m_wait_condition { m_mutex };
        RequestedState m_requested_state { RequestedState::None };

        NonnullRefPtr<Demuxer> m_demuxer;
        Track m_track;
        AK::Duration m_duration;
        OwnPtr<VideoDecoder> m_decoder;
        bool m_decoder_needs_keyframe_next_seek { false };

        RefPtr<MediaTimeProvider> m_time_provider;

        size_t m_queue_max_size { 4 };
        FrameQueue m_queue;
        FrameEndTimeHandler m_duration_change_handler;
        ErrorHandler m_error_handler;
        PipelineStatus m_pending_halting_status { PipelineStatus::Pending };

        u32 m_last_processed_seek_id { 0 };
        Atomic<u32> m_seek_id { 0 };
        AK::Duration m_seek_timestamp;

        PipelineStateChangeHandler m_state_changed_handler;
        PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };
    };

    NonnullRefPtr<ThreadData> m_thread_data;
};

}
