/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCore/Timer.h>
#include <LibMedia/Sinks/AudioPullSink.h>

namespace Media {

// While playing, the audio-driven anchor's monotonic-to-frame extrapolation can drift from the renderer's actual
// progress. Refresh it on the main thread at the same cadence as AudioPlaybackSink instead of from the real-time
// render callback.
static constexpr int CLOCK_REFRESH_INTERVAL_MS = 500;

ErrorOr<NonnullRefPtr<AudioPullSink>> AudioPullSink::try_create(u32 sample_rate)
{
    auto output_queue = TRY(AudioOutputQueue::try_create(nullptr));
    return adopt_nonnull_ref_or_enomem(new (nothrow) AudioPullSink(sample_rate, move(output_queue)));
}

AudioPullSink::AudioPullSink(u32 sample_rate, NonnullRefPtr<AudioOutputQueue> output_queue)
    : m_sample_rate(sample_rate)
    , m_output_queue(move(output_queue))
{
    VERIFY(sample_rate > 0);
    m_clock_refresh_timer = Core::Timer::create_repeating(CLOCK_REFRESH_INTERVAL_MS, [this] {
        publish_clock_anchor();
    });
}

AudioPullSink::~AudioPullSink()
{
    m_clock_refresh_timer->stop();
    m_output_queue->stop();
}

ErrorOr<void> AudioPullSink::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    return m_output_queue->connect_input(input);
}

void AudioPullSink::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    m_output_queue->disconnect_input(input);
}

MediaTimeReader AudioPullSink::time_reader() const
{
    return m_output_queue->time_reader();
}

void AudioPullSink::resume()
{
    m_playing.store(true);
    publish_clock_anchor();
}

void AudioPullSink::pause()
{
    m_playing.store(false);
    publish_clock_anchor();
}

void AudioPullSink::seek(AK::Duration timestamp)
{
    m_output_queue->seek(timestamp);
    publish_clock_anchor();
}

void AudioPullSink::set_playback_rate(float rate)
{
    VERIFY(isfinite(rate));
    VERIFY(rate >= 0.0f);

    m_playback_rate = rate;
    m_output_queue->set_playback_rate(rate);
    publish_clock_anchor();
}

void AudioPullSink::set_state_change_handler(PipelineStateChangeHandler handler)
{
    m_output_queue->set_state_change_handler(move(handler));
}

ErrorOr<void> AudioPullSink::set_channel_map(Audio::ChannelMap channel_map)
{
    VERIFY(channel_map.is_valid());
    auto sample_specification = m_output_queue->sample_specification();
    if (sample_specification.is_valid() && channel_map == sample_specification.channel_map())
        return {};

    TRY(m_output_queue->set_sample_specification({ m_sample_rate, channel_map }));
    m_channel_count.store(channel_map.channel_count());
    resynchronize_output();
    if (!m_started) {
        m_output_queue->start();
        m_started = true;
    }
    publish_clock_anchor();
    return {};
}

void AudioPullSink::set_ticking(bool ticking)
{
    if (m_ticking == ticking)
        return;

    // Capture the latest position heard before switching away from the pull-driven clock.
    if (!ticking && m_output_queue->sample_specification().is_valid()) {
        auto playing = m_playing.load() && m_playback_rate != 0.0f;
        m_output_queue->publish_read_clock_anchor(playing);
    }
    m_ticking = ticking;

    // The clock ran on wall time while the renderer was not pulling, so the queued output is behind it.
    if (ticking)
        resynchronize_output();
    publish_clock_anchor();
}

void AudioPullSink::resynchronize_output()
{
    m_output_queue->seek(m_output_queue->time_reader().current_time());
}

// While the renderer pulls, the clock follows the frames it has heard; otherwise it runs on wall time from where the
// audio stopped.
void AudioPullSink::publish_clock_anchor()
{
    auto playing = m_playing.load() && m_playback_rate != 0.0f;
    if (m_ticking && m_output_queue->sample_specification().is_valid()) {
        if (playing && !m_clock_refresh_timer->is_active())
            m_clock_refresh_timer->start();
        else if (!playing)
            m_clock_refresh_timer->stop();
        m_output_queue->publish_read_clock_anchor(playing);
        return;
    }
    m_clock_refresh_timer->stop();
    m_output_queue->publish_monotonic_clock_anchor(m_output_queue->time_reader().current_time(), m_playback_rate, playing);
}

size_t AudioPullSink::render(Span<Span<float>> output_channels, i64 output_latency_in_frames)
{
    if (!m_playing.load())
        return 0;

    VERIFY(!output_channels.is_empty());
    auto requested_frame_count = output_channels[0].size();
    for (auto channel : output_channels)
        VERIFY(channel.size() == requested_frame_count);

    auto requested_sample_count = requested_frame_count * output_channels.size();
    VERIFY(requested_sample_count <= AudioBlock::SAMPLE_CAPACITY);
    Array<float, AudioBlock::SAMPLE_CAPACITY> interleaved_samples;
    auto read = m_output_queue->read_interleaved(interleaved_samples.span().trim(requested_sample_count), AudioOutputQueue::OutputLatency { .in_frames = output_latency_in_frames });
    if (read.channel_count != output_channels.size())
        return 0;
    auto rendered_frame_count = read.samples.size() / output_channels.size();

    auto volume = m_volume.load();
    for (size_t channel = 0; channel < output_channels.size(); ++channel) {
        for (size_t frame = 0; frame < rendered_frame_count; ++frame)
            output_channels[channel][frame] = static_cast<float>(read.samples[frame * output_channels.size() + channel] * volume);
    }

    return rendered_frame_count;
}

}
