/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/OfflineAudioRenderer.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

struct OfflineAudioRendererCallbacks : AtomicRefCounted<OfflineAudioRendererCallbacks> {
    Core::EventLoop& event_loop { Core::EventLoop::current() };
    Function<void()> on_complete;
    Function<void(double)> on_suspended;
    Function<void(Vector<NodeID> const&)> on_sources_ended;
};

NonnullRefPtr<OfflineAudioRenderer> OfflineAudioRenderer::create(void const* control_message_queue, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size)
{
    return adopt_ref(*new OfflineAudioRenderer(control_message_queue, destination_node_id, number_of_channels, length, sample_rate, quantum_size));
}

OfflineAudioRenderer::OfflineAudioRenderer(void const* control_message_queue, NodeID destination_node_id, size_t number_of_channels, size_t length, float sample_rate, size_t quantum_size)
    : m_callbacks(adopt_ref(*new OfflineAudioRendererCallbacks))
{
    RustFFI::RendererCallbacks callbacks {
        { m_callbacks.ptr(), [](void* service) { static_cast<OfflineAudioRendererCallbacks*>(service)->ref(); }, [](void* service) { static_cast<OfflineAudioRendererCallbacks*>(service)->unref(); } },
        [](void* service, void* task, void (*invoke)(void*)) {
            static_cast<OfflineAudioRendererCallbacks*>(service)->event_loop.deferred_invoke([task, invoke] { invoke(task); });
        },
        [](void* service) {
            auto& callbacks = *static_cast<OfflineAudioRendererCallbacks*>(service);
            if (callbacks.on_complete)
                callbacks.on_complete();
        },
        [](void* service, double time) {
            auto& callbacks = *static_cast<OfflineAudioRendererCallbacks*>(service);
            if (callbacks.on_suspended)
                callbacks.on_suspended(time);
        },
        [](void* service, ReadonlySpan<u64> ids) {
            auto& callbacks = *static_cast<OfflineAudioRendererCallbacks*>(service);
            if (!callbacks.on_sources_ended)
                return;
            Vector<NodeID> nodes;
            for (auto id : ids)
                nodes.append(NodeID { id });
            callbacks.on_sources_ended(nodes);
        },
    };
    m_renderer = RustFFI::web_audio_offline_renderer_create(control_message_queue, destination_node_id.value(), number_of_channels, length, sample_rate, quantum_size, callbacks);
    m_rendered_channels.resize(number_of_channels);
    for (size_t channel = 0; channel < number_of_channels; ++channel)
        RustFFI::web_audio_offline_renderer_channel(m_renderer, channel, &m_rendered_channels[channel]);
}

OfflineAudioRenderer::~OfflineAudioRenderer() { RustFFI::web_audio_offline_renderer_destroy(m_renderer); }
void OfflineAudioRenderer::set_on_complete(Function<void()> callback) { m_callbacks->on_complete = move(callback); }
void OfflineAudioRenderer::set_on_suspended(Function<void(double)> callback) { m_callbacks->on_suspended = move(callback); }
void OfflineAudioRenderer::set_on_sources_ended(Function<void(Vector<NodeID> const&)> callback) { m_callbacks->on_sources_ended = move(callback); }
void OfflineAudioRenderer::clear_callbacks()
{
    m_callbacks->on_complete = nullptr;
    m_callbacks->on_suspended = nullptr;
    m_callbacks->on_sources_ended = nullptr;
}
void OfflineAudioRenderer::start_rendering() { RustFFI::web_audio_offline_renderer_start(m_renderer); }
void OfflineAudioRenderer::stop() { RustFFI::web_audio_offline_renderer_stop(m_renderer); }
bool OfflineAudioRenderer::request_suspend(u64 frame) { return RustFFI::web_audio_offline_renderer_request_suspend(m_renderer, frame); }
void OfflineAudioRenderer::resume() { RustFFI::web_audio_offline_renderer_resume(m_renderer); }
u64 OfflineAudioRenderer::frames_rendered() const { return RustFFI::web_audio_offline_renderer_frames_rendered(m_renderer); }

}
