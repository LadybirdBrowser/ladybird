/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/AudioConverter.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibMedia/TimeRanges.h>
#include <LibMedia/Track.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Forward.h>

namespace Media {

// Retrieves coded data from a demuxer and decodes it asynchronously into audio samples to push to an AudioSink.
class MEDIA_API DecodedAudioProducer final : public AudioProducer {
    class ThreadData;

public:
    static constexpr size_t QUEUE_CAPACITY = 16;
    using AudioQueue = Queue<AudioBlock, QUEUE_CAPACITY>;

    using ErrorHandler = Function<void(DecoderError&&)>;
    using BlockEndTimeHandler = Function<void(AK::Duration)>;

    static DecoderErrorOr<NonnullRefPtr<DecodedAudioProducer>> try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track);
    DecodedAudioProducer(NonnullRefPtr<ThreadData> const&);
    ~DecodedAudioProducer();

    void set_error_handler(ErrorHandler&&);
    void set_duration_change_handler(BlockEndTimeHandler&&);
    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;

    virtual void start() override;
    void suspend();
    void resume();

    virtual PipelineStatus pull(AudioBlock& into) override;
    virtual void set_state_changed_handler(PipelineStateChangeHandler) override;

    virtual void seek(AK::Duration timestamp) override;

    TimeRanges buffered_time_ranges() const;

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, AK::Duration, NonnullOwnPtr<Audio::AudioConverter>&&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);
        void set_duration_change_handler(BlockEndTimeHandler&&);
        ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification);
        void set_state_changed_handler(PipelineStateChangeHandler);

        void start();
        DecoderErrorOr<void> create_decoder();
        void suspend();
        void resume();
        void exit();

        void wait_for_start();
        bool should_thread_exit_while_locked() const;
        bool should_thread_exit() const;
        bool handle_suspension();
        template<typename Invokee>
        void invoke_on_main_thread_while_locked(Invokee);
        template<typename Invokee>
        void invoke_on_main_thread(Invokee);
        void dispatch_block_end_time(AudioBlock const&);
        void queue_block(AudioBlock&&);
        void dispatch_error(DecoderError&&);
        void flush_decoder();
        DecoderErrorOr<void> retrieve_next_block(AudioBlock&);
        bool handle_seek();
        void resolve_seek(u32 seek_id);
        void push_data_and_decode_a_block();

        PipelineStatus pull(AudioBlock& into);

        TimeRanges buffered_time_ranges() const;

        void seek(AK::Duration timestamp);

        [[nodiscard]] Sync::MutexLocker<Sync::Mutex> take_lock() const { return Sync::MutexLocker(m_mutex); }
        void wake() const { m_wait_condition.broadcast(); }

        AudioDecoder const& decoder() const { return *m_decoder; }
        AudioQueue& queue() { return m_queue; }
        void clear_queue();

        void dispatch_state_if_changed_while_locked(PipelineStatus);

        void enter_halting_state(PipelineStatus, Optional<DecoderError>);

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
        OwnPtr<AudioDecoder> m_decoder;
        bool m_decoder_needs_keyframe_next_seek { false };
        NonnullOwnPtr<Audio::AudioConverter> m_converter;
        i64 m_last_output_frame { NumericLimits<i64>::min() };

        size_t m_queue_max_size { 8 };
        AudioQueue m_queue;
        BlockEndTimeHandler m_duration_change_handler;
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
