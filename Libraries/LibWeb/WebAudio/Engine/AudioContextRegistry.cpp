/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Engine/AudioContextRegistry.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/SessionRouter.h>
#include <LibWeb/WebAudio/Worklet/WorkletEventController.h>
#include <LibWebAudio/SessionClientOfWebAudioWorker.h>

namespace Web::WebAudio::Render {

static u32 min_target_latency_ms_for_graph(GraphResourceRegistry const& resources, f32 graph_sample_rate)
{
    if (graph_sample_rate <= 0.0f)
        return 0;

    u32 max_script_processor_buffer_size = 0;
    auto const& script_processors = resources.script_processor_transport_metadata();
    for (auto const& it : script_processors)
        max_script_processor_buffer_size = max(max_script_processor_buffer_size, it.value.buffer_size);

    if (max_script_processor_buffer_size == 0)
        return 0;

    // If a graph contains ScriptProcessorNodes, their bufferSize can incur unavoidable latency.
    // Only apply this as a minimum when ScriptProcessor is present so other graphs remain responsive.
    f64 const buffer_ms = (1000.0 * static_cast<f64>(max_script_processor_buffer_size)) / static_cast<f64>(graph_sample_rate);
    u32 const rounded_ms = static_cast<u32>(ceil(buffer_ms));
    return min(rounded_ms, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);
}

void AudioContextRegistry::set_webaudio_client(NonnullRefPtr<::Web::WebAudio::SessionClientOfWebAudioWorker> client)
{
    SessionRouter::set_webaudio_client(move(client));

    auto client_ref = SessionRouter::webaudio_client();
    if (!client_ref)
        return;

    install_worklet_event_callbacks(*client_ref);
}

AudioContextRegistry::AudioContextRegistry()
{
    if (auto client = SessionRouter::webaudio_client())
        install_worklet_event_callbacks(*client);
}

AudioContextRegistry::~AudioContextRegistry()
{
    shutdown();
    clear_all_worklet_session_state();
    clear_all_worklet_contexts();
}

bool AudioContextRegistry::try_copy_analyser_snapshot(ClientId client_id, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    SessionRouter* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return false;
        worker_session = it->value.worker_session.ptr();
    }
    return worker_session->try_copy_analyser_snapshot(analyser_node_id, fft_size, out_time_domain, out_frequency_db, out_render_quantum_index);
}

bool AudioContextRegistry::try_copy_dynamics_compressor_reduction(ClientId client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    SessionRouter* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return false;
        worker_session = it->value.worker_session.ptr();
    }
    return worker_session->try_copy_dynamics_compressor_reduction(compressor_node_id, out_reduction_db, out_render_quantum_index);
}

void AudioContextRegistry::refresh_client_timing(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    SessionRouter* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;
        worker_session = it->value.worker_session.ptr();
    }
    worker_session->refresh_timing();
}

ErrorOr<void> AudioContextRegistry::open_webaudio_session(ClientId client_id, u32 target_latency_ms, u64 page_id, Function<void(ErrorOr<OutputSessionFormat> const&)> on_complete)
{
    ASSERT_CONTROL_THREAD();

    Optional<OutputSessionFormat> cached_device_format;
    SessionRouter* worker_session = nullptr;
    u32 clamped_target_latency_ms = 0;
    bool should_begin_open = false;

    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return Error::from_string_literal("WebAudio: open_webaudio_session called for unknown client");

        auto& state = it->value.session_state;
        worker_session = it->value.worker_session.ptr();

        if (worker_session->has_session_transport_open() && state.device_format.has_value()) {
            cached_device_format = state.device_format;
        } else {
            state.page_id = page_id;

            u32 effective_target_latency_ms = max(target_latency_ms, state.min_target_latency_ms_from_graph);
            state.target_latency_ms = clamp(effective_target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);
            clamped_target_latency_ms = state.target_latency_ms;

            state.pending_output_open_callbacks.append(move(on_complete));
            if (!state.output_open_in_progress) {
                state.output_open_in_progress = true;
                should_begin_open = true;
            }
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

Optional<AudioContextRegistry::OutputSessionFormat> AudioContextRegistry::output_session_format(ClientId client_id) const
{
    ASSERT_CONTROL_THREAD();

    Threading::MutexLocker locker { m_clients_mutex };
    auto it = m_clients.find(client_id);
    if (it == m_clients.end())
        return {};

    return it->value.session_state.device_format;
}

Optional<u64> AudioContextRegistry::mapped_session_id(ClientId client_id) const
{
    Threading::MutexLocker locker { m_clients_mutex };
    auto it = m_clients.find(client_id);
    if (it == m_clients.end() || it->value.session_state.mapped_session_id == 0)
        return {};
    return it->value.session_state.mapped_session_id;
}

void AudioContextRegistry::set_graph_min_target_latency_ms(ClientId client_id, u32 min_target_latency_ms)
{
    Threading::MutexLocker locker { m_clients_mutex };
    auto it = m_clients.find(client_id);
    if (it == m_clients.end())
        return;
    it->value.session_state.min_target_latency_ms_from_graph = min_target_latency_ms;
}

ErrorOr<void> AudioContextRegistry::maybe_reopen_webaudio_session(ClientId client_id, SessionRouter& worker_session, u32 new_target_latency_ms)
{
    Optional<u64> page_id;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return Error::from_string_literal("WebAudio: cannot reopen output for unknown client");

        auto& state = it->value.session_state;
        if (!worker_session.has_session_transport_open())
            return {};

        if (!state.page_id.has_value())
            return Error::from_string_literal("WebAudio: cannot reopen output without page_id");

        u32 const clamped_target_latency_ms = clamp(new_target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);
        if (clamped_target_latency_ms <= state.target_latency_ms)
            return {};

        state.device_format.clear();
        state.target_latency_ms = clamped_target_latency_ms;
        page_id = state.page_id;
    }

    close_webaudio_session(client_id, worker_session);
    return begin_open(client_id, worker_session, new_target_latency_ms, page_id.value());
}

void AudioContextRegistry::close_webaudio_session(ClientId client_id, SessionRouter& worker_session)
{
    u64 old_session_id = 0;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;

        auto& state = it->value.session_state;
        old_session_id = state.mapped_session_id;

        worker_session.close_session_transport();

        state.mapped_session_id = 0;
        state.device_format.clear();
        state.output_open_in_progress = false;
        state.pending_output_open_callbacks.clear();
    }

    if (old_session_id != 0)
        on_client_session_mapping_changed(client_id, old_session_id, 0);
}

ErrorOr<void> AudioContextRegistry::begin_open(ClientId client_id, SessionRouter& worker_session, u32 target_latency_ms, u64 page_id)
{
    auto weak_self = make_weak_ptr();
    TRY(worker_session.open_session_transport(target_latency_ms, page_id,
        [weak_self, client_id, weak_worker_session = worker_session.make_weak_ptr()](ErrorOr<SessionRouter::OutputSessionFormat> const& open_result) mutable {
            auto* self = weak_self.ptr();
            if (!self)
                return;

            Vector<Function<void(ErrorOr<OutputSessionFormat> const&)>> callbacks;
            Optional<OutputSessionFormat> output_session_format;
            u64 old_session_id = 0;
            u64 new_session_id = 0;

            {
                Threading::MutexLocker locker { self->m_clients_mutex };
                auto it = self->m_clients.find(client_id);
                if (it == self->m_clients.end())
                    return;

                auto& state = it->value.session_state;
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

void AudioContextRegistry::on_client_session_mapping_changed(ClientId client_id, u64 old_session_id, u64 new_session_id)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_clients.find(client_id);
    if (it == m_clients.end())
        return;

    if (old_session_id != 0) {
        unregister_worklet_context(old_session_id);
        clear_worklet_session_state(old_session_id);
    }

    if (new_session_id != 0)
        register_worklet_context(new_session_id, it->value.state.context);
}

AudioContextRegistry::ClientId AudioContextRegistry::register_client(BaseAudioContext& context, ControlMessageQueue& control_message_queue, AssociatedTaskQueue& associated_task_queue, Atomic<u64>& current_frame, Atomic<u8>& suspended, Atomic<u64>& suspend_generation, Atomic<u64>& underrun_frames_total)
{
    ASSERT_CONTROL_THREAD();

    auto client_id = m_next_client_id++;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        m_clients.set(client_id, ClientEntry {
                                     .state = ClientState {
                                         .context = GC::Weak<BaseAudioContext> { context },
                                     },
                                     .worker_session = make<SessionRouter>(client_id, &current_frame, &suspended, &suspend_generation, &underrun_frames_total),
                                     .session_state = {},
                                 });
    }

    (void)control_message_queue;
    (void)associated_task_queue;
    return client_id;
}

void AudioContextRegistry::unregister_client(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    SessionRouter* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;
        worker_session = it->value.worker_session.ptr();
    }

    close_webaudio_session(client_id, *worker_session);

    Threading::MutexLocker relock { m_clients_mutex };
    m_clients.remove(client_id);
}

void AudioContextRegistry::update_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, Vector<SharedAudioBufferBinding> shared_audio_buffer_bindings, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules, Vector<WorkletNodeDefinition> worklet_node_definitions, Vector<WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();

    SessionRouter* worker_session = nullptr;
    u32 min_target_latency_ms = 0;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;

        min_target_latency_ms = min_target_latency_ms_for_graph(*resources, graph_sample_rate);
        it->value.session_state.min_target_latency_ms_from_graph = min_target_latency_ms;
        worker_session = it->value.worker_session.ptr();
    }

    (void)maybe_reopen_webaudio_session(client_id, *worker_session, min_target_latency_ms);

    worker_session->update_client_render_graph(graph_sample_rate, move(encoded_graph), move(shared_audio_buffer_bindings), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void AudioContextRegistry::set_client_suspended(ClientId client_id, bool suspended, u64 generation)
{
    ASSERT_CONTROL_THREAD();
    SessionRouter* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;
        worker_session = it->value.worker_session.ptr();
    }
    worker_session->set_client_suspended(suspended, generation);
}

void AudioContextRegistry::shutdown()
{
    struct ClientSession {
        ClientId client_id { 0 };
        SessionRouter* worker_session { nullptr };
    };

    Vector<ClientSession> sessions;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        sessions.ensure_capacity(m_clients.size());
        for (auto& it : m_clients)
            sessions.append({ .client_id = it.key, .worker_session = it.value.worker_session.ptr() });
    }

    for (auto& entry : sessions)
        close_webaudio_session(entry.client_id, *entry.worker_session);

    Threading::MutexLocker relock { m_clients_mutex };
    for (auto const& entry : m_clients) {
        u64 mapped_session_id = entry.value.session_state.mapped_session_id;
        if (mapped_session_id != 0)
            unregister_worklet_context(mapped_session_id);
        if (mapped_session_id != 0)
            clear_worklet_session_state(mapped_session_id);
    }
    m_clients.clear();
}

size_t AudioContextRegistry::client_count() const
{
    // Control-thread only.
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker { m_clients_mutex };
    return m_clients.size();
}

}
