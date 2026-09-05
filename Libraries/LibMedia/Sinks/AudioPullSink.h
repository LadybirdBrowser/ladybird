/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaClock.h>
#include <LibMedia/MediaTime.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Sinks/AudioOutputQueue.h>
#include <LibMedia/Sinks/AudioSink.h>

namespace Media {

// An audio sink whose output is pulled as planar floating-point samples by an external renderer.
class MEDIA_API AudioPullSink final : public AudioSink
    , public MediaClock {
public:
    static ErrorOr<NonnullRefPtr<AudioPullSink>> try_create(u32 sample_rate);
    virtual ~AudioPullSink() override;

    virtual ErrorOr<void> connect_input(NonnullRefPtr<AudioProducer> const&) override;
    virtual void disconnect_input(NonnullRefPtr<AudioProducer> const&) override;

    virtual MediaTimeReader time_reader() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;
    virtual void set_playback_rate(float) override;

    void set_state_change_handler(PipelineStateChangeHandler);
    ErrorOr<void> set_channel_map(Audio::ChannelMap);
    u8 channel_count() const { return m_channel_count.load(); }
    void set_ticking(bool);

    void set_volume(double volume) { m_volume.store(volume); }

    // Fills as much of each equally-sized channel as is currently available. Unfilled samples are left untouched. The
    // latency is how many frames the renderer runs ahead of what is being heard.
    size_t render(Span<Span<float>> output_channels, i64 output_latency_in_frames);

private:
    AudioPullSink(u32 sample_rate, NonnullRefPtr<AudioOutputQueue>);

    void resynchronize_output();
    void publish_clock_anchor();

    u32 const m_sample_rate { 0 };
    NonnullRefPtr<AudioOutputQueue> m_output_queue;
    RefPtr<Core::Timer> m_clock_refresh_timer;

    Atomic<u8> m_channel_count { 0 };
    Atomic<double> m_volume { 1.0 };
    Atomic<bool> m_playing { false };
    float m_playback_rate { 1.0f };
    bool m_started { false };
    bool m_ticking { true };
};

}
