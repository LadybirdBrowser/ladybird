/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/AudioService.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/WebAudioClientRegistry.h>

namespace Web::WebAudio {

AudioService::AudioService() = default;

AudioService& AudioService::the()
{
    static AudioService service;
    return service;
}

ErrorOr<AudioService::DeviceFormat> AudioService::ensure_output_device_open(u32 target_latency_ms, u64 page_id)
{
    if (!m_engine) {
        m_engine = make<Render::WebAudioClientRegistry>();
        if (::Web::WebAudio::should_log_graph_updates())
            WA_GR_DBGLN("[WebAudio] AudioService: created WebAudioClientRegistry engine");
    }

    auto format = TRY(m_engine->ensure_output_device_open(target_latency_ms, page_id));
    return DeviceFormat {
        .sample_rate = format.sample_rate,
        .channel_count = format.channel_count,
    };
}

AudioService::ClientId AudioService::register_client(BaseAudioContext& context, ControlMessageQueue& control_message_queue, AssociatedTaskQueue& associated_task_queue, Atomic<u64>& current_frame, Atomic<u64>& suspend_state)
{
    VERIFY(m_engine);

    AudioService::ClientId const client_id = m_engine->register_client(context, control_message_queue, associated_task_queue, current_frame, suspend_state);
    if (::Web::WebAudio::should_log_graph_updates())
        WA_GR_DBGLN("[WebAudio] AudioService: registered client {} (client_count={})", client_id, m_engine->client_count());
    return client_id;
}

void AudioService::set_client_suspended(ClientId client_id, bool suspended, u64 generation)
{
    if (!m_engine)
        return;
    m_engine->set_client_suspended(client_id, suspended, generation);
}

void AudioService::unregister_client(ClientId client_id)
{
    if (!m_engine)
        return;
    m_engine->unregister_client(client_id);
    if (::Web::WebAudio::should_log_graph_updates())
        WA_GR_DBGLN("[WebAudio] AudioService: unregistered client {} (client_count={})", client_id, m_engine->client_count());
    stop_if_unused();
}

void AudioService::set_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<Render::GraphResourceRegistry> resources, Vector<Render::WorkletModule> worklet_modules, Vector<Render::WorkletNodeDefinition> worklet_node_definitions, Vector<Render::WorkletPortBinding> worklet_port_bindings)
{
    VERIFY(m_engine);
    if (::Web::WebAudio::should_log_graph_updates())
        WA_GR_DBGLN("[WebAudio] AudioService: set_client_render_graph client_id={} graph_sr={} bytes={} (client_count={})", client_id, graph_sample_rate, encoded_graph.size(), m_engine->client_count());
    m_engine->set_client_render_graph(client_id, graph_sample_rate, move(encoded_graph), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void AudioService::update_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<Render::GraphResourceRegistry> resources, Vector<Render::WorkletModule> worklet_modules, Vector<Render::WorkletNodeDefinition> worklet_node_definitions, Vector<Render::WorkletPortBinding> worklet_port_bindings)
{
    if (!m_engine) {
        if (::Web::WebAudio::should_log_graph_updates())
            WA_GR_DBGLN("[WebAudio] AudioService: dropping graph update (no engine) client_id={} graph_sr={} bytes={}", client_id, graph_sample_rate, encoded_graph.size());
        return;
    }

    if (::Web::WebAudio::should_log_graph_updates())
        WA_GR_DBGLN("[WebAudio] AudioService: update_client_render_graph client_id={} graph_sr={} bytes={} (client_count={})", client_id, graph_sample_rate, encoded_graph.size(), m_engine->client_count());
    m_engine->update_client_render_graph(client_id, graph_sample_rate, move(encoded_graph), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void AudioService::stop_if_unused()
{
    if (!m_engine)
        return;
    size_t const client_count = m_engine->client_count();
    if (client_count != 0)
        return;

    if (::Web::WebAudio::should_log_graph_updates())
        WA_GR_DBGLN("[WebAudio] AudioService: shutting down WebAudioClientRegistry engine (unused, client_count=0)");
    m_engine->shutdown();
    m_engine = nullptr;
}

bool AudioService::try_copy_analyser_snapshot(ClientId client_id, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index)
{
    if (!m_engine)
        return false;
    return m_engine->try_copy_analyser_snapshot(client_id, analyser_node_id, fft_size, out_time_domain, out_frequency_db, out_render_quantum_index);
}

bool AudioService::try_copy_dynamics_compressor_reduction(ClientId client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index)
{
    if (!m_engine)
        return false;
    return m_engine->try_copy_dynamics_compressor_reduction(client_id, compressor_node_id, out_reduction_db, out_render_quantum_index);
}

}
