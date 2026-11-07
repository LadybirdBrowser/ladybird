/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibWeb/WebAudio/Rendering/RealtimeAudioRenderer.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

struct RealtimeAudioRendererCallbacks : AtomicRefCounted<RealtimeAudioRendererCallbacks> {
    Core::EventLoop& event_loop { Core::EventLoop::current() };
    Function<void(Vector<NodeID> const&)> on_sources_ended;
};

NonnullRefPtr<RealtimeAudioRenderer> RealtimeAudioRenderer::create(NonnullRefPtr<ControlMessageQueue> control_message_queue, NodeID destination_node_id, float sample_rate, size_t quantum_size, NonnullRefPtr<Media::AudioOutputFactory> audio_output_factory)
{
    return adopt_ref(*new RealtimeAudioRenderer(move(control_message_queue), destination_node_id, sample_rate, quantum_size, move(audio_output_factory)));
}

RealtimeAudioRenderer::RealtimeAudioRenderer(NonnullRefPtr<ControlMessageQueue> control_message_queue, NodeID destination_node_id, float sample_rate, size_t quantum_size, NonnullRefPtr<Media::AudioOutputFactory> audio_output_factory)
    : m_callbacks(adopt_ref(*new RealtimeAudioRendererCallbacks))
    , m_audio_output_factory(move(audio_output_factory))
{
    RustFFI::RendererCallbacks callbacks {
        { m_callbacks.ptr(), [](void* service) { static_cast<RealtimeAudioRendererCallbacks*>(service)->ref(); }, [](void* service) { static_cast<RealtimeAudioRendererCallbacks*>(service)->unref(); } },
        [](void* service, void* task, void (*invoke)(void*)) {
            static_cast<RealtimeAudioRendererCallbacks*>(service)->event_loop.deferred_invoke([task, invoke] { invoke(task); });
        },
        [](void*) { },
        [](void*, double) { },
        [](void* service, ReadonlySpan<u64> ids) {
            auto& callbacks = *static_cast<RealtimeAudioRendererCallbacks*>(service);
            if (!callbacks.on_sources_ended)
                return;
            Vector<NodeID> nodes;
            for (auto id : ids)
                nodes.append(NodeID { id });
            callbacks.on_sources_ended(nodes);
        },
    };
    m_renderer = RustFFI::web_audio_realtime_renderer_create(control_message_queue->rust_queue(), destination_node_id.value(), sample_rate, quantum_size, callbacks);
}

RealtimeAudioRenderer::~RealtimeAudioRenderer() { RustFFI::web_audio_realtime_renderer_destroy(m_renderer); }

// The callback is set on the control thread before rendering starts and is always invoked on the control thread. It
// is cleared when the context closes to break the reference cycle with the context.
void RealtimeAudioRenderer::set_on_sources_ended(Function<void(Vector<NodeID> const&)> on_sources_ended)
{
    m_callbacks->on_sources_ended = move(on_sources_ended);
}

void RealtimeAudioRenderer::clear_callbacks()
{
    m_callbacks->on_sources_ended = nullptr;
}

void RealtimeAudioRenderer::start_rendering()
{
    prepare_to_start_rendering();

    // The stream is created in a suspended state so the device configuration can be recorded before the first data
    // request callback runs on the audio thread.
    auto promise = m_audio_output_factory->open_output(Audio::OutputState::Suspended, TARGET_LATENCY_MS, [self = NonnullRefPtr(*this)](Span<float> buffer, MonotonicTime buffer_starts_playing_at) { return self->fill_output_buffer(buffer, buffer_starts_playing_at); });
    promise->when_resolved([self = NonnullRefPtr(*this)](NonnullRefPtr<Audio::PlaybackStream>& stream) {
        self->set_playback_stream(stream);
    });
}

void RealtimeAudioRenderer::prepare_to_start_rendering()
{
    VERIFY(!m_playback_stream);
    RustFFI::web_audio_realtime_renderer_prepare(m_renderer);
}

void RealtimeAudioRenderer::set_playback_stream(NonnullRefPtr<Audio::PlaybackStream> stream)
{
    if (RustFFI::web_audio_realtime_renderer_is_shutting_down(m_renderer)) {
        stream->discard_buffer_and_suspend()->when_rejected([](auto&&) { });
        return;
    }
    auto specification = stream->sample_specification();
    RustFFI::web_audio_realtime_renderer_configure(m_renderer, specification.channel_count(), specification.sample_rate());
    m_playback_stream = stream;
    // The device went away with the process behind it: a new stream picks the graph up where it stands.
    stream->on_output_lost = [self = NonnullRefPtr(*this)] {
        if (RustFFI::web_audio_realtime_renderer_is_shutting_down(self->m_renderer))
            return;
        self->m_playback_stream = nullptr;
        self->start_rendering();
    };

    if (!RustFFI::web_audio_realtime_renderer_is_suspended(m_renderer))
        stream->resume()->when_rejected([](auto&&) { });
}

void RealtimeAudioRenderer::resume()
{
    RustFFI::web_audio_realtime_renderer_set_suspended(m_renderer, false);
    if (m_playback_stream)
        m_playback_stream->resume()->when_rejected([](auto&&) { });
}

void RealtimeAudioRenderer::suspend()
{
    RustFFI::web_audio_realtime_renderer_set_suspended(m_renderer, true);
    if (m_playback_stream)
        m_playback_stream->drain_buffer_and_suspend()->when_rejected([](auto&&) { });
}

void RealtimeAudioRenderer::stop()
{
    RustFFI::web_audio_realtime_renderer_stop(m_renderer);
    if (m_playback_stream) {
        m_playback_stream->discard_buffer_and_suspend()->when_rejected([](auto&&) { });
        m_playback_stream = nullptr;
    }
}

double RealtimeAudioRenderer::output_time_played() const
{
    return RustFFI::web_audio_realtime_renderer_output_time(m_renderer, MonotonicTime::now().nanoseconds());
}

ReadonlySpan<float> RealtimeAudioRenderer::fill_output_buffer(Span<float> buffer, MonotonicTime buffer_starts_playing_at)
{
    auto count = RustFFI::web_audio_realtime_renderer_fill(m_renderer, buffer, buffer_starts_playing_at.nanoseconds());
    return buffer.trim(count);
}

u64 RealtimeAudioRenderer::frames_rendered() const { return RustFFI::web_audio_realtime_renderer_frames_rendered(m_renderer); }

}
