/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>

namespace Media {

// Pulls an AudioProducer on a processing thread and exposes its output through a bounded queue.
// Consumers drain the queue from their real-time callback without running the media processors there.
class MEDIA_API AudioOutputQueue final : public AtomicRefCounted<AudioOutputQueue> {
public:
    static ErrorOr<NonnullRefPtr<AudioOutputQueue>> try_create(PipelineStateChangeHandler);
    ~AudioOutputQueue();

    void stop();

    ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&);
    void disconnect_input(NonnullRefPtr<AudioProducer> const&);

    ErrorOr<void> set_sample_specification(Audio::SampleSpecification);
    Audio::SampleSpecification sample_specification() const;

    void start();
    void seek(AK::Duration);
    void set_playback_rate(float);
    float playback_rate() const;

    struct InterleavedSamples {
        ReadonlySpan<float> samples;
        u8 channel_count { 0 };
        bool contains_non_silent_samples { false };
    };
    // A pull consumer supplies how far its renderer runs ahead so periodic clock refreshes can publish the frame being
    // heard.
    struct OutputLatency {
        i64 in_frames { 0 };
    };
    InterleavedSamples read_interleaved(Span<float>, Optional<OutputLatency>);

    MediaTimeReader time_reader() const { return m_time_reader; }
    void refresh_audio_clock_anchor(MonotonicTime, i64 output_frame_index, bool playing);
    void publish_read_clock_anchor(bool playing);
    void publish_monotonic_clock_anchor(AK::Duration media_time, float playback_rate, bool playing);

    void set_state_change_handler(PipelineStateChangeHandler);
    void set_data_available_handler(Function<void()>);

private:
    static constexpr size_t BLOCK_QUEUE_CAPACITY = 4;

    AudioOutputQueue(MediaTimeWriter, MediaTimeReader, PipelineStateChangeHandler);

    void run_processing_loop();
    void disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const&);
    void discard_queued_blocks_while_locked();
    void dispatch_state_if_changed(PipelineStatus, u32 seek_id);
    i64 heard_frame_index_while_locked() const;
    AudioBlockTimingRing& block_timings() { return m_time_writer.timing_ring(); }

    Core::EventLoop& m_main_thread_event_loop;
    MediaTimeWriter m_time_writer;
    MediaTimeReader m_time_reader;

    mutable Sync::Mutex m_mutex;
    mutable Sync::ConditionVariable m_condition { m_mutex };
    RefPtr<AudioProducer> m_input;
    Audio::SampleSpecification m_sample_specification;

    Array<AudioBlock, BLOCK_QUEUE_CAPACITY> m_blocks;
    size_t m_block_head { 0 };
    size_t m_block_tail { 0 };
    size_t m_block_count { 0 };
    i64 m_next_frame_to_play { 0 };
    i64 m_seek_target_in_frames { 0 };
    i64 m_last_real_data_end_in_frames { 0 };
    float m_playback_rate { 1.0f };
    float m_eos_media_frame_remainder { 0.0f };

    i64 m_output_latency_in_frames { 0 };

    PipelineStateChangeHandler m_on_state_changed;
    Function<void()> m_on_data_available;
    PipelineStatus m_last_pull_status { PipelineStatus::Pending };
    PipelineStatus m_last_dispatched_status { PipelineStatus::Pending };

    Atomic<u32> m_seek_id { 0 };
    bool m_started { false };
    bool m_should_exit { false };
    bool m_waiting_for_upstream_data { false };
};

}
