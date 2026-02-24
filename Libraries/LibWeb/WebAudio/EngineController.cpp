/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/AudioContextRegistry.h>
#include <LibWeb/WebAudio/EngineController.h>

namespace Web::WebAudio {

EngineController::EngineController() = default;

EngineController& EngineController::the()
{
    static EngineController service;
    return service;
}

ErrorOr<void> EngineController::open_webaudio_session(ClientId client_id, u32 target_latency_ms, u64 page_id, Function<void(ErrorOr<DeviceFormat> const&)> on_complete)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine) {
        m_engine = make<Render::AudioContextRegistry>();
        WA_DBGLN("[WebAudio] EngineController: created AudioContextRegistry engine");
    }

    return m_engine->open_webaudio_session(client_id, target_latency_ms, page_id,
        [on_complete = move(on_complete)](ErrorOr<Render::AudioContextRegistry::OutputSessionFormat> const& format_or_error) {
            if (format_or_error.is_error()) {
                on_complete(Error::from_string_literal("WebAudio: failed to open output device"));
                return;
            }

            on_complete(DeviceFormat {
                .sample_rate = format_or_error.value().sample_rate,
                .channel_count = format_or_error.value().channel_count,
            });
        });
}

Optional<EngineController::DeviceFormat> EngineController::output_device_format(ClientId client_id) const
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return {};

    auto format = m_engine->output_session_format(client_id);
    if (!format.has_value())
        return {};

    return DeviceFormat {
        .sample_rate = format->sample_rate,
        .channel_count = format->channel_count,
    };
}

EngineController::ClientId EngineController::register_client(BaseAudioContext& context, ControlMessageQueue& control_message_queue, AssociatedTaskQueue& associated_task_queue, Atomic<u64>& current_frame, Atomic<u8>& suspended, Atomic<u64>& suspend_generation, Atomic<u64>& underrun_frames_total)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine) {
        m_engine = make<Render::AudioContextRegistry>();
        WA_DBGLN("[WebAudio] EngineController: created AudioContextRegistry engine");
    }

    EngineController::ClientId const client_id = m_engine->register_client(context, control_message_queue, associated_task_queue, current_frame, suspended, suspend_generation, underrun_frames_total);
    WA_DBGLN("[WebAudio] EngineController: registered client {} (client_count={})", client_id, m_engine->client_count());
    return client_id;
}

void EngineController::set_client_suspended(ClientId client_id, bool suspended, u64 generation)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return;
    m_engine->set_client_suspended(client_id, suspended, generation);
}

void EngineController::unregister_client(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return;
    m_engine->unregister_client(client_id);
    WA_DBGLN("[WebAudio] EngineController: unregistered client {} (client_count={})", client_id, m_engine->client_count());
    stop_if_unused();
}

void EngineController::update_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, Vector<Render::SharedAudioBufferBinding> shared_audio_buffer_bindings, NonnullOwnPtr<Render::GraphResourceRegistry> resources, Vector<Render::WorkletModule> worklet_modules, Vector<Render::WorkletNodeDefinition> worklet_node_definitions, Vector<Render::WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine) {
        return;
    }

    WA_DBGLN("[WebAudio] EngineController: update_client_render_graph client_id={} graph_sr={} bytes={} (client_count={})", client_id, graph_sample_rate, encoded_graph.size(), m_engine->client_count());
    m_engine->update_client_render_graph(client_id, graph_sample_rate, move(encoded_graph), move(shared_audio_buffer_bindings), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void EngineController::refresh_client_timing(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return;
    m_engine->refresh_client_timing(client_id);
}

void EngineController::stop_if_unused()
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return;
    size_t const client_count = m_engine->client_count();
    if (client_count != 0)
        return;

    WA_DBGLN("[WebAudio] EngineController: shutting down AudioContextRegistry engine (unused, client_count=0)");
    m_engine->shutdown();
    m_engine = nullptr;
}

bool EngineController::try_copy_analyser_snapshot(ClientId client_id, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return false;
    return m_engine->try_copy_analyser_snapshot(client_id, analyser_node_id, fft_size, out_time_domain, out_frequency_db, out_render_quantum_index);
}

bool EngineController::try_copy_dynamics_compressor_reduction(ClientId client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    if (!m_engine)
        return false;
    return m_engine->try_copy_dynamics_compressor_reduction(client_id, compressor_node_id, out_reduction_db, out_render_quantum_index);
}

}
