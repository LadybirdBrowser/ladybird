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
#include <AK/ThreadID.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/SeekMode.h>
#include <LibMedia/TimeRanges.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFramePool.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>

namespace Media {

// Retrieves coded data from a demuxer and decodes it asynchronously into video frames ready for display.
class MEDIA_API DecodedVideoProducer : public VideoProducer {
    class ThreadData;

public:
    struct QueuedFrame {
        NonnullRefPtr<VideoFrame> frame;
    };
    using FrameQueue = Queue<QueuedFrame>;

    using ErrorHandler = Function<void(DecoderError&&)>;

    static constexpr AK::Duration DEFAULT_AUTO_SUSPEND_IDLE_TIMEOUT = AK::Duration::from_milliseconds(5'000);

    static DecoderErrorOr<NonnullRefPtr<DecodedVideoProducer>> try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, AK::Duration auto_suspend_idle_timeout = DEFAULT_AUTO_SUSPEND_IDLE_TIMEOUT);

    DecodedVideoProducer(NonnullRefPtr<ThreadData> const&);
    ~DecodedVideoProducer();

    void set_error_handler(ErrorHandler&&);
    void set_read_blocked_change_handler(ReadBlockedChangeHandler);

    virtual void start() override;

    virtual VideoProducerOutput peek() override;
    virtual void consume() override;
    virtual void set_wake_handler(PipelineWakeHandler) override;

    AK::Duration select_fast_seek_target(AK::Duration timestamp, SeekMode);
    virtual void seek(AK::Duration timestamp) override;

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, AK::Duration auto_suspend_idle_timeout);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);
        void set_read_blocked_change_handler(ReadBlockedChangeHandler);
        void set_wake_handler(PipelineWakeHandler);

        void start();
        DecoderErrorOr<void> create_decoder();
        void release_decoder();
        void exit();

        FrameQueue& queue();

        VideoProducerOutput peek();
        VideoProducerOutput peek_while_locked();
        void consume();

        void seek(AK::Duration timestamp);
        AK::Duration select_fast_seek_target(AK::Duration target, SeekMode) const;

        void register_decode_thread();
        void wait_for_start();
        bool should_thread_exit_while_locked() const;
        bool should_thread_exit() const;
        bool handle_auto_suspension();
        template<typename Invokee>
        void invoke_on_main_thread_while_locked(Invokee);
        void queue_frame(NonnullRefPtr<VideoFrame> const&);
        void dispatch_error(DecoderError&&);
        bool handle_seek();
        void resolve_seek(u32 seek_id, bool moved_position);
        DecoderErrorOr<void> ensure_frame_pool();
        DecoderErrorOr<NonnullRefPtr<VideoFrame>> take_frame_into_acquired_slot(VideoFrameMetadata const&, VideoFramePool::AcquiredSlot const&);
        void push_data_and_decode_some_frames();

        void enter_halting_state(PipelineStatus, Optional<DecoderError>);

        void dispatch_wake_if_needed_while_locked();

        [[nodiscard]] Sync::MutexLocker<Sync::Mutex> take_lock() const { return Sync::MutexLocker(m_wait_state->mutex); }
        void wake() const { m_wait_state->condition.broadcast(); }

    private:
        enum class RequestedState : u8 {
            None,
            Running,
            Exit,
        };

        void note_consumer_activity_while_locked() const;
        void wait_to_decode_or_auto_suspend_while_locked();
        void dispatch_read_blocked_change(ReadBlocked blocked);

        Core::EventLoop& m_main_thread_event_loop;

        // Shared with the frame pools' slot-freed callbacks, which can outlive this ThreadData
        // through frames still held downstream.
        struct WaitState : public AtomicRefCounted<WaitState> {
            Sync::Mutex mutex;
            Sync::ConditionVariable condition { mutex };
        };
        NonnullRefPtr<WaitState> m_wait_state { make_ref_counted<WaitState>() };
        RequestedState m_requested_state { RequestedState::None };

        AK::ThreadID m_decode_thread_id;
        NonnullRefPtr<Demuxer> m_demuxer;
        Track m_track;
        OwnPtr<VideoDecoder> m_decoder;
        RefPtr<VideoFramePool> m_frame_pool;
        bool m_decoder_needs_keyframe_next_seek { false };

        FrameQueue m_queue;
        AK::Duration m_earliest_available_timestamp;
        AK::Duration m_latest_available_timestamp;
        ErrorHandler m_error_handler;
        ReadBlockedChangeHandler m_read_blocked_change_handler;
        PipelineStatus m_current_halting_status { PipelineStatus::Pending };

        u32 m_last_processed_seek_id { 0 };
        Atomic<u32> m_seek_id { 0 };
        AK::Duration m_seek_timestamp;

        PipelineWakeHandler m_wake_handler;
        mutable bool m_downstream_needs_wake { true };

        AK::Duration const m_auto_suspend_idle_timeout;
        mutable MonotonicTime m_last_consumer_activity { MonotonicTime::now() };
        bool m_auto_suspended { false };
        mutable bool m_auto_suspend_requested { false };
    };

    NonnullRefPtr<ThreadData> m_thread_data;
};

}
