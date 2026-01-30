/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/OfflineAudioRenderThread.h>
#include <LibWeb/WebAudio/Engine/OfflineAudioRenderTypes.h>
#include <LibWeb/WebAudio/GraphBuilder.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorHost.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorInvoker.h>
#include <LibWeb/WebAudio/Worklet/OfflineAudioWorkletProcessorHost.h>
#include <cmath>

namespace Web::WebAudio {

static u64 s_next_completion_id = 1;
static HashMap<u64, GC::Root<OfflineAudioContext>> s_completion_contexts;

static void register_completion_context(u64 completion_id, OfflineAudioContext& context)
{
    ASSERT_CONTROL_THREAD();
    s_completion_contexts.set(completion_id, GC::Root<OfflineAudioContext> { context });
}

static void unregister_completion_context(u64 completion_id)
{
    ASSERT_CONTROL_THREAD();
    s_completion_contexts.remove(completion_id);
}

void OfflineAudioContext::handle_render_thread_completion(u64 completion_id)
{
    ASSERT_CONTROL_THREAD();
    auto it = s_completion_contexts.find(completion_id);
    if (it == s_completion_contexts.end())
        return;
    auto context_root = move(it->value);
    s_completion_contexts.remove(it);
    if (context_root)
        context_root->handle_offline_render_completion();
}

GC_DEFINE_ALLOCATOR(OfflineAudioContext);

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm, OfflineAudioContextOptions const& context_options)
{
    // AD-HOC: This spec text is currently only mentioned in the constructor overload that takes separate arguments,
    //         but these parameters should be validated for both constructors.
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    TRY(verify_audio_options_inside_nominal_range(realm, context_options.number_of_channels, context_options.length, context_options.sample_rate));

    WebIDL::UnsignedLong render_quantum_size = BaseAudioContext::default_render_quantum_size();
    auto max_render_quantum_size = static_cast<u64>(context_options.sample_rate * 6.0);
    if (context_options.render_size_hint.template has<Bindings::AudioContextRenderSizeCategory>()) {
        auto hint = context_options.render_size_hint.template get<Bindings::AudioContextRenderSizeCategory>();
        if (hint == Bindings::AudioContextRenderSizeCategory::Default || hint == Bindings::AudioContextRenderSizeCategory::Hardware)
            render_quantum_size = BaseAudioContext::default_render_quantum_size();
    } else {
        auto hint = context_options.render_size_hint.template get<WebIDL::UnsignedLong>();
        if (hint == 0 || hint > max_render_quantum_size) {
            auto error = WebIDL::NotSupportedError::create(realm, "renderSizeHint is outside the supported range"_utf16);
            return WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> { error };
        }
        render_quantum_size = hint;
    }

    // Let c be a new OfflineAudioContext object. Initialize c as follows:
    auto c = realm.create<OfflineAudioContext>(realm, context_options.number_of_channels, context_options.length, context_options.sample_rate);
    c->set_render_quantum_size(render_quantum_size);

    // 1. Set the [[control thread state]] for c to "suspended".
    c->set_control_state(Bindings::AudioContextState::Suspended);

    // 2. Set the [[rendering thread state]] for c to "suspended".
    c->set_rendering_state(Bindings::AudioContextState::Suspended);

    // 3. Determine the [[render quantum size]] for this OfflineAudioContext, based on the value of the renderSizeHint.

    // 4. Construct an AudioDestinationNode with its channelCount set to contextOptions.numberOfChannels.
    c->m_destination = TRY(AudioDestinationNode::construct_impl(realm, c, context_options.number_of_channels));

    // FIXME: 5. Let messageChannel be a new MessageChannel.
    // FIXME: 6. Let controlSidePort be the value of messageChannel’s port1 attribute.
    // FIXME: 7. Let renderingSidePort be the value of messageChannel’s port2 attribute.
    // FIXME: 8. Let serializedRenderingSidePort be the result of StructuredSerializeWithTransfer(renderingSidePort, « renderingSidePort »).
    // FIXME: 9. Set this audioWorklet's port to controlSidePort.
    // FIXME: 10. Queue a control message to set the MessagePort on the AudioContextGlobalScope, with serializedRenderingSidePort.

    return c;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext-numberofchannels-length-samplerate
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm,
    WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    return construct_impl(realm, { number_of_channels, length, sample_rate });
}

OfflineAudioContext::~OfflineAudioContext()
{
    if (m_render_thread) {
        m_render_thread->request_abort();
        m_render_thread.clear();
    }

    if (m_render_suspend_notifier) {
        m_render_suspend_notifier->set_enabled(false);
        m_render_suspend_notifier = nullptr;
    }

    if (m_render_suspend_read_fd >= 0) {
        MUST(Core::System::close(m_render_suspend_read_fd));
        m_render_suspend_read_fd = -1;
    }

    if (m_offline_render_completion_id != 0) {
        unregister_completion_context(m_offline_render_completion_id);
        m_offline_render_completion_id = 0;
    }
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::start_rendering()
{
    auto& realm = this->realm();

    // 1. If this’s relevant global object’s associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
    auto const& associated_document = window.associated_document();

    if (!associated_document.is_fully_active()) {
        auto error = WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // AD-HOC: Not in spec explicitly, but this should account for detached iframes too. See /the-offlineaudiocontext-interface/startrendering-after-discard.html WPT.
    auto navigable = window.navigable();
    if (navigable && navigable->has_been_destroyed()) {
        auto error = WebIDL::InvalidStateError::create(realm, "The iframe has been detached"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // 2. If the [[rendering started]] slot on the OfflineAudioContext is true, return a rejected promise with InvalidStateError, and abort these steps.
    if (m_rendering_started) {
        auto error = WebIDL::InvalidStateError::create(realm, "Rendering is already started"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // 3. Set the [[rendering started]] slot of the OfflineAudioContext to true.
    m_rendering_started = true;

    // 4. Let promise be a new promise.
    auto promise = WebIDL::create_promise(realm);

    // 5. Create a new AudioBuffer, with a number of channels, length and sample rate equal respectively to the
    //    numberOfChannels, length and sampleRate values passed to this instance’s constructor in the contextOptions
    //    parameter.
    auto buffer_result = create_buffer(m_number_of_channels, length(), sample_rate());

    // 6. If an exception was thrown during the preceding AudioBuffer constructor call, reject promise with this exception.
    if (buffer_result.is_exception()) {
        return WebIDL::create_rejected_promise_from_exception(realm, buffer_result.exception());
    }

    // Assign this buffer to an internal slot [[rendered buffer]] in the OfflineAudioContext.
    m_rendered_buffer = buffer_result.release_value();

    // 7. Otherwise, in the case that the buffer was successfully constructed, begin offline rendering.
    begin_offline_rendering(promise);

    // 8. Append promise to [[pending promises]].
    m_pending_promises.append(promise);

    // 9. Return promise.
    return promise;
}

u32 OfflineAudioContext::quantum_aligned_frame_index_for_time(double time_seconds) const
{
    if (time_seconds <= 0.0)
        return 0;

    u32 const quantum = static_cast<u32>(render_quantum_size());
    double const frame_index_as_double = time_seconds * sample_rate();
    if (!__builtin_isfinite(frame_index_as_double) || frame_index_as_double <= 0.0)
        return 0;

    if (frame_index_as_double >= static_cast<double>(NumericLimits<u32>::max()))
        return NumericLimits<u32>::max() / quantum * quantum;

    u32 const frame_index = static_cast<u32>(frame_index_as_double);
    return quantum * ((frame_index + quantum - 1) / quantum);
}

void OfflineAudioContext::begin_offline_rendering(GC::Ref<WebIDL::Promise> promise)
{
    // To begin offline rendering, the following steps MUST happen on a rendering thread that is created for the occasion.
    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering

    // 1. Given the current connections and scheduled changes, start rendering length sample-frames of audio into [[rendered buffer]].
    //
    // NOTE: The WebAudio DOM objects are GC-managed and must remain on the control thread.
    // We snapshot the relevant graph state into a render-thread-friendly structure.

    HashMap<NodeID, GC::Ref<AnalyserNode>> analyser_nodes;
    HashMap<NodeID, GC::Ref<AudioWorkletNode>> audio_worklet_nodes;
    HashMap<NodeID, GC::Ref<ScriptProcessorNode>> script_processor_nodes;
    Render::GraphResourceRegistry resources;
    auto graph = Web::WebAudio::build_graph(destination(), sample_rate(), &analyser_nodes, &audio_worklet_nodes, &script_processor_nodes, &resources);

    Vector<u32> suspend_frame_indices;
    suspend_frame_indices.ensure_capacity(m_suspend_requests.size());
    for (auto const& s : m_suspend_requests) {
        suspend_frame_indices.append(s.suspend_frame_index);
    }
    quick_sort(suspend_frame_indices);

    // Store control-thread-only state so the render thread can post a single completion notification.
    m_pending_render_promise = promise;
    m_pending_analyser_nodes = move(analyser_nodes);

    // Set the state attribute of the OfflineAudioContext to running.
    // Fire an event named statechange at the OfflineAudioContext.
    set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Running);

    // If the graph contains nodes that must execute JS on the control thread (AudioWorklet, ScriptProcessor),
    // run offline rendering on the control thread.
    if (!audio_worklet_nodes.is_empty() || !script_processor_nodes.is_empty()) {
        m_worklet_realm_for_rendering = nullptr;
        m_worklet_processor_host.clear();
        m_worklet_processor_instances.clear();
        m_audio_worklet_nodes_for_rendering.clear();

        if (!audio_worklet_nodes.is_empty()) {
            m_worklet_realm_for_rendering = audio_worklet()->worklet_environment_settings_object().realm();

            for (auto const& it : audio_worklet_nodes) {
                if (auto* instance = it.value->processor_instance())
                    m_worklet_processor_instances.set(it.key, GC::Root<JS::Object> { *instance });
                m_audio_worklet_nodes_for_rendering.set(it.key, GC::Root<AudioWorkletNode> { it.value });
            }

            m_worklet_processor_host = make<Render::OfflineAudioWorkletProcessorHost>(*m_worklet_realm_for_rendering, m_worklet_processor_instances);
        }

        m_script_processor_nodes_for_rendering.clear();
        for (auto const& it : script_processor_nodes) {
            m_script_processor_nodes_for_rendering.set(it.key, GC::Root<ScriptProcessorNode> { it.value });
        }

        if (!m_script_processor_nodes_for_rendering.is_empty())
            m_script_processor_host = make<Render::OfflineScriptProcessorHost>(realm(), *this, m_script_processor_nodes_for_rendering);
        else
            m_script_processor_host.clear();

        m_worklet_render_state = make<WorkletRenderState>();
        m_worklet_render_state->resources = move(resources);
        m_worklet_render_state->graph_description = move(graph);
        m_worklet_render_state->render_quantum_size = static_cast<u32>(render_quantum_size());
        m_worklet_render_state->graph = make<RenderGraph>(m_worklet_render_state->graph_description, static_cast<f32>(sample_rate()), m_worklet_render_state->render_quantum_size, &m_worklet_render_state->resources);
        m_worklet_render_state->channel_count = static_cast<u32>(m_number_of_channels);
        m_worklet_render_state->length_in_sample_frames = static_cast<u32>(length());
        m_worklet_render_state->suspend_frame_indices = move(suspend_frame_indices);
        m_worklet_render_state->next_suspend_index = 0;

        schedule_worklet_rendering_step();
        return;
    }

    Render::OfflineAudioRenderRequest request;
    request.graph = move(graph);
    *request.resources = move(resources);
    request.number_of_channels = static_cast<u32>(m_number_of_channels);
    request.length_in_sample_frames = static_cast<u32>(length());
    request.sample_rate = static_cast<f32>(sample_rate());
    request.render_quantum_size = static_cast<u32>(render_quantum_size());
    request.suspend_frame_indices = move(suspend_frame_indices);

    m_offline_render_completion_id = s_next_completion_id++;
    register_completion_context(m_offline_render_completion_id, *this);

    auto completion_dispatcher = [completion_event_loop = m_control_event_loop, completion_id = m_offline_render_completion_id] {
        if (auto strong_loop = completion_event_loop->take()) {
            strong_loop->deferred_invoke([completion_id] {
                handle_render_thread_completion(completion_id);
            });
        }
    };

    int suspend_write_fd = -1;
    if (!request.suspend_frame_indices.is_empty()) {
        auto suspend_pipe_fds = MUST(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
        m_render_suspend_read_fd = suspend_pipe_fds[0];
        suspend_write_fd = suspend_pipe_fds[1];

        m_render_suspend_notifier = Core::Notifier::construct(m_render_suspend_read_fd, Core::Notifier::Type::Read);
        m_render_suspend_notifier->on_activation = [this] {
            u32 frame_index = 0;
            for (;;) {
                auto nread_or_error = Core::System::read(m_render_suspend_read_fd, { &frame_index, sizeof(frame_index) });
                if (nread_or_error.is_error()) {
                    auto code = nread_or_error.error().code();
                    if (code == EAGAIN)
                        break;
                    if (code == EWOULDBLOCK)
                        break;
                    break;
                }
                if (nread_or_error.value() == 0)
                    break;
                if (nread_or_error.value() < sizeof(frame_index))
                    break;
                handle_offline_render_suspended(frame_index);
            }
        };
        m_render_suspend_notifier->set_enabled(true);
    }

    m_render_thread = make<Render::OfflineAudioRenderThread>(move(request), move(completion_dispatcher), suspend_write_fd);
    m_render_thread->start();

    // 4. Once the rendering is complete, queue a media element task to execute the following steps.
    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
}

void OfflineAudioContext::schedule_worklet_rendering_step()
{
    if (!m_worklet_render_state)
        return;

    queue_a_media_element_task("offline worklet render step"sv, GC::create_function(heap(), [this]() {
        render_worklet_step();
    }));
}

void OfflineAudioContext::render_worklet_step()
{
    auto& realm = this->realm();
    HTML::TemporaryExecutionContext temporary_execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    if (!m_worklet_render_state)
        return;

    if (!m_pending_render_promise.has_value())
        return;

    if (is_suspended())
        return;

    auto& state = *m_worklet_render_state;
    if (!state.graph)
        return;
    if (!m_rendered_buffer)
        return;

    // Cache AudioBuffer channel views for this step.
    Vector<GC::Ref<JS::Float32Array>> channel_data;
    channel_data.ensure_capacity(state.channel_count);
    for (u32 ch = 0; ch < state.channel_count; ++ch) {
        auto channel_data_or_exception = m_rendered_buffer->get_channel_data(ch);
        if (channel_data_or_exception.is_exception())
            return;
        channel_data.unchecked_append(channel_data_or_exception.release_value());
    }

    auto previous_role = current_thread_role();
    mark_current_thread_as_offline_thread();

    // Install the host into the render graph context so AudioWorkletRenderNode can call into JS.
    state.graph->process_context().worklet_processor_host = m_worklet_processor_host.ptr();
    state.graph->process_context().script_processor_host = m_script_processor_host.ptr();

    u32 const quantum_size = state.render_quantum_size > 0 ? state.render_quantum_size : static_cast<u32>(render_quantum_size());
    u32 const length_in_frames = state.length_in_sample_frames;
    u32& frame_index = state.frame_index;

    // Avoid blocking the event loop for large offline renders.
    u32 const max_quanta_per_step = 64;
    u32 quanta_processed = 0;

    auto dispatch_worklet_processor_errors = [this]() {
        if (!m_worklet_processor_host)
            return;

        auto* offline_host = static_cast<Render::OfflineAudioWorkletProcessorHost*>(m_worklet_processor_host.ptr());
        if (!offline_host->has_errors())
            return;

        auto errors = offline_host->take_errors();
        for (auto& entry : errors) {
            auto node_it = m_audio_worklet_nodes_for_rendering.find(entry.node_id);
            if (node_it == m_audio_worklet_nodes_for_rendering.end())
                continue;

            auto* node = node_it->value.ptr();
            if (!node)
                continue;

            queue_a_media_element_task("audio worklet processorerror fired"sv, GC::create_function(heap(), [node, error_value = entry.error]() {
                HTML::ErrorEventInit event_init;
                event_init.error = error_value;
                node->dispatch_event(HTML::ErrorEvent::create(node->realm(), HTML::EventNames::processorerror, event_init));
            }));
        }
    };

    while (frame_index < length_in_frames && quanta_processed < max_quanta_per_step) {
        // Handle scheduled suspend boundaries.
        if (state.next_suspend_index < state.suspend_frame_indices.size()
            && frame_index >= state.suspend_frame_indices[state.next_suspend_index]) {
            u32 const suspend_frame = state.suspend_frame_indices[state.next_suspend_index];
            ++state.next_suspend_index;

            if (m_worklet_realm_for_rendering) {
                auto& global_scope = as<AudioWorkletGlobalScope>(m_worklet_realm_for_rendering->global_object());
                global_scope.set_current_frame(suspend_frame);
                global_scope.set_sample_rate(sample_rate());
            }

            current_thread_role() = previous_role;
            set_current_frame(suspend_frame);
            handle_offline_render_suspended(suspend_frame);
            return;
        }

        state.graph->begin_new_quantum(frame_index);

        // Graph rebuilds can swap the executor; re-apply render-thread hosts each quantum.
        state.graph->process_context().worklet_processor_host = m_worklet_processor_host.ptr();
        state.graph->process_context().script_processor_host = m_script_processor_host.ptr();

        Render::AudioBus const& destination_bus = state.graph->render_destination_for_current_quantum();
        state.graph->render_analysers_for_current_quantum();

        u32 const frames_this_quantum = min(quantum_size, length_in_frames - frame_index);
        for (u32 out_channel = 0; out_channel < state.channel_count; ++out_channel) {
            auto span = channel_data[out_channel]->data();
            auto bus_channel0 = destination_bus.channel(min(out_channel, destination_bus.channel_count() - 1));
            for (u32 i = 0; i < frames_this_quantum; ++i)
                span[frame_index + i] = bus_channel0[i];
        }

        frame_index += frames_this_quantum;
        ++quanta_processed;

        dispatch_worklet_processor_errors();
    }

    current_thread_role() = previous_role;

    if (frame_index < length_in_frames) {
        dispatch_worklet_processor_errors();
        schedule_worklet_rendering_step();
        return;
    }

    // Rendering complete.
    size_t const rendered_frames = length();
    size_t const processed_frames = quantum_size == 0
        ? rendered_frames
        : ((rendered_frames + quantum_size - 1) / quantum_size) * quantum_size;
    set_current_frame(processed_frames);
    if (m_worklet_realm_for_rendering) {
        auto& global_scope = as<AudioWorkletGlobalScope>(m_worklet_realm_for_rendering->global_object());
        global_scope.set_current_frame(static_cast<u64>(processed_frames));
        global_scope.set_sample_rate(sample_rate());
    }

    dispatch_scheduled_source_ends(processed_frames);

    dispatch_scheduled_source_ends(processed_frames);

    auto promise = m_pending_render_promise.release_value();
    m_worklet_render_state.clear();
    m_worklet_processor_host.clear();
    m_worklet_processor_instances.clear();
    m_audio_worklet_nodes_for_rendering.clear();
    m_script_processor_host.clear();
    m_script_processor_nodes_for_rendering.clear();

    auto worklet_completion_label = MUST(String::formatted("offline render completion fired render_thread=0 pending_promise=1 worklet=1"));
    queue_a_media_element_task(worklet_completion_label, GC::create_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        set_rendering_state(Bindings::AudioContextState::Closed);
        set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Closed);

        resolve_promise_and_remove_from_pending(promise, this->m_rendered_buffer);

        queue_a_media_element_task("offline audio completion event fired"sv, GC::create_function(heap(), [this]() {
            auto event_init = OfflineAudioCompletionEventInit {
                {
                    .bubbles = false,
                    .cancelable = false,
                    .composed = false,
                },
                this->m_rendered_buffer,
            };
            auto event = MUST(OfflineAudioCompletionEvent::construct_impl(this->realm(), HTML::EventNames::complete, event_init));
            this->dispatch_event(event);
        }));
    }));
}

void OfflineAudioContext::handle_offline_render_completion()
{
    auto& realm = this->realm();

    WA_DBGLN("[WebAudio] offline render completion fired render_thread={} pending_promise={}", m_render_thread ? 1 : 0, m_pending_render_promise.has_value() ? 1 : 0);

    if (m_offline_render_completion_id != 0)
        m_offline_render_completion_id = 0;

    // The completion notifier can fire outside any JS execution context. We need an active execution context
    // for queuing media element tasks via BaseAudioContext::queue_a_media_element_task().
    HTML::TemporaryExecutionContext temporary_execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    if (!m_render_thread)
        return;
    if (!m_render_thread->is_finished())
        return;

    if (!m_pending_render_promise.has_value())
        return;

    GC::Ref<WebIDL::Promise> promise = m_pending_render_promise.release_value();
    HashMap<NodeID, GC::Ref<AnalyserNode>> analyser_nodes = move(m_pending_analyser_nodes);
    m_pending_analyser_nodes.clear();

    Optional<Render::OfflineAudioRenderResult> render_result = m_render_thread->take_result();
    m_render_thread.clear();

    if (!render_result.has_value()) {
        // Aborted render: reject the promise.
        queue_a_media_element_task("offline render aborted"sv, GC::create_function(heap(), [promise, self = GC::Ref { *this }] {
            auto& realm = self->realm();
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::reject_promise(realm, promise, WebIDL::AbortError::create(realm, "Offline rendering was aborted"_utf16));
            self->take_pending_promise(promise);
        }));
        return;
    }

    Render::OfflineAudioRenderResult result = render_result.release_value();

    // Copy rendered audio into [[rendered buffer]].
    if (m_rendered_buffer) {
        u32 const channels_to_copy = min(static_cast<u32>(m_rendered_buffer->number_of_channels()), static_cast<u32>(result.rendered_channels.size()));
        for (u32 channel_index = 0; channel_index < channels_to_copy; ++channel_index) {
            auto channel_data_or_exception = m_rendered_buffer->get_channel_data(channel_index);
            if (channel_data_or_exception.is_exception())
                continue;
            GC::Ref<JS::Float32Array> channel_data = channel_data_or_exception.release_value();
            auto span = channel_data->data();
            Vector<f32> const& rendered = result.rendered_channels[channel_index];
            size_t const samples_to_copy = min(span.size(), rendered.size());
            for (size_t i = 0; i < samples_to_copy; ++i)
                span[i] = rendered[i];
        }
    }

    // Advance currentTime to the end of the rendered buffer. Offline rendering is performed in
    // fixed-sized render quanta, and currentTime is advanced per-quantum.
    // See: wpt/webaudio/the-audio-api/the-offlineaudiocontext-interface/current-time-block-size.html
    size_t const quantum_size = render_quantum_size();
    size_t const rendered_frames = length();
    size_t const processed_frames = quantum_size == 0
        ? rendered_frames
        : ((rendered_frames + quantum_size - 1) / quantum_size) * quantum_size;
    set_current_frame(processed_frames);
    if (m_worklet_realm_for_rendering) {
        auto& global_scope = as<AudioWorkletGlobalScope>(m_worklet_realm_for_rendering->global_object());
        global_scope.set_current_frame(static_cast<u64>(processed_frames));
        global_scope.set_sample_rate(sample_rate());
    }

    dispatch_scheduled_source_ends(processed_frames);

    // Feed analyser nodes with render-thread computed data.
    // Set the cache-key to match what AnalyserNode will compute from currentTime after rendering.
    size_t const final_render_quantum_index = quantum_size == 0 ? 0 : processed_frames / quantum_size;
    for (auto const& it : result.analyser_time_domain_data) {
        NodeID const analyser_id = it.key;
        auto analyser_it = analyser_nodes.find(analyser_id);
        if (analyser_it == analyser_nodes.end())
            continue;

        auto frequency_it = result.analyser_frequency_data_db.find(analyser_id);
        if (frequency_it == result.analyser_frequency_data_db.end()) {
            Vector<f32> stub_frequency;
            stub_frequency.resize(analyser_it->value->frequency_bin_count());
            for (size_t i = 0; i < stub_frequency.size(); ++i)
                stub_frequency[i] = -AK::Infinity<f32>;
            analyser_it->value->set_analysis_data_from_rendering({}, it.value.span(), stub_frequency.span(), final_render_quantum_index);
            continue;
        }

        analyser_it->value->set_analysis_data_from_rendering({}, it.value.span(), frequency_it->value.span(), final_render_quantum_index);
    }

    auto completion_label = MUST(String::formatted("offline render completion fired render_thread={} pending_promise={}", m_render_thread ? 1 : 0, m_pending_render_promise.has_value() ? 1 : 0));
    queue_a_media_element_task(completion_label, GC::create_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // After rendering completes, OfflineAudioContext transitions to "closed".
        // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
        set_rendering_state(Bindings::AudioContextState::Closed);
        set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Closed);

        // 4.1 Resolve the promise created by startRendering() with [[rendered buffer]].
        resolve_promise_and_remove_from_pending(promise, this->m_rendered_buffer);

        // 4.2: Queue a media element task to fire an event named complete at the OfflineAudioContext using OfflineAudioCompletionEvent
        //      whose renderedBuffer property is set to [[rendered buffer]].
        queue_a_media_element_task("offline audio completion event fired"sv, GC::create_function(heap(), [this]() {
            auto event_init = OfflineAudioCompletionEventInit {
                {
                    .bubbles = false,
                    .cancelable = false,
                    .composed = false,
                },
                this->m_rendered_buffer,
            };
            auto event = MUST(OfflineAudioCompletionEvent::construct_impl(this->realm(), HTML::EventNames::complete, event_init));
            this->dispatch_event(event);
        }));
    }));
}

void OfflineAudioContext::handle_offline_render_suspended(u32 frame_index)
{
    auto& realm = this->realm();

    // The suspend notifier can fire outside any JS execution context.
    HTML::TemporaryExecutionContext temporary_execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    set_current_frame(frame_index);
    // Set the state attribute of the OfflineAudioContext to suspended.
    // Fire an event named statechange at the OfflineAudioContext.
    set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Suspended);

    WA_DBGLN("[WebAudio] offline suspended at frame={} worklet_state={} render_thread={}", frame_index, m_worklet_render_state ? 1 : 0, m_render_thread ? 1 : 0);

    // Make analyser state observable at the suspension boundary.
    if (m_render_thread) {
        auto snapshot = m_render_thread->take_analyser_snapshot(frame_index);
        if (snapshot.has_value()) {
            for (auto const& it : snapshot->analyser_time_domain_data) {
                NodeID const analyser_id = it.key;
                auto analyser_it = m_pending_analyser_nodes.find(analyser_id);
                if (analyser_it == m_pending_analyser_nodes.end())
                    continue;

                auto frequency_it = snapshot->analyser_frequency_data_db.find(analyser_id);
                if (frequency_it == snapshot->analyser_frequency_data_db.end()) {
                    Vector<f32> stub_frequency;
                    stub_frequency.resize(analyser_it->value->frequency_bin_count());
                    for (size_t i = 0; i < stub_frequency.size(); ++i)
                        stub_frequency[i] = -AK::Infinity<f32>;
                    analyser_it->value->set_analysis_data_from_rendering({}, it.value.span(), stub_frequency.span(), snapshot->render_quantum_index);
                    continue;
                }

                analyser_it->value->set_analysis_data_from_rendering({}, it.value.span(), frequency_it->value.span(), snapshot->render_quantum_index);
            }
        }
    }

    // Resolve the corresponding suspend() promise.
    for (auto& request : m_suspend_requests) {
        if (request.resolved)
            continue;
        if (request.suspend_frame_index != frame_index)
            continue;

        request.resolved = true;
        GC::Ref<WebIDL::Promise> promise = request.promise;

        // NOTE: The suspend notifier can fire outside any JS execution context, so we created a
        // TemporaryExecutionContext at the start of this method.
        // Resolve the promise immediately here to avoid depending on queued tasks being serviced
        // promptly (which can delay resume() and make the UI appear hung).
        resolve_promise_and_remove_from_pending(promise, JS::js_undefined());
        break;
    }
}

void OfflineAudioContext::dispatch_scheduled_source_end_event(AudioScheduledSourceNode& node)
{
    auto node_ref = GC::Ref<AudioScheduledSourceNode> { node };
    queue_a_media_element_task("audio scheduled source ended"sv, GC::create_function(heap(), [node_ref] {
        auto& realm = node_ref->realm();
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        node_ref->dispatch_event(DOM::Event::create(realm, HTML::EventNames::ended));
    }));
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::resume()
{
    auto& realm = this->realm();

    auto promise = WebIDL::create_promise(realm);

    if (!m_rendering_started || !is_suspended() || (!m_render_thread && !m_worklet_render_state)) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "OfflineAudioContext is not suspended"_utf16));
        return promise;
    }

    m_pending_promises.append(promise);

    WA_DBGLN("[WebAudio] offline resume requested (worklet_state={} render_thread={})", m_worklet_render_state ? 1 : 0, m_render_thread ? 1 : 0);

    // Ensure any pending graph mutations are applied before we snapshot.
    flush_pending_audio_graph_update();

    if (m_worklet_render_state) {
        // Snapshot the graph again at the suspension boundary so automation events scheduled
        // during suspend() (and other control-thread mutations) can take effect.
        Render::GraphResourceRegistry updated_resources;
        auto updated_graph = Web::WebAudio::build_graph(destination(), sample_rate(), nullptr, nullptr, nullptr, &updated_resources);

        m_worklet_render_state->resources = move(updated_resources);
        m_worklet_render_state->graph_description = updated_graph;
        if (m_worklet_render_state->graph)
            m_worklet_render_state->graph->enqueue_update(updated_graph);

        // Set the state attribute of the OfflineAudioContext to running.
        // Fire an event named statechange at the OfflineAudioContext.
        set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Running);
        schedule_worklet_rendering_step();
        resolve_promise_and_remove_from_pending(promise, JS::js_undefined());
        return promise;
    }

    // Snapshot the graph again at the suspension boundary so graph mutations (disconnect/connect) can take effect.
    // FIXME: The render thread should ideally always apply this without resetting node DSP state.
    Render::GraphResourceRegistry updated_resources;
    auto updated_graph = Web::WebAudio::build_graph(destination(), sample_rate(), nullptr, nullptr, nullptr, &updated_resources);
    Render::OfflineAudioGraphUpdate graph_update;
    graph_update.graph = move(updated_graph);
    graph_update.resources = move(updated_resources);
    m_render_thread->request_resume(Optional<Render::OfflineAudioGraphUpdate> { move(graph_update) });

    // Set the state attribute of the OfflineAudioContext to running.
    // Fire an event named statechange at the OfflineAudioContext.
    set_control_state_and_dispatch_statechange(Bindings::AudioContextState::Running);

    resolve_promise_and_remove_from_pending(promise, JS::js_undefined());
    return promise;
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::suspend(double suspend_time)
{
    auto& realm = this->realm();

    auto promise = WebIDL::create_promise(realm);

    if (m_rendering_started) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Cannot call suspend() after startRendering()"_utf16));
        return promise;
    }

    if (!__builtin_isfinite(suspend_time) || suspend_time < 0.0) {
        auto error = JS::RangeError::create(realm, "suspendTime must be a finite non-negative number"_string);
        WebIDL::reject_promise(realm, promise, error);
        return promise;
    }

    double const duration_seconds = static_cast<double>(length()) / static_cast<double>(sample_rate());
    if (suspend_time > duration_seconds) {
        auto error = JS::RangeError::create(realm, "suspendTime exceeds render duration"_string);
        WebIDL::reject_promise(realm, promise, error);
        return promise;
    }

    m_pending_promises.append(promise);

    u32 const frame_index = quantum_aligned_frame_index_for_time(suspend_time);
    m_suspend_requests.append(SuspendRequest {
        .suspend_time_seconds = suspend_time,
        .suspend_frame_index = frame_index,
        .promise = promise,
        .resolved = false,
    });

    return promise;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-length
WebIDL::UnsignedLong OfflineAudioContext::length() const
{
    // The size of the buffer in sample-frames. This is the same as the value of the length parameter for the constructor.
    return m_length;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-oncomplete
GC::Ptr<WebIDL::CallbackType> OfflineAudioContext::oncomplete()
{
    return event_handler_attribute(HTML::EventNames::complete);
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-oncomplete
void OfflineAudioContext::set_oncomplete(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::complete, value);
}

OfflineAudioContext::OfflineAudioContext(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
    : BaseAudioContext(realm, sample_rate)
    , m_length(length)
    , m_number_of_channels(number_of_channels)
    , m_control_event_loop(Core::EventLoop::current_weak())
{
}

void OfflineAudioContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OfflineAudioContext);
    Base::initialize(realm);
}

void OfflineAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);
    visitor.visit(m_worklet_realm_for_rendering);

    for (auto const& it : m_worklet_processor_instances)
        visitor.visit(it.value.cell());

    for (auto const& it : m_audio_worklet_nodes_for_rendering)
        visitor.visit(it.value.cell());

    for (auto const& it : m_script_processor_nodes_for_rendering)
        visitor.visit(it.value.cell());

    if (m_pending_render_promise.has_value())
        visitor.visit(m_pending_render_promise.value());
    for (auto const& it : m_pending_analyser_nodes)
        visitor.visit(it.value);
}

}
