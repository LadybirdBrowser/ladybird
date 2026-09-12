/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/AudioOutputMonitor.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaClock.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibMedia/Sinks/AudioOutputQueue.h>
#include <LibMedia/Sinks/AudioSink.h>

namespace Media {

class MEDIA_API AudioPlaybackSink final : public AudioSink
    , public MediaClock {
public:
    static ErrorOr<NonnullRefPtr<AudioPlaybackSink>> try_create(PipelineStateChangeHandler on_state_changed);
    AudioPlaybackSink(NonnullRefPtr<AudioOutputQueue>, MediaTimeReader);
    virtual ~AudioPlaybackSink() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual MediaTimeReader time_reader() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;

    virtual void set_playback_rate(float) override;

    void set_volume(double);
    void set_muted(bool);
    void set_audio_output_state_change_handler(Function<void(bool)>);

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
    void update_volume();

    Core::EventLoop& m_main_thread_event_loop;

    bool m_started_creating_playback_stream { false };
    bool m_playing { false };
    StreamState m_stream_state { StreamState::Suspended };
    double m_volume { 1 };
    bool m_muted { false };

    AK::Duration m_anchor_stream_time;
    i64 m_anchor_output_frame_index { 0 };
    Optional<AK::Duration> m_seek_target_awaiting_drain;

    NonnullRefPtr<AudioOutputQueue> m_output_queue;
    NonnullRefPtr<Audio::AudioOutputMonitor> m_audio_output_monitor;
    RefPtr<Audio::PlaybackStream> m_playback_stream;
    RefPtr<Core::Timer> m_clock_refresh_timer;
    MediaTimeReader m_time_reader;
};

}
