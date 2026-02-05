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
#include <LibMedia/Audio/AudioConverter.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
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
    using BlockEndTimeHandler = Function<void(AK::Duration)>;
    using SeekCompletionHandler = Function<void()>;

    static DecoderErrorOr<NonnullRefPtr<AudioDataProvider>> try_create(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const& demuxer, Track const& track);
    AudioDataProvider(NonnullRefPtr<ThreadData> const&);
    ~AudioDataProvider();

    void set_error_handler(ErrorHandler&&);
    void set_block_end_time_handler(BlockEndTimeHandler&&);
    void set_output_sample_specification(Audio::SampleSpecification);

    void start();
    void suspend();
    void resume();

    AudioBlock retrieve_block();

    void seek(AK::Duration timestamp, SeekCompletionHandler&& = nullptr);

private:
    class ThreadData final : public AtomicRefCounted<ThreadData> {
    public:
        ThreadData(NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop, NonnullRefPtr<Demuxer> const&, Track const&, NonnullOwnPtr<Audio::AudioConverter>&&);
        ~ThreadData();

        void set_error_handler(ErrorHandler&&);
        void set_block_end_time_handler(BlockEndTimeHandler&&);
        void set_output_sample_specification(Audio::SampleSpecification);

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
        void flush_decoder();
        DecoderErrorOr<void> retrieve_next_block(AudioBlock&);
        bool handle_seek();
        template<typename Callback>
        void process_seek_on_main_thread(u32 seek_id, Callback);
        void resolve_seek(u32 seek_id);
        void push_data_and_decode_a_block();

        void seek(AK::Duration timestamp, SeekCompletionHandler&&);

        [[nodiscard]] Threading::MutexLocker take_lock() const { return Threading::MutexLocker(m_mutex); }
        void wake() const { m_wait_condition.broadcast(); }

        AudioDecoder const& decoder() const { return *m_decoder; }
        AudioQueue& queue() { return m_queue; }

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
        OwnPtr<AudioDecoder> m_decoder;
        bool m_decoder_needs_keyframe_next_seek { false };
        NonnullOwnPtr<Audio::AudioConverter> m_converter;
        i64 m_last_sample { NumericLimits<i64>::min() };

        size_t m_queue_max_size { 8 };
        AudioQueue m_queue;
        BlockEndTimeHandler m_frame_end_time_handler;
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
