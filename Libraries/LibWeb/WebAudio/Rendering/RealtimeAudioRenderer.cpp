/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/NullPlaybackStream.h>
#include <LibWeb/WebAudio/Rendering/RealtimeAudioRenderer.h>

namespace Web::WebAudio::Rendering {

static constexpr u32 TARGET_LATENCY_MS = 100;
NonnullRefPtr<RealtimeAudioRenderer> RealtimeAudioRenderer::create(NonnullRefPtr<ControlMessageQueue> control_message_queue, NodeID destination_node_id, float sample_rate, size_t quantum_size)
{
    return adopt_ref(*new RealtimeAudioRenderer(move(control_message_queue), destination_node_id, sample_rate, quantum_size));
}

RealtimeAudioRenderer::RealtimeAudioRenderer(NonnullRefPtr<ControlMessageQueue> control_message_queue, NodeID destination_node_id, float sample_rate, size_t quantum_size)
    : m_control_message_queue(move(control_message_queue))
    , m_destination_node_id(destination_node_id)
    , m_sample_rate(sample_rate)
    , m_quantum_size(quantum_size)
    , m_main_thread_event_loop(Core::EventLoop::current())
    , m_audio_output_monitor(Audio::AudioOutputMonitor::create(m_main_thread_event_loop))
{
}

RealtimeAudioRenderer::~RealtimeAudioRenderer() = default;

// The callback is set on the control thread before rendering starts and is always invoked on the control thread. It
// is cleared when the context closes to break the reference cycle with the context.
void RealtimeAudioRenderer::set_on_sources_ended(Function<void(Vector<NodeID> const&)> on_sources_ended)
{
    m_on_sources_ended = move(on_sources_ended);
}

void RealtimeAudioRenderer::set_on_audio_output_state_changed(Function<void(bool)> on_audio_output_state_changed)
{
    m_audio_output_monitor->set_output_state_change_handler(move(on_audio_output_state_changed));
}

void RealtimeAudioRenderer::clear_callbacks()
{
    m_on_sources_ended = nullptr;
    m_audio_output_monitor->set_output_state_change_handler(nullptr);
}

void RealtimeAudioRenderer::start_rendering()
{
    prepare_to_start_rendering(MonitorOutput::Yes);

    // The stream is created in a suspended state so the device configuration can be recorded before the first data
    // request callback runs on the audio thread.
    auto promise = Audio::PlaybackStream::create_platform_or_null(Audio::OutputState::Suspended, TARGET_LATENCY_MS, [self = NonnullRefPtr(*this)](Span<float> buffer) { return self->fill_output_buffer(buffer); });
    promise->when_resolved([self = NonnullRefPtr(*this)](NonnullRefPtr<Audio::PlaybackStream>& stream) {
        self->set_playback_stream(stream);
    });
}

void RealtimeAudioRenderer::start_rendering_with_null_output()
{
    prepare_to_start_rendering(MonitorOutput::No);
    set_playback_stream(Audio::NullPlaybackStream::create(Audio::OutputState::Suspended, TARGET_LATENCY_MS, [self = NonnullRefPtr(*this)](Span<float> buffer) { return self->fill_output_buffer(buffer); }));
}

void RealtimeAudioRenderer::prepare_to_start_rendering(MonitorOutput monitor_output)
{
    VERIFY(!m_playback_stream);
    m_monitor_output = monitor_output;
    m_graph.set_destination(m_destination_node_id);
}

void RealtimeAudioRenderer::set_playback_stream(NonnullRefPtr<Audio::PlaybackStream> stream)
{
    if (m_shutting_down.load()) {
        stream->discard_buffer_and_suspend()->when_rejected([](auto&&) { });
        return;
    }
    auto specification = stream->sample_specification();
    m_device_channel_count = specification.channel_count();
    m_device_bus = make<AudioBus>(specification.channel_count(), m_quantum_size);

    // When the device sample rate differs from the context's, the rendered audio is resampled by advancing
    // through the rendered frames at this rate.
    m_playhead_step = m_sample_rate / static_cast<double>(specification.sample_rate());
    m_playback_stream = stream;
    stream->set_volume(m_muted ? 0.0 : 1.0)->when_rejected([](auto&&) { });

    if (!m_suspended.load()) {
        m_audio_output_monitor->set_enabled(m_monitor_output == MonitorOutput::Yes);
        stream->resume()->when_rejected([](auto&&) { });
    }
}

void RealtimeAudioRenderer::resume()
{
    m_suspended.store(false);
    m_audio_output_monitor->set_enabled(m_monitor_output == MonitorOutput::Yes);
    if (m_playback_stream)
        m_playback_stream->resume()->when_rejected([](auto&&) { });
}

void RealtimeAudioRenderer::suspend()
{
    m_suspended.store(true);
    m_audio_output_monitor->set_enabled(false);
    if (m_playback_stream)
        m_playback_stream->drain_buffer_and_suspend()->when_rejected([](auto&&) { });
}

void RealtimeAudioRenderer::stop()
{
    m_shutting_down.store(true);
    m_audio_output_monitor->set_enabled(false);
    if (m_playback_stream) {
        m_playback_stream->discard_buffer_and_suspend()->when_rejected([](auto&&) { });
        m_playback_stream = nullptr;
    }
}

void RealtimeAudioRenderer::set_muted(bool muted)
{
    m_muted = muted;
    m_audio_output_monitor->set_muted(muted);
    if (m_playback_stream)
        m_playback_stream->set_volume(muted ? 0.0 : 1.0)->when_rejected([](auto&&) { });
}

// The amount of audio the output device has played so far, in seconds.
double RealtimeAudioRenderer::output_time_played() const
{
    if (m_playback_stream)
        return m_playback_stream->total_time_played().to_nanoseconds() / 1e9;
    return 0;
}

ReadonlySpan<float> RealtimeAudioRenderer::fill_output_buffer(Span<float> buffer)
{
    if (m_shutting_down.load() || m_device_channel_count == 0) {
        m_audio_output_monitor->update(false);
        return {};
    }

    auto channel_count = m_device_channel_count;
    auto frames_requested = buffer.size() / channel_count;

    // The device is playing the buffer delivered before this one, so everything older than that has been heard.
    auto device_frames_played = m_device_frames_delivered > frames_requested ? m_device_frames_delivered - frames_requested : 0;
    auto frames_played = static_cast<u64>(static_cast<double>(device_frames_played) * m_playhead_step);
    m_device_frames_delivered += frames_requested;

    bool contains_non_silent_samples = false;
    for (size_t frame = 0; frame < frames_requested; ++frame) {
        auto first_frame = static_cast<size_t>(m_playhead);
        auto fraction = static_cast<float>(m_playhead - first_frame);

        // Interpolation between adjacent rendered frames requires one frame of lookahead.
        while (m_pending_samples.size() / channel_count < first_frame + 2)
            render_quantum_into_pending_samples(frames_played);

        for (size_t channel = 0; channel < channel_count; ++channel) {
            auto sample = m_pending_samples[first_frame * channel_count + channel];
            if (fraction != 0.f) {
                auto next_sample = m_pending_samples[(first_frame + 1) * channel_count + channel];
                sample += fraction * (next_sample - sample);
            }
            buffer[frame * channel_count + channel] = sample;
            contains_non_silent_samples |= sample != 0.0f;
        }
        m_playhead += m_playhead_step;
    }

    // Trim rendered frames preceding the playhead. If resampling advances the playhead beyond the rendered frames,
    // carry the remaining offset into the next callback.
    auto rendered_frames = m_pending_samples.size() / channel_count;
    auto consumed_frames = min(static_cast<size_t>(m_playhead), rendered_frames);
    if (consumed_frames > 0) {
        m_pending_samples.remove(0, consumed_frames * channel_count);
        m_playhead -= consumed_frames;
    }
    m_audio_output_monitor->update(contains_non_silent_samples);
    return buffer;
}

void RealtimeAudioRenderer::render_quantum_into_pending_samples(u64 frames_played)
{
    m_graph.apply_control_messages(m_control_message_queue->drain());

    auto quantum_start_frame = m_frames_rendered.load();
    auto frames_rendered_after_quantum = quantum_start_frame + m_quantum_size;
    RenderContext context {
        .sample_rate = m_sample_rate,
        .quantum_size = m_quantum_size,
        .quantum_start_frame = quantum_start_frame,
        .output_latency_in_frames = frames_rendered_after_quantum > frames_played ? frames_rendered_after_quantum - frames_played : 0,
    };
    m_graph.render_quantum(context);

    // Mix the destination's output to the stream's channel layout and append it as interleaved samples.
    if (m_device_bus) {
        auto& device_bus = *m_device_bus;
        device_bus.zero();
        if (auto* destination_node = m_graph.destination())
            device_bus.sum_from(destination_node->output(0), Bindings::ChannelInterpretation::Speakers);

        auto sample_offset = m_pending_samples.size();
        m_pending_samples.resize(sample_offset + m_quantum_size * m_device_channel_count);
        for (size_t frame = 0; frame < m_quantum_size; ++frame) {
            for (size_t channel = 0; channel < m_device_channel_count; ++channel)
                m_pending_samples[sample_offset + frame * m_device_channel_count + channel] = device_bus.channel(channel)[frame];
        }
    }

    m_frames_rendered.fetch_add(m_quantum_size);

    if (auto ended_nodes = m_graph.take_ended_nodes(); !ended_nodes.is_empty()) {
        m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), ended_nodes = move(ended_nodes)] {
            if (self->m_on_sources_ended)
                self->m_on_sources_ended(ended_nodes);
        });
    }
}

}
