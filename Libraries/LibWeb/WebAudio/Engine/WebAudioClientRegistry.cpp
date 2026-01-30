/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Math.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/Engine/WebAudioClientRegistry.h>
#include <LibWeb/WebAudio/Engine/WebAudioWorkerSession.h>
#include <LibWebAudioWorkerClient/WebAudioClient.h>

namespace Web::WebAudio::Render {

static WeakPtr<WebAudioClientRegistry> s_registry_instance;

static void enqueue_worklet_processor_error_task(u64 session_id, u64 node_id)
{
    auto registry = s_registry_instance;
    if (!registry)
        return;

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [registry, session_id, node_id] {
            if (auto* instance = registry.ptr())
                instance->handle_worklet_processor_error(session_id, NodeID { node_id });
        }));
}

static void enqueue_worklet_processor_registration_task(u64 session_id, String name, Vector<Web::WebAudio::AudioParamDescriptor> descriptors, u64 generation)
{
    auto registry = s_registry_instance;
    if (!registry)
        return;

    HTML::queue_a_task(HTML::Task::Source::AudioWorklet, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [registry, session_id, name = move(name), descriptors = move(descriptors), generation] {
            if (auto* instance = registry.ptr())
                instance->handle_worklet_processor_registration(session_id, name, descriptors, generation);
        }));
}

static void enqueue_worklet_module_evaluated_task(u64 session_id, u64 module_id, u64 required_generation, bool success, String error_name, String error_message, Vector<String> failed_processor_registrations)
{
    auto registry = s_registry_instance;
    if (!registry)
        return;

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [registry, session_id, module_id, required_generation, success, error_name = move(error_name), error_message = move(error_message), failed_processor_registrations = move(failed_processor_registrations)] {
            if (auto* instance = registry.ptr())
                instance->handle_worklet_module_evaluated(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
        }));
}

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

void WebAudioClientRegistry::set_webaudio_client(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient> client)
{
    WebAudioWorkerSession::set_webaudio_client(move(client));

    auto client_ref = WebAudioWorkerSession::webaudio_client();
    if (!client_ref)
        return;

    client_ref->on_worklet_processor_error = [](u64 session_id, u64 node_id) {
        enqueue_worklet_processor_error_task(session_id, node_id);
    };
    client_ref->on_worklet_processor_registered = [](u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
        enqueue_worklet_processor_registration_task(session_id, name, descriptors, generation);
    };
    client_ref->on_worklet_module_evaluated = [](u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations) {
        enqueue_worklet_module_evaluated_task(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
    };
}

WebAudioClientRegistry::WebAudioClientRegistry()
{
    s_registry_instance = this->make_weak_ptr();

    if (auto client = WebAudioWorkerSession::webaudio_client()) {
        client->on_worklet_processor_error = [](u64 session_id, u64 node_id) {
            enqueue_worklet_processor_error_task(session_id, node_id);
        };
        client->on_worklet_processor_registered = [](u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
            enqueue_worklet_processor_registration_task(session_id, name, descriptors, generation);
        };
        client->on_worklet_module_evaluated = [](u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations) {
            enqueue_worklet_module_evaluated_task(session_id, module_id, required_generation, success, error_name, error_message, failed_processor_registrations);
        };
    }
}

WebAudioClientRegistry::~WebAudioClientRegistry()
{
    shutdown();
    if (s_registry_instance.ptr() == this)
        s_registry_instance = {};
}

bool WebAudioClientRegistry::try_copy_analyser_snapshot(ClientId client_id, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    WebAudioWorkerSession* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return false;
        worker_session = it->value.worker_session.ptr();
    }
    return worker_session->try_copy_analyser_snapshot(*this, client_id, analyser_node_id, fft_size, out_time_domain, out_frequency_db, out_render_quantum_index);
}

bool WebAudioClientRegistry::try_copy_dynamics_compressor_reduction(ClientId client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index)
{
    ASSERT_CONTROL_THREAD();
    WebAudioWorkerSession* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return false;
        worker_session = it->value.worker_session.ptr();
    }
    return worker_session->try_copy_dynamics_compressor_reduction(*this, client_id, compressor_node_id, out_reduction_db, out_render_quantum_index);
}

void WebAudioClientRegistry::refresh_client_timing(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    WebAudioWorkerSession* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;
        worker_session = it->value.worker_session.ptr();
    }
    worker_session->update_current_frames_from_timing_page(*this);
}

ErrorOr<WebAudioClientRegistry::DeviceFormat> WebAudioClientRegistry::ensure_output_device_open(ClientId client_id, u32 target_latency_ms, u64 page_id)
{
    ASSERT_CONTROL_THREAD();

    WebAudioWorkerSession* worker_session = nullptr;
    u32 target_latency = 0;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return Error::from_string_literal("WebAudio: ensure_output_device_open called for unknown client");
        auto& entry = it->value;

        if (entry.worker_session->has_output_open(*this) && entry.device_format.has_value())
            return entry.device_format.value();

        entry.page_id = page_id;

        // Allow the current graph (if it contains ScriptProcessor) to raise the session target latency.
        u32 effective_target_latency_ms = max(target_latency_ms, entry.min_target_latency_ms_from_graph);
        entry.target_latency_ms = clamp(effective_target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);

        worker_session = entry.worker_session.ptr();
        target_latency = entry.target_latency_ms;
    }

    TRY(worker_session->ensure_output_open(*this, target_latency, page_id));

    Threading::MutexLocker relock { m_clients_mutex };
    auto it_after = m_clients.find(client_id);
    if (it_after == m_clients.end())
        return Error::from_string_literal("WebAudio: client removed while opening output");
    auto& entry_after = it_after->value;
    if (!entry_after.device_format.has_value())
        return Error::from_string_literal("WebAudio: no device format after opening output");
    update_client_session_mapping(client_id, entry_after.worker_session->session_id());
    return entry_after.device_format.value();
}

ErrorOr<void> WebAudioClientRegistry::reopen_output_device(ClientId client_id, u32 new_target_latency_ms)
{
    ASSERT_CONTROL_THREAD();

    WebAudioWorkerSession* worker_session = nullptr;
    u64 page_id = 0;
    u32 clamped = 0;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return Error::from_string_literal("WebAudio: cannot reopen output device for unknown client");
        auto& entry = it->value;

        if (!entry.worker_session->has_output_open(*this))
            return {};

        if (!entry.page_id.has_value())
            return Error::from_string_literal("WebAudio: cannot reopen output device without page_id");

        clamped = clamp(new_target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);
        if (clamped <= entry.target_latency_ms)
            return {};

        worker_session = entry.worker_session.ptr();
        page_id = entry.page_id.value();
    }

    worker_session->shutdown_output(*this);

    {
        Threading::MutexLocker relock { m_clients_mutex };
        auto it_after = m_clients.find(client_id);
        if (it_after == m_clients.end())
            return Error::from_string_literal("WebAudio: client removed while reopening output");
        it_after->value.device_format.clear();
        it_after->value.target_latency_ms = clamped;
    }

    TRY(worker_session->ensure_output_open(*this, clamped, page_id));

    Threading::MutexLocker relock_after { m_clients_mutex };
    auto it_final = m_clients.find(client_id);
    if (it_final != m_clients.end())
        update_client_session_mapping(client_id, it_final->value.worker_session->session_id());
    return {};
}

void WebAudioClientRegistry::set_client_device_format(ClientId client_id, DeviceFormat format)
{
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker { m_clients_mutex };
    auto it = m_clients.find(client_id);
    if (it == m_clients.end())
        return;
    it->value.device_format = format;
}

void WebAudioClientRegistry::update_client_session_mapping(ClientId client_id, u64 session_id)
{
    ASSERT_CONTROL_THREAD();
    auto it = m_clients.find(client_id);
    if (it == m_clients.end())
        return;

    if (it->value.session_id == session_id && session_id != 0)
        return;

    if (it->value.session_id != 0)
        m_session_id_to_client.remove(it->value.session_id);

    it->value.session_id = session_id;
    if (session_id != 0)
        m_session_id_to_client.set(session_id, client_id);
}

Optional<WebAudioClientRegistry::ClientId> WebAudioClientRegistry::client_id_for_session(u64 session_id) const
{
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker { m_clients_mutex };
    if (auto it = m_session_id_to_client.find(session_id); it != m_session_id_to_client.end())
        return it->value;
    return {};
}

WebAudioClientRegistry::ClientId WebAudioClientRegistry::register_client(BaseAudioContext& context, ControlMessageQueue& control_message_queue, AssociatedTaskQueue& associated_task_queue, Atomic<u64>& current_frame, Atomic<u64>& suspend_state, Atomic<u64>& underrun_frames_total)
{
    ASSERT_CONTROL_THREAD();

    auto client_id = m_next_client_id++;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        m_clients.set(client_id, ClientEntry {
                                     .state = ClientState {
                                         .context = GC::Weak<BaseAudioContext> { context },
                                         .current_frame = &current_frame,
                                         .suspend_state = &suspend_state,
                                         .underrun_frames_total = &underrun_frames_total,
                                     },
                                     .worker_session = make<WebAudioWorkerSession>(client_id),
                                     .device_format = Optional<DeviceFormat> {},
                                     .page_id = Optional<u64> {},
                                     .target_latency_ms = 50,
                                     .min_target_latency_ms_from_graph = 0,
                                     .registered_processor_descriptors = {},
                                     .failed_processor_registrations = {},
                                     .last_registration_generation = 0,
                                     .session_id = 0,
                                 });
    }

    (void)control_message_queue;
    (void)associated_task_queue;
    return client_id;
}

void WebAudioClientRegistry::unregister_client(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    WebAudioWorkerSession* worker_session = nullptr;
    u64 session_id = 0;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;
        session_id = it->value.session_id;
        worker_session = it->value.worker_session.ptr();
    }

    worker_session->shutdown_output(*this);

    Threading::MutexLocker relock { m_clients_mutex };
    m_clients.remove(client_id);
    if (session_id != 0)
        m_session_id_to_client.remove(session_id);
}

void WebAudioClientRegistry::update_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules, Vector<WorkletNodeDefinition> worklet_node_definitions, Vector<WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();

    WebAudioWorkerSession* worker_session = nullptr;
    u32 new_target_latency = 0;
    u32 current_target_latency = 0;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;

        auto& entry = it->value;
        entry.min_target_latency_ms_from_graph = min_target_latency_ms_for_graph(*resources, graph_sample_rate);
        new_target_latency = entry.min_target_latency_ms_from_graph;
        current_target_latency = entry.target_latency_ms;
        worker_session = entry.worker_session.ptr();
    }

    if (new_target_latency > current_target_latency)
        (void)reopen_output_device(client_id, new_target_latency);

    {
        Threading::MutexLocker relock { m_clients_mutex };
        auto it_after = m_clients.find(client_id);
        if (it_after == m_clients.end())
            return;
        worker_session = it_after->value.worker_session.ptr();
    }

    worker_session->update_client_render_graph(*this, client_id, graph_sample_rate, move(encoded_graph), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void WebAudioClientRegistry::set_client_suspended(ClientId client_id, bool suspended, u64 generation)
{
    ASSERT_CONTROL_THREAD();
    WebAudioWorkerSession* worker_session = nullptr;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id);
        if (it == m_clients.end())
            return;
        worker_session = it->value.worker_session.ptr();
    }
    worker_session->set_client_suspended(*this, client_id, suspended, generation);
}

void WebAudioClientRegistry::shutdown()
{
    Vector<WebAudioWorkerSession*> sessions;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        sessions.ensure_capacity(m_clients.size());
        for (auto& it : m_clients)
            sessions.append(it.value.worker_session.ptr());
    }

    for (auto* session : sessions)
        session->shutdown_output(*this);

    Threading::MutexLocker relock { m_clients_mutex };
    m_clients.clear();
    m_session_id_to_client.clear();
}

size_t WebAudioClientRegistry::client_count() const
{
    // Control-thread only.
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker { m_clients_mutex };
    return m_clients.size();
}

void WebAudioClientRegistry::handle_worklet_processor_error(u64 session_id, NodeID node_id)
{
    ASSERT_CONTROL_THREAD();

    auto client_id = client_id_for_session(session_id);
    if (!client_id.has_value())
        return;

    Vector<GC::Ref<AudioWorkletNode>> nodes_to_notify;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id.value());
        if (it == m_clients.end())
            return;
        auto context = it->value.state.context.ptr();
        if (!context)
            return;
        for (auto const& weak_node : context->audio_nodes_for_snapshot()) {
            if (!weak_node)
                continue;
            auto& node = *weak_node.ptr();
            if (!is<AudioWorkletNode>(node))
                continue;
            auto& worklet_node = static_cast<AudioWorkletNode&>(node);
            if (worklet_node.node_id() == node_id)
                nodes_to_notify.append(worklet_node);
        }
    }

    for (auto& node : nodes_to_notify) {
        auto* context = node->context().ptr();
        context->queue_a_media_element_task("audio worklet processorerror fired"sv, GC::create_function(context->heap(), [node] {
            HTML::ErrorEventInit event_init;
            event_init.error = JS::js_undefined();
            node->dispatch_event(HTML::ErrorEvent::create(node->realm(), HTML::EventNames::processorerror, event_init));
        }));
    }
}

void WebAudioClientRegistry::handle_worklet_processor_registration(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation)
{
    ASSERT_CONTROL_THREAD();

    auto client_id = client_id_for_session(session_id);
    if (!client_id.has_value())
        return;

    GC::Ptr<BaseAudioContext> target_context;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        auto it = m_clients.find(client_id.value());
        if (it == m_clients.end())
            return;
        it->value.last_registration_generation = max(it->value.last_registration_generation, generation);
        it->value.registered_processor_descriptors.set(name, descriptors);
        it->value.failed_processor_registrations.remove(name);
        target_context = it->value.state.context.ptr();
    }

    if (!target_context)
        return;

    auto worklet = target_context->audio_worklet();
    if (worklet->has_loaded_any_module() || worklet->has_pending_module_promises())
        worklet->register_processor_from_worker(name, descriptors);
    if (worklet->has_loaded_any_module() || worklet->has_pending_module_promises())
        worklet->set_registration_generation(generation);
}

void WebAudioClientRegistry::handle_worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations)
{
    ASSERT_CONTROL_THREAD();

    auto client_id = client_id_for_session(session_id);
    if (!client_id.has_value())
        return;

    u64 const local_module_id = module_id & 0xffffffffu;

    GC::Ptr<BaseAudioContext> target_context;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        if (auto it = m_clients.find(client_id.value()); it != m_clients.end())
            target_context = it->value.state.context.ptr();
    }

    if (!target_context)
        return;

    auto worklet = target_context->audio_worklet();
    if (!failed_processor_registrations.is_empty()) {
        Threading::MutexLocker locker { m_clients_mutex };
        if (auto it = m_clients.find(client_id.value()); it != m_clients.end()) {
            for (auto const& name : failed_processor_registrations)
                it->value.failed_processor_registrations.set(name);
        }
        worklet->register_failed_processors_from_worker(failed_processor_registrations);
    }
    if (required_generation > worklet->registration_generation()
        && (worklet->has_loaded_any_module() || worklet->has_pending_module_promises())) {
        HashMap<String, Vector<Web::WebAudio::AudioParamDescriptor>> registered_descriptors;
        HashTable<String> failed_registrations;
        u64 last_generation = 0;
        {
            Threading::MutexLocker locker { m_clients_mutex };
            if (auto it = m_clients.find(client_id.value()); it != m_clients.end()) {
                registered_descriptors = it->value.registered_processor_descriptors;
                failed_registrations = it->value.failed_processor_registrations;
                last_generation = it->value.last_registration_generation;
            }
        }

        if (last_generation >= required_generation) {
            for (auto const& entry : registered_descriptors)
                worklet->register_processor_from_worker(entry.key, entry.value);
            if (!failed_registrations.is_empty()) {
                Vector<String> failed_names;
                failed_names.ensure_capacity(failed_registrations.size());
                for (auto const& name : failed_registrations)
                    failed_names.append(name);
                worklet->register_failed_processors_from_worker(failed_names);
            }
            worklet->set_registration_generation(last_generation);
        }
    }
    auto message_copy = error_message;
    auto error_name_copy = error_name;
    target_context->queue_a_media_element_task("audio worklet module evaluated"sv, GC::create_function(target_context->heap(), [worklet, local_module_id, required_generation, success, error_name_copy = move(error_name_copy), message_copy = move(message_copy)] {
        worklet->handle_module_evaluated(local_module_id, required_generation, success, error_name_copy, message_copy);
    }));
}

}
