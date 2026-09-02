/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/Timer.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Sinks/AudioOutputQueue.h>

#include "AudioPlaybackSink.h"

namespace Media {

// While playing, the audio-driven anchor's monotonic→frame extrapolation drifts against the
// device clock; out-of-process readers can't trigger the read-path refresh, so the writer
// re-syncs the anchor on this cadence. Coarse is fine: error is bounded by drift × interval.
static constexpr int CLOCK_REFRESH_INTERVAL_MS = 500;

ErrorOr<NonnullRefPtr<AudioPlaybackSink>> AudioPlaybackSink::try_create(PipelineStateChangeHandler on_state_changed)
{
    auto output_queue = TRY(AudioOutputQueue::try_create(move(on_state_changed)));
    return try_make_ref_counted<AudioPlaybackSink>(output_queue, output_queue->time_reader());
}

AudioPlaybackSink::AudioPlaybackSink(NonnullRefPtr<AudioOutputQueue> output_queue, MediaTimeReader time_reader)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_output_queue(move(output_queue))
    , m_audio_output_monitor(Audio::AudioOutputMonitor::create(m_main_thread_event_loop))
    , m_time_reader(move(time_reader))
{
    m_clock_refresh_timer = Core::Timer::create_repeating(CLOCK_REFRESH_INTERVAL_MS, [this] {
        publish_clock_anchor(MonotonicTime::now());
    });

    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this)] {
        self->create_playback_stream();
    });
}

AudioPlaybackSink::~AudioPlaybackSink()
{
    m_audio_output_monitor->set_enabled(false);
    m_output_queue->set_data_available_handler(nullptr);
    m_output_queue->stop();
}

ErrorOr<void> AudioPlaybackSink::connect_input(NonnullRefPtr<AudioProducer> const& input)
{
    return m_output_queue->connect_input(input);
}

void AudioPlaybackSink::disconnect_input(NonnullRefPtr<AudioProducer> const& input)
{
    m_output_queue->disconnect_input(input);
}

void AudioPlaybackSink::create_playback_stream()
{
    if (m_started_creating_playback_stream)
        return;

    m_started_creating_playback_stream = true;

    auto data_callback = [output_queue = m_output_queue, audio_output_monitor = m_audio_output_monitor](Span<float> buffer) -> ReadonlySpan<float> {
        auto output = output_queue->read_interleaved(buffer, {});
        audio_output_monitor->update(output.contains_non_silent_samples);
        return output.samples;
    };
    constexpr u32 target_latency_ms = 100;

    auto promise = Audio::PlaybackStream::create_platform_or_null(Audio::OutputState::Suspended, target_latency_ms, move(data_callback));

    promise->when_resolved([self = NonnullRefPtr(*this)](auto& stream) {
        auto sample_specification = stream->sample_specification();
        if (auto result = self->m_output_queue->set_sample_specification(sample_specification); result.is_error()) {
            if (self->on_audio_output_error)
                self->on_audio_output_error(result.release_error());
            return;
        }
        self->m_playback_stream = stream;
        self->m_output_queue->set_data_available_handler([stream = stream.ptr()] {
            stream->notify_data_available();
        });
        self->update_volume();
        self->m_output_queue->start();

        if (self->m_seek_target_awaiting_drain.has_value()) {
            self->seek(self->m_seek_target_awaiting_drain.release_value());
            return;
        }

        self->m_output_queue->seek(self->m_time_reader.current_time());

        self->update_playback_stream_state();
    });

    promise->when_rejected([self = NonnullRefPtr(*this)](auto& error) {
        if (self->on_audio_output_error)
            self->on_audio_output_error(move(error));
    });
}

MediaTimeReader AudioPlaybackSink::time_reader() const
{
    return m_time_reader;
}

void AudioPlaybackSink::publish_clock_anchor(MonotonicTime now) const
{
    auto sample_specification = m_output_queue->sample_specification();
    if (!m_playback_stream || !sample_specification.is_valid())
        return;
    if (m_seek_target_awaiting_drain.has_value())
        return;

    auto stream_time = m_playback_stream->total_time_played();
    auto stream_delta = stream_time - m_anchor_stream_time;
    auto frames_played = stream_delta.to_time_units(1, sample_specification.sample_rate());
    auto current_output_frame_index = m_anchor_output_frame_index + frames_played;

    m_output_queue->refresh_audio_clock_anchor(now, current_output_frame_index, m_stream_state == StreamState::Playing);
}

void AudioPlaybackSink::resume()
{
    m_playing = true;
    update_playback_stream_state();
}

void AudioPlaybackSink::pause()
{
    m_playing = false;
    update_playback_stream_state();
}

bool AudioPlaybackSink::effectively_paused() const
{
    if (!m_playing)
        return true;
    if (m_output_queue->playback_rate() == 0.0f)
        return true;
    if (m_seek_target_awaiting_drain.has_value())
        return true;
    return false;
}

void AudioPlaybackSink::update_playback_stream_state()
{
    if (effectively_paused()) {
        pause_playback_stream();
        return;
    }

    resume_playback_stream();
}

void AudioPlaybackSink::resume_playback_stream()
{
    if (m_stream_state == StreamState::Playing)
        return;
    if (!m_playback_stream)
        return;

    VERIFY(!m_clock_refresh_timer->is_active());
    m_stream_state = StreamState::Playing;
    m_audio_output_monitor->set_enabled(true);
    m_clock_refresh_timer->start();
    m_playback_stream->resume()
        ->when_resolved([self = NonnullRefPtr(*this)](auto new_device_time) {
            self->m_main_thread_event_loop.deferred_invoke([self, new_device_time]() {
                self->m_anchor_stream_time = new_device_time;
                self->publish_clock_anchor(MonotonicTime::now());
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while resuming AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::pause_playback_stream()
{
    if (m_stream_state == StreamState::Suspended)
        return;
    if (!m_playback_stream)
        return;

    VERIFY(m_clock_refresh_timer->is_active());
    m_stream_state = StreamState::Suspended;
    m_audio_output_monitor->set_enabled(false);
    m_clock_refresh_timer->stop();
    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                auto stream_delta = new_stream_time - self->m_anchor_stream_time;
                auto frames_played = stream_delta.to_time_units(1, self->m_output_queue->sample_specification().sample_rate());
                self->m_anchor_output_frame_index += frames_played;
                self->m_anchor_stream_time = new_stream_time;
                self->publish_clock_anchor(MonotonicTime::now());
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::seek(AK::Duration time)
{
    bool already_draining_for_seek = m_seek_target_awaiting_drain.has_value();
    m_seek_target_awaiting_drain = time;
    m_output_queue->seek(time);

    if (!m_playback_stream)
        return;

    if (already_draining_for_seek)
        return;

    m_stream_state = StreamState::Suspended;
    m_clock_refresh_timer->stop();
    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([self = NonnullRefPtr(*this)]() {
            auto new_stream_time = self->m_playback_stream->total_time_played();

            self->m_main_thread_event_loop.deferred_invoke([self, new_stream_time]() {
                self->m_anchor_stream_time = new_stream_time;
                auto seek_target = self->m_seek_target_awaiting_drain.release_value();
                self->m_anchor_output_frame_index = seek_target.to_time_units(1, self->m_output_queue->sample_specification().sample_rate());

                self->update_playback_stream_state();
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while seeking AudioPlaybackSink: {}", error.string_literal());
        });
}

void AudioPlaybackSink::set_volume(double volume)
{
    m_volume = volume;
    update_volume();
}

void AudioPlaybackSink::set_muted(bool muted)
{
    m_muted = muted;
    update_volume();
}

void AudioPlaybackSink::update_volume()
{
    auto effective_volume = m_muted ? 0.0 : m_volume;
    m_audio_output_monitor->set_muted(effective_volume == 0.0);

    if (m_playback_stream) {
        m_playback_stream->set_volume(effective_volume)
            ->when_rejected([](Error&&) {
                // FIXME: Do we even need this function to return a promise?
            });
    }
}

void AudioPlaybackSink::set_audio_output_state_change_handler(Function<void(bool)> handler)
{
    m_audio_output_monitor->set_output_state_change_handler(move(handler));
}

void AudioPlaybackSink::set_playback_rate(float rate)
{
    VERIFY(rate >= 0);
    m_output_queue->set_playback_rate(rate);
    update_playback_stream_state();
}

}
