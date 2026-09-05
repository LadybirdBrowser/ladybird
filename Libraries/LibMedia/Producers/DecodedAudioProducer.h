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
#include <AK/ThreadID.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/AudioConverter.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodedFrame.h>
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
    struct QueuedBlock {
        AudioBlock block;
    };
    using AudioQueue = Queue<QueuedBlock, QUEUE_CAPACITY>;

    using ErrorHandler = Function<void(DecoderError&&)>;

    static constexpr AK::Duration DEFAULT_AUTO_SUSPEND_IDLE_TIMEOUT = AK::Duration::from_milliseconds(5'000);

    static DecoderErrorOr<NonnullRefPtr<DecodedAudioProducer>> try_create(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track, AK::Duration auto_suspend_idle_timeout = DEFAULT_AUTO_SUSPEND_IDLE_TIMEOUT);
    DecodedAudioProducer(NonnullRefPtr<ThreadData> const&);
    ~DecodedAudioProducer();

    void set_error_handler(ErrorHandler&&);
    void set_read_blocked_change_handler(ReadBlockedChangeHandler);
    virtual ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification) override;

    virtual void start() override;

    virtual AudioProducerOutput peek() override;
    virtual void consume() override;
    virtual void set_wake_handler(PipelineWakeHandler) override;

    virtual void seek(AK::Duration timestamp) override;

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(Core::EventLoop& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, AK::Duration auto_suspend_idle_timeout, NonnullOwnPtr<Audio::AudioConverter>&&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);
        void set_read_blocked_change_handler(ReadBlockedChangeHandler);
        ErrorOr<void> set_output_sample_specification(Audio::SampleSpecification);
        void set_wake_handler(PipelineWakeHandler);

        void start();
        DecoderErrorOr<void> create_decoder_for_frame(CodedFrame const&);
        DecoderErrorOr<void> receive_coded_frame(CodedFrame const&);
        DecoderErrorOr<bool> replace_drained_decoder();
        void release_decoder();
        void exit();

        void register_decode_thread();
        void wait_for_start();
        bool should_thread_exit_while_locked() const;
        bool should_thread_exit() const;
        bool handle_auto_suspension();
        template<typename Invokee>
        void invoke_on_main_thread_while_locked(Invokee);
        void queue_block(AudioBlock const&);
        void dispatch_error(DecoderError&&);
        void flush_decoder();
        DecoderErrorOr<void> retrieve_next_block(AudioBlock&);
        bool handle_seek();
        void resolve_seek(u32 seek_id, bool moved_position);
        void push_data_and_decode_a_block();

        AudioProducerOutput peek();
        AudioProducerOutput peek_while_locked();
        void consume();

        void seek(AK::Duration timestamp);

        [[nodiscard]] Sync::MutexLocker<Sync::Mutex> take_lock() const { return Sync::MutexLocker(m_mutex); }
        void wake() const { m_wait_condition.broadcast(); }

        AudioDecoder const& decoder() const { return *m_decoder; }

        void dispatch_wake_if_needed_while_locked();

        void enter_halting_state(PipelineStatus, Optional<DecoderError>);

    private:
        enum class RequestedState : u8 {
            None,
            Running,
            Exit,
        };

        void note_consumer_activity_while_locked() const;
        void wait_for_queue_space_or_auto_suspend_while_locked();
        void dispatch_read_blocked_change(ReadBlocked blocked);

        Core::EventLoop& m_main_thread_event_loop;

        mutable Sync::Mutex m_mutex;
        mutable Sync::ConditionVariable m_wait_condition { m_mutex };
        RequestedState m_requested_state { RequestedState::None };

        AK::ThreadID m_decode_thread_id;
        NonnullRefPtr<Demuxer> m_demuxer;
        Track m_track;
        CodecID m_decoder_codec_id { CodecID::Unknown };
        Optional<CodedFrame> m_frame_awaiting_decoder_replacement;
        OwnPtr<AudioDecoder> m_decoder;
        bool m_decoder_needs_keyframe_next_seek { false };
        bool m_decoder_needs_codec_configuration_next_seek { true };
        // The converter is reconfigured by the consumer's thread while the decode thread is feeding it.
        mutable Sync::Mutex m_converter_mutex;
        NonnullOwnPtr<Audio::AudioConverter> m_converter;
        i64 m_last_output_frame { NumericLimits<i64>::min() };

        size_t m_queue_max_size { 8 };
        AudioQueue m_queue;
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
