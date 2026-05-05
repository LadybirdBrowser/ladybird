/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/AudioContextRegistry.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Script/WorkletEventController.h>
#include <LibWeb/WebAudio/SessionRouter.h>
#include <LibWebAudio/Engine/Policy.h>

namespace Web::WebAudio::Render {

AudioContextRegistry::AudioContextRegistry() = default;

AudioContextRegistry::~AudioContextRegistry()
{
    ASSERT_CONTROL_THREAD();
    shutdown();
    clear_all_worklet_session_state();
    clear_all_worklet_contexts();
}

void AudioContextRegistry::set_webaudio_transport_provider(
    Function<ErrorOr<IPC::TransportHandle>(u64 page_id)> provider)
{
    SessionRouter::set_webaudio_transport_provider(move(provider));
}

bool AudioContextRegistry::try_copy_analyser_snapshot(ClientId client_id, NodeID analyser_node_id,
    u32 fft_size, Span<f32> out_time_domain,
    Span<f32> out_frequency_db,
    u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return false;

    SessionRouter& worker_session = *it->value.worker_session;
    return worker_session.try_copy_analyser_snapshot(analyser_node_id, fft_size, out_time_domain,
        out_frequency_db, out_render_quantum_index);
}

bool AudioContextRegistry::try_copy_dynamics_compressor_reduction(ClientId client_id,
    NodeID compressor_node_id,
    f32& out_reduction_db,
    u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return false;

    SessionRouter& worker_session = *it->value.worker_session;
    return worker_session.try_copy_dynamics_compressor_reduction(compressor_node_id, out_reduction_db,
        out_render_quantum_index);
}

void AudioContextRegistry::refresh_client_timing(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return;

    it->value.worker_session->refresh_timing();
}

ErrorOr<void> AudioContextRegistry::open_webaudio_session(
    ClientId client_id, u32 target_latency_ms, u64 page_id,
    Function<void(ErrorOr<OutputSessionFormat> const&)> on_complete)
{
    ASSERT_CONTROL_THREAD();

    Optional<OutputSessionFormat> cached_device_format;
    SessionRouter* worker_session = nullptr;
    u32 clamped_target_latency_ms = 0;
    bool should_begin_open = false;

    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return Error::from_string_literal("WebAudio: open_webaudio_session called for unknown client");

    auto& state = it->value;
    worker_session = it->value.worker_session.ptr();

    if (worker_session->has_session_transport_open() && state.device_format.has_value()) {
        cached_device_format = state.device_format;
    } else {
        state.target_latency_ms = clamp(target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);
        clamped_target_latency_ms = state.target_latency_ms;

        state.pending_output_open_callbacks.append(move(on_complete));
        if (!state.output_open_in_progress) {
            state.output_open_in_progress = true;
            should_begin_open = true;
        }
    }

    if (cached_device_format.has_value()) {
        on_complete(cached_device_format.value());
        return {};
    }

    if (!should_begin_open)
        return {};

    return begin_open(client_id, *worker_session, clamped_target_latency_ms, page_id);
}

Optional<AudioContextRegistry::OutputSessionFormat>
AudioContextRegistry::output_session_format(ClientId client_id) const
{
    ASSERT_CONTROL_THREAD();

    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return {};

    return it->value.device_format;
}

void AudioContextRegistry::close_webaudio_session(ClientId client_id, SessionRouter& worker_session)
{
    ASSERT_CONTROL_THREAD();
    u64 old_session_id = 0;
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return;

    auto& state = it->value;
    old_session_id = state.mapped_session_id;

    worker_session.close_session_transport();

    state.mapped_session_id = 0;
    state.device_format.clear();
    state.output_open_in_progress = false;
    state.pending_output_open_callbacks.clear();

    if (old_session_id != 0)
        on_client_session_mapping_changed(client_id, old_session_id, 0);
}

ErrorOr<void> AudioContextRegistry::begin_open(ClientId client_id, SessionRouter& worker_session,
    u32 target_latency_ms, u64 page_id)
{
    ASSERT_CONTROL_THREAD();
    auto weak_self = make_weak_ptr();
    TRY(worker_session.open_session_transport(
        target_latency_ms, page_id,
        [weak_self, client_id, weak_worker_session = worker_session.make_weak_ptr()](
            ErrorOr<SessionRouter::OutputSessionFormat> const& open_result) mutable {
            ASSERT_CONTROL_THREAD();
            auto* self = weak_self.ptr();
            if (!self)
                return;

            Vector<Function<void(ErrorOr<OutputSessionFormat> const&)>> callbacks;
            Optional<OutputSessionFormat> output_session_format;
            u64 old_session_id = 0;
            u64 new_session_id = 0;

            auto it = self->m_sessions.find(client_id);
            if (it == self->m_sessions.end())
                return;

            auto& state = it->value;
            state.output_open_in_progress = false;
            callbacks = move(state.pending_output_open_callbacks);
            state.pending_output_open_callbacks.clear();

            if (!open_result.is_error()) {
                state.device_format = OutputSessionFormat {
                    .sample_rate = open_result.value().sample_rate,
                    .channel_count = open_result.value().channel_count,
                };
                output_session_format = state.device_format;

                if (weak_worker_session) {
                    old_session_id = state.mapped_session_id;
                    new_session_id = weak_worker_session->session_id();
                    state.mapped_session_id = new_session_id;
                }
            }
            if (!open_result.is_error()) {
                if (old_session_id != new_session_id)
                    self->on_client_session_mapping_changed(client_id, old_session_id, new_session_id);

                for (auto& callback : callbacks)
                    callback(output_session_format.value());
                return;
            }
            for (auto& callback : callbacks)
                callback(Error::from_string_literal("WebAudio: async output open failed"));
        }));

    return {};
}

void AudioContextRegistry::on_client_session_mapping_changed(ClientId client_id, u64 old_session_id,
    u64 new_session_id)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return;

    if (old_session_id != 0) {
        unregister_worklet_context(old_session_id);
        clear_worklet_session_state(old_session_id);
    }
    if (new_session_id != 0)
        register_worklet_context(new_session_id, it->value.context);
}

AudioContextRegistry::ClientId
AudioContextRegistry::register_client(BaseAudioContext& context, ControlMessageQueue& control_message_queue,
    AssociatedTaskQueue& associated_task_queue, Atomic<u64>& current_frame,
    Atomic<u8>& suspended, Atomic<u64>& suspend_generation,
    Atomic<u64>& underrun_frames_total)
{
    ASSERT_CONTROL_THREAD();
    auto client_id = m_next_client_id++;
    m_sessions.set(client_id,
        SessionState {
            .context = GC::Weak<BaseAudioContext> { context },
            .worker_session = make<SessionRouter>(client_id, &current_frame, &suspended, &suspend_generation, &underrun_frames_total),
            .device_format = {},
            .target_latency_ms = 50,
            .output_open_in_progress = false,
            .pending_output_open_callbacks = {},
            .mapped_session_id = 0,
        });

    (void)control_message_queue;
    (void)associated_task_queue;
    return client_id;
}

void AudioContextRegistry::unregister_client(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return;

    SessionRouter& worker_session = *it->value.worker_session;

    close_webaudio_session(client_id, worker_session);

    m_sessions.remove(client_id);
}

void AudioContextRegistry::update_client_render_graph(
    ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph,
    Vector<SharedAudioBufferBinding> shared_audio_buffer_bindings,
    NonnullOwnPtr<GraphResources> resources, Vector<WorkletModule> worklet_modules,
    Vector<WorkletNodeDefinition> worklet_node_definitions,
    Vector<WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return;

    SessionRouter& worker_session = *it->value.worker_session;

    // ScriptProcessor transport sizing is handled by the stream bindings and worker-side session
    // state. Do not tear down the session transport from inside a graph update.

    worker_session.update_client_render_graph(
        graph_sample_rate, move(encoded_graph), move(shared_audio_buffer_bindings), move(resources),
        move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void AudioContextRegistry::set_client_suspended(ClientId client_id, bool suspended, u64 generation)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_sessions.find(client_id);
    if (it == m_sessions.end())
        return;

    it->value.worker_session->set_client_suspended(suspended, generation);
}

void AudioContextRegistry::shutdown()
{
    ASSERT_CONTROL_THREAD();
    struct ClientSession {
        ClientId client_id { 0 };
        SessionRouter* worker_session { nullptr };
    };

    Vector<ClientSession> sessions;
    sessions.ensure_capacity(m_sessions.size());
    for (auto& it : m_sessions)
        sessions.append({ .client_id = it.key, .worker_session = it.value.worker_session.ptr() });

    for (auto& entry : sessions)
        close_webaudio_session(entry.client_id, *entry.worker_session);

    for (auto const& entry : m_sessions) {
        u64 mapped_session_id = entry.value.mapped_session_id;
        if (mapped_session_id != 0)
            unregister_worklet_context(mapped_session_id);
        if (mapped_session_id != 0)
            clear_worklet_session_state(mapped_session_id);
    }
    m_sessions.clear();
}

size_t AudioContextRegistry::client_count() const
{
    ASSERT_CONTROL_THREAD();
    return m_sessions.size();
}

} // namespace Web::WebAudio::Render
