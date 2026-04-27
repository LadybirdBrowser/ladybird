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
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/MediaTimeProvider.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/AudioProducer.h>
#include <LibMedia/Sinks/AudioSink.h>

namespace Media {

class MEDIA_API AudioPlaybackSink final : public AudioSink
    , public MediaTimeProvider {
private:
    class OutputThreadData;

public:
    static ErrorOr<NonnullRefPtr<AudioPlaybackSink>> try_create(PipelineStateChangeHandler on_state_changed);
    AudioPlaybackSink(NonnullRefPtr<OutputThreadData>);
    virtual ~AudioPlaybackSink() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    void disconnect_input_while_locked(NonnullRefPtr<AudioProducer> const&);
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual AK::Duration current_time() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;

    void set_volume(double);

    Function<void(Error&&)> on_audio_output_error;

private:
    void create_playback_stream();

    Core::EventLoop& m_main_thread_event_loop;

    Audio::SampleSpecification m_sample_specification;

    bool m_started_creating_playback_stream { false };
    bool m_playing { false };
    double m_volume { 1 };

    AK::Duration m_last_stream_time;
    AK::Duration m_last_media_time;
    Optional<AK::Duration> m_temporary_time;

    NonnullRefPtr<OutputThreadData> m_output_thread_data;
};

}
