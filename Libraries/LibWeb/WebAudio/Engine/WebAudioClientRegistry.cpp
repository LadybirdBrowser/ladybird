/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Error.h>
#include <AK/Math.h>
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
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/Engine/WebAudioClientRegistry.h>
#include <LibWeb/WebAudio/Engine/WebAudioWorkerSession.h>

#include <LibWebAudioWorkerClient/WebAudioClient.h>

namespace Web::WebAudio::Render {

static WeakPtr<WebAudioClientRegistry> s_registry_instance;

static void enqueue_worklet_processor_error_task(u64 node_id)
{
    auto registry = s_registry_instance;
    if (!registry)
        return;

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [registry, node_id] {
            if (auto* instance = registry.ptr())
                instance->handle_worklet_processor_error(NodeID { node_id });
        }));
}

static void enqueue_worklet_processor_registration_task(String name, Vector<Web::WebAudio::AudioParamDescriptor> descriptors, u64 generation)
{
    auto registry = s_registry_instance;
    if (!registry)
        return;

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), nullptr,
        GC::create_function(HTML::main_thread_event_loop().heap(), [registry, name = move(name), descriptors = move(descriptors), generation] {
            if (auto* instance = registry.ptr())
                instance->handle_worklet_processor_registration(name, descriptors, generation);
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

    client_ref->on_worklet_processor_error = [](u64, u64 node_id) {
        enqueue_worklet_processor_error_task(node_id);
    };
    client_ref->on_worklet_processor_registered = [](u64, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
        enqueue_worklet_processor_registration_task(name, descriptors, generation);
    };
}

WebAudioClientRegistry::WebAudioClientRegistry()
    : m_worker_session(make<WebAudioWorkerSession>())
    , m_device_format_selected(m_device_format_mutex)
{
    s_registry_instance = make_weak_ptr();

    if (auto client = WebAudioWorkerSession::webaudio_client()) {
        client->on_worklet_processor_error = [](u64, u64 node_id) {
            enqueue_worklet_processor_error_task(node_id);
        };
        client->on_worklet_processor_registered = [](u64, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation) {
            enqueue_worklet_processor_registration_task(name, descriptors, generation);
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
    return m_worker_session->try_copy_analyser_snapshot(*this, client_id, analyser_node_id, fft_size, out_time_domain, out_frequency_db, out_render_quantum_index);
}

bool WebAudioClientRegistry::try_copy_dynamics_compressor_reduction(ClientId client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index)
{
    return m_worker_session->try_copy_dynamics_compressor_reduction(*this, client_id, compressor_node_id, out_reduction_db, out_render_quantum_index);
}

ErrorOr<WebAudioClientRegistry::DeviceFormat> WebAudioClientRegistry::ensure_output_device_open(u32 target_latency_ms, u64 page_id)
{
    if (m_worker_session->has_output_open(*this)) {
        Threading::MutexLocker locker { m_device_format_mutex };
        VERIFY(m_device_format.has_value());
        return m_device_format.value();
    }

    m_page_id = page_id;

    // Allow the current graph (if it contains ScriptProcessor) to raise the session target latency.
    u32 effective_target_latency_ms = max(target_latency_ms, m_min_target_latency_ms_from_graph);

    m_target_latency_ms = clamp(effective_target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);

    TRY(m_worker_session->ensure_output_open(*this, m_target_latency_ms, page_id));

    Threading::MutexLocker locker { m_device_format_mutex };
    while (!m_device_format.has_value())
        m_device_format_selected.wait();
    return m_device_format.value();
}

ErrorOr<void> WebAudioClientRegistry::reopen_output_device(u32 new_target_latency_ms)
{
    if (!m_worker_session->has_output_open(*this))
        return {};

    if (!m_page_id.has_value())
        return Error::from_string_literal("WebAudio: cannot reopen output device without page_id");

    u32 const clamped = clamp(new_target_latency_ms, 10u, Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS);
    if (clamped <= m_target_latency_ms)
        return {};

    m_worker_session->shutdown_output(*this);
    {
        Threading::MutexLocker locker { m_device_format_mutex };
        m_device_format.clear();
    }

    m_target_latency_ms = clamped;
    TRY(m_worker_session->ensure_output_open(*this, m_target_latency_ms, *m_page_id));

    Threading::MutexLocker locker { m_device_format_mutex };
    while (!m_device_format.has_value())
        m_device_format_selected.wait();
    return {};
}

WebAudioClientRegistry::ClientId WebAudioClientRegistry::register_client(BaseAudioContext& context, ControlMessageQueue& control_message_queue, AssociatedTaskQueue& associated_task_queue, Atomic<u64>& current_frame, Atomic<u64>& suspend_state)
{
    ASSERT_CONTROL_THREAD();

    auto client_id = m_next_client_id++;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        m_clients.set(client_id, ClientState {
                                     .context = GC::Weak<BaseAudioContext> { context },
                                     .current_frame = &current_frame,
                                     .suspend_state = &suspend_state,
                                 });
    }

    (void)control_message_queue;
    (void)associated_task_queue;
    return client_id;
}

void WebAudioClientRegistry::unregister_client(ClientId client_id)
{
    ASSERT_CONTROL_THREAD();
    {
        Threading::MutexLocker locker { m_clients_mutex };
        m_clients.remove(client_id);
    }
}
void WebAudioClientRegistry::set_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules, Vector<WorkletNodeDefinition> worklet_node_definitions, Vector<WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();

    // Initial graph install uses the same mechanism as updates.
    update_client_render_graph(client_id, graph_sample_rate, move(encoded_graph), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void WebAudioClientRegistry::update_client_render_graph(ClientId client_id, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules, Vector<WorkletNodeDefinition> worklet_node_definitions, Vector<WorkletPortBinding> worklet_port_bindings)
{
    ASSERT_CONTROL_THREAD();

    m_min_target_latency_ms_from_graph = min_target_latency_ms_for_graph(*resources, graph_sample_rate);
    if (m_min_target_latency_ms_from_graph > m_target_latency_ms)
        (void)reopen_output_device(m_min_target_latency_ms_from_graph);

    m_worker_session->update_client_render_graph(*this, client_id, graph_sample_rate, move(encoded_graph), move(resources), move(worklet_modules), move(worklet_node_definitions), move(worklet_port_bindings));
}

void WebAudioClientRegistry::set_client_suspended(ClientId client_id, bool suspended, u64 generation)
{
    ASSERT_CONTROL_THREAD();
    m_worker_session->set_client_suspended(*this, client_id, suspended, generation);
}

void WebAudioClientRegistry::shutdown()
{
    m_worker_session->shutdown_output(*this);

    {
        Threading::MutexLocker locker { m_device_format_mutex };
        m_device_format.clear();
    }
    {
        Threading::MutexLocker locker { m_clients_mutex };
        m_clients.clear();
    }
}

size_t WebAudioClientRegistry::client_count() const
{
    // Control-thread only.
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker { m_clients_mutex };
    return m_clients.size();
}

void WebAudioClientRegistry::handle_worklet_processor_error(NodeID node_id)
{
    ASSERT_CONTROL_THREAD();

    Vector<GC::Ref<AudioWorkletNode>> nodes_to_notify;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        for (auto const& it : m_clients) {
            auto context = it.value.context.ptr();
            if (!context)
                continue;
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
    }

    for (auto& node : nodes_to_notify) {
        auto& context = *node->context();
        context.queue_a_media_element_task(GC::create_function(context.heap(), [node] {
            HTML::ErrorEventInit event_init;
            event_init.error = JS::js_undefined();
            node->dispatch_event(HTML::ErrorEvent::create(node->realm(), HTML::EventNames::processorerror, event_init));
        }));
    }
}

void WebAudioClientRegistry::handle_worklet_processor_registration(String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation)
{
    ASSERT_CONTROL_THREAD();

    Vector<GC::Ref<BaseAudioContext>> contexts;
    {
        Threading::MutexLocker locker { m_clients_mutex };
        contexts.ensure_capacity(m_clients.size());
        for (auto const& it : m_clients) {
            auto context = it.value.context.ptr();
            if (!context)
                continue;
            contexts.append(*context);
        }
    }

    for (auto& context : contexts) {
        auto worklet = context->audio_worklet();
        auto descriptors_copy = descriptors;
        context->queue_a_media_element_task(GC::create_function(context->heap(), [worklet, name, descriptors_copy = move(descriptors_copy), generation] {
            if (worklet->has_loaded_any_module())
                worklet->register_processor_from_worker(name, descriptors_copy);
            worklet->set_registration_generation(generation);
        }));
    }
}

}
