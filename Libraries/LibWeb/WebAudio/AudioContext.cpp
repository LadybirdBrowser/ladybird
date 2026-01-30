/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ScopeGuard.h>
#include <AK/StringView.h>
#include <LibAudioServerClient/Client.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/AudioContextPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/AudioPlaybackStats.h>
#include <LibWeb/WebAudio/AudioSinkInfo.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/Engine/SharedMemory.h>
#include <LibWeb/WebAudio/EngineController.h>
#include <LibWeb/WebAudio/GraphBuilder.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorHost.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

static bool sink_id_matches_current(Variant<String, AudioSinkOptions> const& requested, Variant<String, GC::Ref<AudioSinkInfo>> const& current)
{
    if (requested.has<String>()) {
        if (!current.has<String>())
            return false;
        return requested.get<String>() == current.get<String>();
    }

    if (!current.has<GC::Ref<AudioSinkInfo>>())
        return false;

    return requested.get<AudioSinkOptions>().type == current.get<GC::Ref<AudioSinkInfo>>()->type();
}

static bool validate_sink_identifier(Variant<String, AudioSinkOptions> const& sink_id)
{
    // FIXME: Implement speaker-selection permission and device enumeration validation.
    if (sink_id.has<AudioSinkOptions>())
        return sink_id.get<AudioSinkOptions>().type == Bindings::AudioSinkType::None;
    return true;
}

GC_DEFINE_ALLOCATOR(AudioContext);

AudioContext::AudioContext(JS::Realm& realm)
    : BaseAudioContext(realm)
    , m_control_event_loop(Core::EventLoop::current_weak())
{
    ASSERT_CONTROL_THREAD();
}

bool AudioContext::try_copy_realtime_analyser_data(NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index) const
{
    if (fft_size == 0)
        return false;
    if (out_time_domain.size() != fft_size)
        return false;
    if (!out_frequency_db.is_empty() && out_frequency_db.size() != (fft_size / 2))
        return false;

    if (!m_audio_service_client_id.has_value())
        return false;
    return EngineController::the().try_copy_analyser_snapshot(*m_audio_service_client_id, analyser_node_id, fft_size, out_time_domain, out_frequency_db, out_render_quantum_index);
}

bool AudioContext::try_copy_realtime_dynamics_compressor_reduction(NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index) const
{
    if (!m_audio_service_client_id.has_value())
        return false;
    return EngineController::the().try_copy_dynamics_compressor_reduction(*m_audio_service_client_id, compressor_node_id, out_reduction_db, out_render_quantum_index);
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-audiocontext
WebIDL::ExceptionOr<GC::Ref<AudioContext>> AudioContext::construct_impl(JS::Realm& realm, Optional<AudioContextOptions> const& context_options)
{
    // If the current settings object’s responsible document is NOT fully active, throw an InvalidStateError and abort these steps.
    auto& settings = HTML::current_principal_settings_object();

    // FIXME: Not all settings objects currently return a responsible document.
    //        Therefore we only fail this check if responsible document is not null.
    if (!settings.responsible_document() || !settings.responsible_document()->is_fully_active()) {
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);
    }

    // AD-HOC: The spec doesn't currently require the sample rate to be validated here,
    //         but other browsers do perform a check and there is a WPT test that expects this.
    if (context_options.has_value() && context_options->sample_rate.has_value())
        TRY(verify_audio_options_inside_nominal_range(realm, *context_options->sample_rate));

    // 1. Let context be a new AudioContext object.
    auto context = realm.create<AudioContext>(realm);

    // Root the context for the duration of construction. The construction path can allocate
    // heavily enough to trigger GC.
    GC::Root<AudioContext> context_root { context };

    // Register the context with the relevant global so navigation/unload cleanup can
    // forcibly close it and release resources promptly.
    auto& relevant_global = as<HTML::WindowOrWorkerGlobalScopeMixin>(HTML::relevant_global_object(*context));
    relevant_global.register_audio_context({}, context);
    ArmedScopeGuard unregister_on_error { [&] { relevant_global.unregister_audio_context({}, *context); } };

    // Page visibility can suspend or resume an AudioContext.
    // See: https://webaudio.github.io/web-audio-api/#dom-audiocontext-suspend
    // See: https://webaudio.github.io/web-audio-api/#dom-audiocontext-resume
    if (auto* window = as_if<HTML::Window>(relevant_global); window && !HTML::Window::in_test_mode()) {
        context->m_document_observer = realm.create<DOM::DocumentObserver>(realm, window->associated_document());
        auto context_ref = GC::Ref<AudioContext> { *context };
        context->m_document_observer->set_document_visibility_state_observer([context_ref](HTML::VisibilityState visibility_state) {
            bool const was_user = context_ref->m_suspended_by_user;
            if (visibility_state == HTML::VisibilityState::Hidden) {
                if (context_ref->is_running() && !context_ref->m_suspended_by_visibility) {
                    HTML::TemporaryExecutionContext execution_context(context_ref->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                    context_ref->m_suspended_by_visibility = true;
                    (void)context_ref->suspend();
                    context_ref->m_suspended_by_user = was_user;
                }
                return;
            }

            if (context_ref->m_suspended_by_visibility && context_ref->state() != Bindings::AudioContextState::Closed) {
                HTML::TemporaryExecutionContext execution_context(context_ref->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                context_ref->m_suspended_by_visibility = false;
                (void)context_ref->resume();
                context_ref->m_suspended_by_user = was_user;
            }
        });
    }

    context->m_destination = TRY(AudioDestinationNode::construct_impl(realm, context));

    // 2. Set a [[control thread state]] to suspended on context.
    context->set_control_state(Bindings::AudioContextState::Suspended);

    // 3. Set a [[rendering thread state]] to suspended on context.
    context->set_rendering_state(Bindings::AudioContextState::Suspended);

    // FIXME: 4. Let messageChannel be a new MessageChannel.
    // FIXME: 5. Let controlSidePort be the value of messageChannel’s port1 attribute.
    // FIXME: 6. Let renderingSidePort be the value of messageChannel’s port2 attribute.
    // FIXME: 7. Let serializedRenderingSidePort be the result of StructuredSerializeWithTransfer(renderingSidePort, « renderingSidePort »).
    // FIXME: 8. Set this audioWorklet's port to controlSidePort.
    // FIXME: 9. Queue a control message to set the MessagePort on the AudioContextGlobalScope, with serializedRenderingSidePort.
    // AD-HOC: Steps 4-9 are spread across multiple TUs due to the dedicated WebAudioWorker process

    // 10. If contextOptions is given, apply the options:
    if (context_options.has_value()) {
        // https://webaudio.github.io/web-audio-api/#AudioContext-constructors
        // 1. If sinkId is specified, let sinkId be the value of contextOptions.sinkId and run the following substeps:
        if (context_options->sink_id.has_value()) {
            auto const& sink_id = context_options->sink_id.value();

            // 1. If both sinkId and [[sink ID]] are a type of DOMString, and they are equal to each other, abort these substeps.
            if (sink_id.has<String>() && context->m_sink_id.has<String>() && sink_id.get<String>() == context->m_sink_id.get<String>()) {
            }
            // 2. If sinkId is a type of AudioSinkOptions and [[sink ID]] is a type of AudioSinkInfo, and type in sinkId and type in [[sink ID]] are equal, abort these substeps.
            else if (sink_id.has<AudioSinkOptions>() && context->m_sink_id.has<GC::Ref<AudioSinkInfo>>() && sink_id.get<AudioSinkOptions>().type == context->m_sink_id.get<GC::Ref<AudioSinkInfo>>()->type()) {
            }
            // 3. If sinkId is a type of DOMString, set [[sink ID at construction]] to sinkId and abort these substeps.
            else if (sink_id.has<String>()) {
                context->m_sink_id_at_construction = sink_id.get<String>();
            }
            // 4. If sinkId is a type of AudioSinkOptions, set [[sink ID at construction]] to a new instance of AudioSinkInfo created with the value of type of sinkId.
            else {
                context->m_sink_id_at_construction = AudioSinkInfo::create(realm, sink_id.get<AudioSinkOptions>().type);
            }
        }

        // 2. Set the internal latency of context according to contextOptions.latencyHint, as described in latencyHint.
        constexpr u32 interactive_target_latency_ms = Render::AUDIO_CONTEXT_INTERACTIVE_TARGET_LATENCY_MS;
        constexpr u32 balanced_target_latency_ms = Render::AUDIO_CONTEXT_BALANCED_TARGET_LATENCY_MS;
        constexpr u32 playback_target_latency_ms = Render::AUDIO_CONTEXT_PLAYBACK_TARGET_LATENCY_MS;
        constexpr u32 max_supported_target_latency_ms = Render::AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS;

        auto const& latency_hint = context_options->latency_hint;

        if (latency_hint.has<Bindings::AudioContextLatencyCategory>()) {
            switch (latency_hint.get<Bindings::AudioContextLatencyCategory>()) {
            case Bindings::AudioContextLatencyCategory::Interactive:
                context->m_target_latency_ms = interactive_target_latency_ms;
                break;
            case Bindings::AudioContextLatencyCategory::Balanced:
                context->m_target_latency_ms = balanced_target_latency_ms;
                break;
            case Bindings::AudioContextLatencyCategory::Playback:
                context->m_target_latency_ms = playback_target_latency_ms;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        } else {
            auto latency_hint_seconds = latency_hint.get<double>();
            if (!isfinite(latency_hint_seconds) || latency_hint_seconds < 0.0)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid latencyHint"_string };

            if (latency_hint_seconds <= static_cast<double>(interactive_target_latency_ms) / 1000.0) {
                context->m_target_latency_ms = interactive_target_latency_ms;
            } else {
                auto latency_ms = static_cast<u32>(lround(latency_hint_seconds * 1000.0));
                context->m_target_latency_ms = clamp(latency_ms, interactive_target_latency_ms, max_supported_target_latency_ms);
            }
        }

        // 3: If contextOptions.sampleRate is specified, set the sampleRate of context to this value.
        if (context_options->sample_rate.has_value()) {
            context->set_sample_rate(context_options->sample_rate.value());
            context->m_sample_rate_is_explicit = true;
        }
        // Otherwise, follow these substeps:
        else {
            // FIXME: 1. If sinkId is the empty string or a type of AudioSinkOptions, use the sample rate of the default output device. Abort these substeps.
            // FIXME: 2. If sinkId is a DOMString, use the sample rate of the output device identified by sinkId. Abort these substeps.
            // If contextOptions.sampleRate differs from the sample rate of the output device, the user agent MUST resample the audio output to match the sample rate of the output device.
            float default_sample_rate = 44100;

            // Ask AudioServer for the output device format now so AudioContext.sampleRate matches from construction.
            if (auto client = AudioServerClient::Client::default_client(); client) {
                auto device_format_or_error = client->get_output_device_format();
                if (!device_format_or_error.is_error())
                    default_sample_rate = static_cast<float>(device_format_or_error.value().sample_rate);
            }

            context->set_sample_rate(default_sample_rate);
            context->m_sample_rate_is_explicit = false;
        }
    }

    // 13. Set [[playback stats]] to a new instance of AudioPlaybackStats.
    context->m_playback_stats = AudioPlaybackStats::create(realm, *context);

    // 14. Return context.
    unregister_on_error.disarm();

    if (HTML::Window::in_test_mode())
        (void)context->resume(); // AD-HOC: For wpt coverage

    return context;
}

AudioContext::~AudioContext()
{
    if (m_render_thread_state_ack_timer) {
        m_render_thread_state_ack_timer->stop();
        m_render_thread_state_ack_timer = nullptr;
    }
    m_pending_render_thread_state_acks.clear();
    stop_rendering_audio_graph();
}

void AudioContext::ensure_render_thread_state_ack_timer_running()
{
    if (m_render_thread_state_ack_timer)
        return;

    m_render_thread_state_ack_timer = Core::Timer::create_repeating(Render::AUDIO_CONTEXT_RENDER_THREAD_STATE_ACK_POLL_INTERVAL_MS, [this] {
        process_render_thread_state_acks();
    });
    m_render_thread_state_ack_timer->start();
}

void AudioContext::snapshot_render_graph_and_prepare_resources(Render::GraphResourceRegistry& resources, Render::GraphDescription& out_graph_description)
{
    HashMap<NodeID, GC::Ref<ScriptProcessorNode>> script_processor_nodes;
    out_graph_description = build_graph(destination(), sample_rate(), nullptr, nullptr, &script_processor_nodes, &resources);

    m_script_processor_nodes_for_rendering.clear();
    for (auto const& it : script_processor_nodes)
        m_script_processor_nodes_for_rendering.set(it.key, GC::Root<ScriptProcessorNode> { it.value });

    if (!m_script_processor_host)
        m_script_processor_host = make<Render::RealtimeScriptProcessorHost>(realm(), *this, m_control_event_loop, m_script_processor_nodes_for_rendering);
    resources.set_script_processor_host(m_script_processor_host.ptr());

    resources.clear_script_processor_transport_metadata();
    for (auto const& it : out_graph_description.nodes) {
        auto node_id = it.key;
        auto const& node_desc = it.value;
        if (!node_desc.has<Render::ScriptProcessorGraphNode>())
            continue;
        auto const& sp = node_desc.get<Render::ScriptProcessorGraphNode>();
        resources.set_script_processor_transport_metadata(node_id, {
                                                                       .buffer_size = static_cast<u32>(sp.buffer_size),
                                                                       .input_channel_count = static_cast<u32>(sp.input_channel_count),
                                                                       .output_channel_count = static_cast<u32>(sp.output_channel_count),
                                                                   });
    }
}

void AudioContext::on_audio_graph_changed()
{
    if (!m_audio_service_client_id.has_value()) {
        auto worklet = audio_worklet();
        if (!worklet->needs_realtime_worklet_session())
            return;

        u64 page_id = 0;
        if (is<HTML::Window>(HTML::relevant_global_object(*this))) {
            auto const& window = static_cast<HTML::Window const&>(HTML::relevant_global_object(*this));
            page_id = window.page().client().id();
        }

        m_audio_service_client_id = EngineController::the().register_client(*this, control_message_queue(), associated_task_queue(), current_frame_atomic(), render_thread_suspend_state_atomic(), underrun_frames_total_atomic());

        auto device_format_or_error = EngineController::the().ensure_output_device_open(*m_audio_service_client_id, m_target_latency_ms, page_id);
        if (device_format_or_error.is_error()) {
            if (should_log_info())
                WA_DBGLN("[WebAudio] Failed to open output device for worklet graph: {}", device_format_or_error.error());
            EngineController::the().unregister_client(*m_audio_service_client_id);
            m_audio_service_client_id.clear();
            return;
        }

        auto device_format = device_format_or_error.release_value();
        if (!m_sample_rate_is_explicit)
            set_sample_rate(static_cast<float>(device_format.sample_rate));
        m_base_latency = static_cast<double>(m_target_latency_ms) / 1000.0;
    }
    auto resources = make<Render::GraphResourceRegistry>();
    Render::GraphDescription graph_description;
    snapshot_render_graph_and_prepare_resources(*resources, graph_description);

    WA_DBGLN("[WebAudio] on_audio_graph_changed: nodes={} conns={} param_conns={} param_autos={} dest_id={}",
        graph_description.nodes.size(),
        graph_description.connections.size(),
        graph_description.param_connections.size(),
        graph_description.param_automations.size(),
        graph_description.destination_node_id);

    if (should_log_info()) {
        size_t const max_dump = 32;
        size_t const connection_dump_count = graph_description.connections.size() < max_dump ? graph_description.connections.size() : max_dump;
        for (size_t i = 0; i < connection_dump_count; ++i) {
            auto const& c = graph_description.connections[i];
            WA_DBGLN("[WebAudio]   conn[{}]: {}:{} -> {}:{}", i, c.source, c.source_output_index, c.destination, c.destination_input_index);
        }
        if (graph_description.connections.size() > max_dump)
            WA_DBGLN("[WebAudio]   ... ({} more connections)", graph_description.connections.size() - max_dump);

        size_t const param_connection_dump_count = graph_description.param_connections.size() < max_dump ? graph_description.param_connections.size() : max_dump;
        for (size_t i = 0; i < param_connection_dump_count; ++i) {
            auto const& c = graph_description.param_connections[i];
            WA_DBGLN("[WebAudio]   param_conn[{}]: {}:{} -> {}:param{}", i, c.source, c.source_output_index, c.destination, c.destination_param_index);
        }
        if (graph_description.param_connections.size() > max_dump)
            WA_DBGLN("[WebAudio]   ... ({} more param connections)", graph_description.param_connections.size() - max_dump);
    }

    auto encoded_or_error = Render::encode_render_graph_for_media_server(graph_description, sample_rate(), *resources);
    if (encoded_or_error.is_error()) {
        WA_DBGLN("[WebAudio] Failed to encode render graph update: {}", encoded_or_error.error());
        return;
    }

    auto worklet = audio_worklet();
    auto worklet_modules = worklet->loaded_modules();

    // Use the set of AudioWorkletNodes tracked by AudioWorklet, not only nodes
    // reachable from the destination render graph snapshot.
    Vector<Render::WorkletNodeDefinition> worklet_nodes = worklet->realtime_node_definitions();
    Vector<NodeID> worklet_node_ids = worklet->realtime_node_ids();

    // Best-effort: include any worklet nodes that are part of the current render
    // graph snapshot but were not tracked for some reason.
    for (auto const& it : graph_description.nodes) {
        auto node_id = it.key;
        auto const& node_desc = it.value;
        if (!node_desc.has<Render::AudioWorkletGraphNode>())
            continue;

        bool already_tracked = false;
        for (auto tracked_id : worklet_node_ids) {
            if (tracked_id == node_id) {
                already_tracked = true;
                break;
            }
        }
        if (already_tracked)
            continue;

        auto const& aw = node_desc.get<Render::AudioWorkletGraphNode>();
        worklet_node_ids.append(node_id);
        worklet_nodes.append(Render::WorkletNodeDefinition {
            .node_id = node_id,
            .processor_name = aw.processor_name,
            .number_of_inputs = aw.number_of_inputs,
            .number_of_outputs = aw.number_of_outputs,
            .output_channel_count = aw.output_channel_count,
            .channel_count = aw.channel_count,
            .channel_count_mode = aw.channel_count_mode,
            .channel_interpretation = aw.channel_interpretation,
            .parameter_names = aw.parameter_names,
            .parameter_data = {},
            .serialized_processor_options = {},
        });
    }

    worklet->prune_realtime_processor_ports(worklet_node_ids);

    Vector<Render::WorkletPortBinding> worklet_port_bindings;
    auto global_fd = worklet->clone_realtime_global_port_fd();
    worklet_port_bindings.ensure_capacity(worklet_node_ids.size() + (global_fd.has_value() ? 1 : 0));

    if (global_fd.has_value()) {
        worklet_port_bindings.unchecked_append(Render::WorkletPortBinding {
            .node_id = NodeID { 0 },
            .processor_port_fd = global_fd.value(),
        });
    }

    for (auto node_id : worklet_node_ids) {
        auto fd = worklet->clone_realtime_processor_port_fd(node_id);
        if (!fd.has_value())
            continue;
        worklet_port_bindings.unchecked_append(Render::WorkletPortBinding {
            .node_id = node_id,
            .processor_port_fd = fd.value(),
        });
    }

    EngineController::the().update_client_render_graph(*m_audio_service_client_id, sample_rate(), encoded_or_error.release_value(), move(resources), move(worklet_modules), move(worklet_nodes), move(worklet_port_bindings));
}

void AudioContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioContext);
    Base::initialize(realm);
}

void AudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_pending_resume_promises);
    visitor.visit(m_document_observer);
    visitor.visit(m_playback_stats);
    if (m_sink_id.has<GC::Ref<AudioSinkInfo>>())
        visitor.visit(m_sink_id.get<GC::Ref<AudioSinkInfo>>());
    if (m_sink_id_at_construction.has<GC::Ref<AudioSinkInfo>>())
        visitor.visit(m_sink_id_at_construction.get<GC::Ref<AudioSinkInfo>>());
    for (auto& pending : m_pending_render_thread_state_acks)
        visitor.visit(pending.promise);
}

void AudioContext::process_render_thread_state_acks()
{
    ASSERT_CONTROL_THREAD();

    dispatch_scheduled_source_ends(current_frame());

    if (m_pending_render_thread_state_acks.is_empty()) {
        if (!has_pending_scheduled_source_ends()) {
            if (m_render_thread_state_ack_timer) {
                m_render_thread_state_ack_timer->stop();
                m_render_thread_state_ack_timer = nullptr;
            }
            return;
        }
    }

    u64 const suspend_state = render_thread_suspend_state_atomic().load(AK::MemoryOrder::memory_order_acquire);
    bool const is_suspended = Render::decode_webaudio_suspend_state_is_suspended(suspend_state);
    u64 const generation = Render::decode_webaudio_suspend_state_generation(suspend_state);

    auto self = GC::Ref { *this };

    while (!m_pending_render_thread_state_acks.is_empty()) {
        auto const& pending = m_pending_render_thread_state_acks.first();
        if (pending.suspended != is_suspended)
            break;
        if (generation < pending.generation)
            break;

        auto promise = pending.promise;
        bool const target_suspended = pending.suspended;
        m_pending_render_thread_state_acks.take_first();

        queue_a_media_element_task("audio context render state ack"sv, GC::create_function(heap(), [promise, self, target_suspended]() {
            auto& realm = self->realm();
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            bool promise_resolved = false;

            // Resume() needs to resolve any queued resume promises first.
            if (!target_suspended) {
                for (auto const& pending_resume_promise : self->m_pending_resume_promises) {
                    if (!self->take_pending_promise(pending_resume_promise))
                        continue;
                    WebIDL::resolve_promise(realm, pending_resume_promise, JS::js_undefined());
                    if (pending_resume_promise == promise)
                        promise_resolved = true;
                }
                self->m_pending_resume_promises.clear();
            }

            if (!promise_resolved) {
                if (!self->take_pending_promise(promise))
                    return;
                WebIDL::resolve_promise(realm, promise, JS::js_undefined());
            }

            auto const desired_state = target_suspended ? Bindings::AudioContextState::Suspended : Bindings::AudioContextState::Running;
            // Set the state attribute of the AudioContext to the acknowledged value.
            self->set_control_state_and_dispatch_statechange(desired_state);
        }));
    }

    if (m_pending_render_thread_state_acks.is_empty() && !has_pending_scheduled_source_ends() && m_render_thread_state_ack_timer) {
        m_render_thread_state_ack_timer->stop();
        m_render_thread_state_ack_timer = nullptr;
    }
}

void AudioContext::refresh_timing_page_for_stats()
{
    ASSERT_CONTROL_THREAD();
    if (!m_audio_service_client_id.has_value())
        return;
    EngineController::the().refresh_client_timing(m_audio_service_client_id.value());
}

void AudioContext::on_scheduled_source_end_added()
{
    ensure_render_thread_state_ack_timer_running();
}

void AudioContext::set_onsinkchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::sinkchange, event_handler);
}

WebIDL::CallbackType* AudioContext::onsinkchange()
{
    return event_handler_attribute(HTML::EventNames::sinkchange);
}

void AudioContext::set_onerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

WebIDL::CallbackType* AudioContext::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-setsinkid
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::set_sink_id(Variant<String, AudioSinkOptions> const& sink_id)
{
    auto& realm = this->realm();

    // 1. Let sinkId be the method's first argument.
    // 2. If sinkId is equal to [[sink ID]], return a promise, resolve it immediately and abort these steps.
    if (sink_id_matches_current(sink_id, m_sink_id))
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 3. Let validationResult be the return value of sink identifier validation of sinkId.
    bool const validation_result = validate_sink_identifier(sink_id);

    // 4. If validationResult is false, return a promise rejected with a new DOMException whose name is NotAllowedError. Abort these steps.
    if (!validation_result)
        return WebIDL::create_rejected_promise(realm, WebIDL::NotAllowedError::create(realm, "Speaker selection is not allowed"_utf16));

    // 5. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 6. Send a control message with p and sinkId to start processing.
    // FIXME: Need audio output routing in AudioServer.
    auto sink_id_copy = sink_id;
    queue_a_media_element_task("audio context sinkId resolved"sv, GC::create_function(heap(), [self = GC::Ref { *this }, promise, sink_id_copy]() mutable {
        auto& realm = self->realm();
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        if (sink_id_copy.has<String>()) {
            self->m_sink_id = sink_id_copy.get<String>();
        } else {
            auto info = AudioSinkInfo::create(realm, sink_id_copy.get<AudioSinkOptions>().type);
            self->m_sink_id = info;
        }

        WebIDL::resolve_promise(realm, promise, JS::js_undefined());
        self->dispatch_event(DOM::Event::create(realm, HTML::EventNames::sinkchange));
    }));

    // 7. Return p.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-getoutputtimestamp
AudioTimestamp AudioContext::get_output_timestamp()
{
    AudioTimestamp timestamp;
    timestamp.context_time = current_time();
    timestamp.performance_time = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(*this));
    return timestamp;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-resume
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::resume()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. If the [[control thread state]] on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == Bindings::AudioContextState::Closed) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Audio context is already closed."_utf16));
        return promise;
    }

    // 4. Set [[suspended by user]] to true.
    m_suspended_by_user = true;

    // 5. If the context is not allowed to start, append promise to [[pending promises]] and [[pending resume promises]] and abort these steps, returning promise.
    if (!m_allowed_to_start) {
        m_pending_promises.append(promise);
        m_pending_resume_promises.append(promise);
        return promise;
    }

    m_pending_promises.append(promise);
    m_pending_resume_promises.append(promise);

    // 7. Queue a control message to resume the AudioContext.
    // FIXME: 7.1: Attempt to acquire system resources.

    u64 const generation = m_next_suspend_state_generation++;
    queue_control_message(ResumeContext { .generation = generation });

    // 7.2: Set the [[rendering thread state]] on the AudioContext to running.
    set_rendering_state(Bindings::AudioContextState::Running);

    // 7.3: Start rendering the audio graph.
    if (!m_audio_service_client_id.has_value() && !start_rendering_audio_graph()) {
        set_rendering_state(Bindings::AudioContextState::Suspended);
        // 7.4: In case of failure, queue a media element task to execute the following steps:
        queue_a_media_element_task("audio context resume failed"sv, GC::create_function(heap(), [self = GC::Ref { *this }]() {
            auto& realm = self->realm();
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            // 7.4.1: Reject all promises from [[pending resume promises]] in order, then clear [[pending resume promises]].
            for (auto const& promise : self->m_pending_resume_promises) {
                if (!self->take_pending_promise(promise))
                    continue;
                WebIDL::reject_promise(realm, promise, JS::js_null());
            }
            self->m_pending_resume_promises.clear();
        }));
        return promise;
    }

    // Wait for the rendering backend to apply the resume.
    m_pending_render_thread_state_acks.append({
        .promise = promise,
        .generation = generation,
        .suspended = false,
    });

    if (m_audio_service_client_id.has_value())
        EngineController::the().set_client_suspended(*m_audio_service_client_id, false, generation);

    ensure_render_thread_state_ack_timer_running();

    // 8. Return promise.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-suspend
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::suspend()
{
    // https://webaudio.github.io/web-audio-api/#dom-audiocontext-suspend
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. If the [[control thread state]] on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == Bindings::AudioContextState::Closed) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Audio context is already closed."_utf16));
        return promise;
    }

    // 4. Append promise to [[pending promises]].
    m_pending_promises.append(promise);

    // 5. Set [[suspended by user]] to true.
    m_suspended_by_user = true;

    // 7. Queue a control message to suspend the AudioContext.
    // FIXME: 7.1: Attempt to release system resources.

    u64 const generation = m_next_suspend_state_generation++;
    queue_control_message(SuspendContext { .generation = generation });

    // 7.2: Set the [[rendering thread state]] on the AudioContext to suspended.
    set_rendering_state(Bindings::AudioContextState::Suspended);

    // If we're not rendering yet, there's no backend state to wait for.
    if (!m_audio_service_client_id.has_value()) {
        queue_a_media_element_task("audio context suspend resolved"sv, GC::create_function(heap(), [promise, self = GC::Ref { *this }]() {
            auto& realm = self->realm();
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            if (!self->take_pending_promise(promise))
                return;

            WebIDL::resolve_promise(realm, promise, JS::js_undefined());

            // Set the state attribute of the AudioContext to suspended.
            self->set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Suspended);
        }));
        return promise;
    }

    // Wait for the rendering backend to apply the suspend.
    m_pending_render_thread_state_acks.append({
        .promise = promise,
        .generation = generation,
        .suspended = true,
    });
    EngineController::the().set_client_suspended(*m_audio_service_client_id, true, generation);

    ensure_render_thread_state_ack_timer_running();

    // 8. Return promise.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-close
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::close()
{
    // https://webaudio.github.io/web-audio-api/#dom-audiocontext-close
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);

    // 2. If the [[control thread state]] flag on the AudioContext is closed, return a resolved promise.
    // NOTE: WPT/audit.js code often does not attach rejection handlers to close() promises.
    //       For compatibility, treat close() as idempotent.
    if (state() == Bindings::AudioContextState::Closed)
        return WebIDL::create_resolved_promise(realm, JS::js_undefined());

    // 3. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 4. Set the [[control thread state]] flag on the AudioContext to closed.
    set_control_state(Bindings::AudioContextState::Closed);

    // 5. Queue a control message to close the AudioContext.
    // FIXME: 5.1: Attempt to release system resources.

    queue_control_message(CloseContext {});

    // 5.2: Set the [[rendering thread state]] to "suspended".
    set_rendering_state(Bindings::AudioContextState::Suspended);

    // https://webaudio.github.io/web-audio-api/#rendering-thread
    // Closing a context stops audio processing and releases any system audio resources.
    stop_rendering_audio_graph();
    auto& relevant_global = as<HTML::WindowOrWorkerGlobalScopeMixin>(HTML::relevant_global_object(*this));
    relevant_global.unregister_audio_context({}, *this);

    // FIXME: 5.3: If this control message is being run in a reaction to the document being unloaded, abort this algorithm.

    // 5.4: queue a media element task to execute the following steps:
    queue_a_media_element_task("audio context close resolved"sv, GC::create_function(heap(), [promise, self = GC::Ref { *this }]() {
        auto& realm = self->realm();
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 5.4.1: Resolve promise.
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());

        // 5.4.2: If the state attribute of the AudioContext is not already "closed":
        if (self->state() != Bindings::AudioContextState::Closed) {
            // 5.4.2.1: Set the state attribute of the AudioContext to "closed".
            self->set_control_state(Bindings::AudioContextState::Closed);
        }

        // 5.4.2.2: queue a media element task to fire an event named statechange at the AudioContext.
        // FIXME: Attempting to queue another task in here causes an assertion fail at Vector.h:148
        self->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
    }));

    // 6. Return promise
    return promise;
}

void AudioContext::forcibly_close()
{
    if (state() == Bindings::AudioContextState::Closed)
        return;

    set_control_state(Bindings::AudioContextState::Closed);
    set_rendering_state(Bindings::AudioContextState::Suspended);
    stop_rendering_audio_graph();

    auto& relevant_global = as<HTML::WindowOrWorkerGlobalScopeMixin>(HTML::relevant_global_object(*this));
    relevant_global.unregister_audio_context({}, *this);
}

bool AudioContext::start_rendering_audio_graph()
{
    // https://webaudio.github.io/web-audio-api/#rendering-thread

    u64 page_id = 0;
    if (is<HTML::Window>(HTML::relevant_global_object(*this))) {
        auto const& window = static_cast<HTML::Window const&>(HTML::relevant_global_object(*this));
        page_id = window.page().client().id();
    }

    if (!m_audio_service_client_id.has_value())
        m_audio_service_client_id = EngineController::the().register_client(*this,
            control_message_queue(),
            associated_task_queue(),
            current_frame_atomic(),
            render_thread_suspend_state_atomic(),
            underrun_frames_total_atomic());

    auto device_format_or_error = EngineController::the().ensure_output_device_open(
        *m_audio_service_client_id,
        m_target_latency_ms,
        page_id);
    if (device_format_or_error.is_error()) {
        warnln("WebAudio: failed to open output device: {}", device_format_or_error.error());
        stop_rendering_audio_graph();
        return false;
    }

    auto device_format = device_format_or_error.release_value();
    if (!m_sample_rate_is_explicit)
        set_sample_rate(static_cast<float>(device_format.sample_rate));

    // Approximate base latency from our buffering target.
    m_base_latency = static_cast<double>(m_target_latency_ms) / 1000.0;
    m_output_latency = m_base_latency;

    auto resources = make<Render::GraphResourceRegistry>();
    Render::GraphDescription graph_description;
    snapshot_render_graph_and_prepare_resources(*resources, graph_description);

    auto encoded_or_error = Render::encode_render_graph_for_media_server(graph_description, sample_rate(), *resources);
    if (encoded_or_error.is_error()) {
        WA_DBGLN("[WebAudio] Failed to encode render graph: {}", encoded_or_error.error());
        stop_rendering_audio_graph();
        return false;
    }

    auto worklet_modules = audio_worklet()->loaded_modules();

    Vector<Render::WorkletNodeDefinition> worklet_nodes;
    for (auto const& it : graph_description.nodes) {
        auto node_id = it.key;
        auto const& node_desc = it.value;
        if (!node_desc.has<Render::AudioWorkletGraphNode>())
            continue;
        auto const& aw = node_desc.get<Render::AudioWorkletGraphNode>();
        worklet_nodes.append(Render::WorkletNodeDefinition {
            .node_id = node_id,
            .processor_name = aw.processor_name,
            .number_of_inputs = aw.number_of_inputs,
            .number_of_outputs = aw.number_of_outputs,
            .output_channel_count = aw.output_channel_count,
            .channel_count = aw.channel_count,
            .channel_count_mode = aw.channel_count_mode,
            .channel_interpretation = aw.channel_interpretation,
            .parameter_names = aw.parameter_names,
            .parameter_data = {},
            .serialized_processor_options = {},
        });
    }

    EngineController::the().update_client_render_graph(
        *m_audio_service_client_id,
        sample_rate(),
        encoded_or_error.release_value(),
        move(resources),
        move(worklet_modules),
        move(worklet_nodes));
    return true;
}

void AudioContext::stop_rendering_audio_graph()
{
    if (m_audio_service_client_id.has_value()) {
        if (should_log_info())
            WA_DBGLN("[WebAudio] AudioContext: stop_rendering_audio_graph client_id={} state={} allowed_to_start={}", *m_audio_service_client_id, static_cast<u32>(state()), m_allowed_to_start);
        EngineController::the().unregister_client(*m_audio_service_client_id);
        m_audio_service_client_id.clear();
    }
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-createmediaelementsource
WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> AudioContext::create_media_element_source(GC::Ptr<HTML::HTMLMediaElement> media_element)
{
    if (!media_element)
        return WebIDL::InvalidStateError::create(realm(), "Media element is null"_utf16);
    if (media_element->has_webaudio_audio_tap())
        return WebIDL::InvalidStateError::create(realm(), "Media element is already connected to WebAudio"_utf16);

    MediaElementAudioSourceOptions options;
    options.media_element = media_element;
    return MediaElementAudioSourceNode::create(realm(), *this, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> AudioContext::create_media_stream_source(GC::Ref<MediaCapture::MediaStream> media_stream)
{
    MediaStreamAudioSourceOptions options;
    options.media_stream = media_stream;
    return MediaStreamAudioSourceNode::create(realm(), *this, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamTrackAudioSourceNode>> AudioContext::create_media_stream_track_source(GC::Ref<MediaCapture::MediaStreamTrack> track)
{
    MediaStreamTrackAudioSourceOptions options;
    options.media_stream_track = track;
    return MediaStreamTrackAudioSourceNode::create(realm(), *this, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> AudioContext::create_media_stream_destination(AudioNodeOptions const& options)
{
    return MediaStreamAudioDestinationNode::create(realm(), *this, options);
}

}
