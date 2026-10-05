/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/ThreadSafeWeakable.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/AudioOutput.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaClock.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibMedia/Sinks/AudioSink.h>

namespace Media {

class MEDIA_API AudioPlaybackSink final : public AudioSink
    , public MediaClock
    , public ThreadSafeWeakable<AudioPlaybackSink> {
private:
    class OutputThreadData;

public:
    static ErrorOr<NonnullRefPtr<AudioPlaybackSink>> try_create(PipelineStateChangeHandler on_state_changed, AudioOutput = AudioOutput::Platform);
    AudioPlaybackSink(NonnullRefPtr<OutputThreadData>, MediaTimeReader, PipelineStateChangeHandler, AudioOutput);
    virtual ~AudioPlaybackSink() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    void start();

    virtual MediaTimeReader time_reader() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;
    // Replaces the queued output with data read again from the input, without moving the clock.
    void reseek_input_keeping_clock();

    virtual void set_playback_rate(float) override;

    void set_volume(double);

    Function<void(Error&&)> on_audio_output_error;

private:
    enum class StreamState {
        Suspended,
        Playing,
    };

    void create_playback_stream();
    void publish_clock_anchor(MonotonicTime now) const;
    bool effectively_paused() const;
    void update_playback_stream_state();
    void resume_playback_stream();
    void pause_playback_stream();
    void resume_input_from_suspension();
    i64 played_output_frame_index() const;
    void dispatch_waiting_status_once_played_out();

    Core::EventLoop& m_main_thread_event_loop;
    PipelineStateChangeHandler m_on_state_changed;
    AudioOutput m_audio_output { AudioOutput::Platform };

    bool m_started_creating_playback_stream { false };
    bool m_playing { false };
    StreamState m_stream_state { StreamState::Suspended };
    double m_volume { 1 };

    AK::Duration m_anchor_stream_time;
    i64 m_anchor_output_frame_index { 0 };
    Optional<AK::Duration> m_seek_target_awaiting_drain;

    NonnullRefPtr<OutputThreadData> m_output_thread_data;
    RefPtr<Core::Timer> m_clock_refresh_timer;
    RefPtr<Core::Timer> m_played_out_timer;
    MediaTimeReader m_time_reader;
};

}
