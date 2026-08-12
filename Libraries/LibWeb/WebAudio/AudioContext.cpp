/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/MessageChannel.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioContext);

WebIDL::ExceptionOr<GC::Ref<AudioContext>> AudioContext::create_for_constructor(JS::Object& relevant_global_object, AudioContextOptions const& context_options)
{
    auto& window = HTML::relevant_window(relevant_global_object);
    return create_for_constructor(window, HTML::relevant_settings_object(window), context_options);
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-audiocontext
WebIDL::ExceptionOr<GC::Ref<AudioContext>> AudioContext::create_for_constructor(GC::Ref<DOM::EventTarget> relevant_global_object, HTML::EnvironmentSettingsObject& settings, Optional<AudioContextOptions> const& context_options)
{
    // If the current settings object’s responsible document is NOT fully active, throw an InvalidStateError and abort these steps.
    // FIXME: Not all settings objects currently return a responsible document.
    //        Therefore we only fail this check if responsible document is not null.
    if (!settings.responsible_document() || !settings.responsible_document()->is_fully_active()) {
        return WebIDL::InvalidStateError::create("Document is not fully active"_utf16);
    }

    // AD-HOC: The spec doesn't currently require the sample rate to be validated here,
    //         but other browsers do perform a check and there is a WPT test that expects this.
    if (context_options.has_value() && context_options->sample_rate.has_value())
        TRY(verify_audio_options_inside_nominal_range(*context_options->sample_rate));

    // 1. Let context be a new AudioContext object.
    auto context = GC::Heap::the().allocate<AudioContext>(relevant_global_object);
    context->set_listener(AudioListener::create(context));
    context->m_destination = TRY(AudioDestinationNode::create(context));

    // 2. Set a [[control thread state]] to suspended on context.
    context->set_control_state(AudioContextState::Suspended);

    // 3. Set a [[rendering thread state]] to suspended on context.
    context->set_rendering_state(AudioContextState::Suspended);

    // FIXME: 4. Let messageChannel be a new MessageChannel.
    // FIXME: 5. Let controlSidePort be the value of messageChannel’s port1 attribute.
    // FIXME: 6. Let renderingSidePort be the value of messageChannel’s port2 attribute.
    // FIXME: 7. Let serializedRenderingSidePort be the result of StructuredSerializeWithTransfer(renderingSidePort, « renderingSidePort »).
    // FIXME: 8. Set this audioWorklet's port to controlSidePort.
    // FIXME: 9. Queue a control message to set the MessagePort on the AudioContextGlobalScope, with serializedRenderingSidePort.

    // 10. If contextOptions is given, apply the options:
    if (context_options.has_value()) {
        // 1. If sinkId is specified, let sinkId be the value of contextOptions.sinkId and run the following substeps:

        // 2. Set the internal latency of context according to contextOptions.latencyHint, as described in latencyHint.
        // FIXME: Determine optimal settings for contextOptions.latencyHint.

        // 3: If contextOptions.sampleRate is specified, set the sampleRate of context to this value.
        if (context_options->sample_rate.has_value()) {
            context->set_sample_rate(context_options->sample_rate.value());
        }
        // Otherwise, follow these substeps:
        else {
            // FIXME: 1. If sinkId is the empty string or a type of AudioSinkOptions, use the sample rate of the default output device. Abort these substeps.
            // FIXME: 2. If sinkId is a DOMString, use the sample rate of the output device identified by sinkId. Abort these substeps.
            // If contextOptions.sampleRate differs from the sample rate of the output device, the user agent MUST resample the audio output to match the sample rate of the output device.
            context->set_sample_rate(44100);
        }
    }

    // AD-HOC: Ensure a sample rate is always set, even when no options were passed at all.
    if (context->sample_rate() == 0)
        context->set_sample_rate(44100);

    // 11. If context is allowed to start, send a control message to start processing.
    // FIXME: The context is currently always allowed to start; this should be gated on sticky activation.
    if (context->m_allowed_to_start) {
        // FIXME: 1. Let document be the current settings object's relevant global object's associated Document.

        // 2. Attempt to acquire system resources to use a following audio output device based on [[sink ID]] for
        //    rendering.
        context->m_renderer = Rendering::RealtimeAudioRenderer::create(context->control_message_queue(), context->destination()->node_id(), context->sample_rate(), render_quantum_size());
        context->set_renderer_callbacks();
        // AD-HOC: Headless instances use a null output stream, so their graph follows the same pull path as audio
        // output while discarding the rendered samples.
        if (settings.responsible_document()->page().client().is_headless())
            context->m_renderer->start_rendering_with_null_output();
        else
            context->m_renderer->start_rendering();

        // 2. Set this [[rendering thread state]] to running on the AudioContext.
        context->set_rendering_state(AudioContextState::Running);

        // 3. Queue a media element task to execute the following steps:
        context->queue_a_media_element_task(GC::create_function(GC::Heap::the(), [context]() {
            // 1. Set the state attribute of the AudioContext to "running".
            context->set_control_state(AudioContextState::Running);

            // 2. Fire an event named statechange at the AudioContext.
            context->dispatch_event(context->create_associated_event(HTML::EventNames::statechange));
        }));
    }

    // 12. Return context.
    return context;
}

AudioContext::~AudioContext() = default;

void AudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_pending_resume_promises);
}

void AudioContext::finalize()
{
    Base::finalize();
    if (m_renderer) {
        m_renderer->stop();
        m_renderer->clear_callbacks();
    }
}

void AudioContext::document_became_inactive()
{
    // Release the audio output device and the GC roots held by the renderer's callbacks, so a context belonging to a
    // navigated-away document stops playing and can eventually be collected.
    if (m_renderer) {
        m_renderer->stop();
        m_renderer->clear_callbacks();
    }
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-currenttime
double AudioContext::current_time() const
{
    // This is the time in seconds of the sample frame immediately following the last sample-frame in the block of
    // audio most recently processed by the context's rendering graph.
    if (m_renderer)
        return m_renderer->frames_rendered() / static_cast<double>(sample_rate());
    return Base::current_time();
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-getoutputtimestamp
AudioTimestamp AudioContext::get_output_timestamp()
{
    // If the context's rendering graph has not yet processed a block of audio, then
    // getOutputTimestamp call returns an AudioTimestamp instance with both members
    // containing zero.
    if (!m_renderer || m_renderer->frames_rendered() == 0) {
        return {
            .context_time = 0.0,
            .performance_time = 0.0,
        };
    }
    auto& window = relevant_window();
    // FIXME: performanceTime should be the moment the sample frame at contextTime was rendered by the output device.
    //        We sample performance.now() when getOutputTimestamp() is called instead of caching a timestamp when the
    //        renderer produced that block, so the returned pair does not describe a single output instant.
    return {
        .context_time = m_renderer->output_time_played(),
        .performance_time = window.performance()->now(),
    };
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-resume
WebIDL::ExceptionOr<void> AudioContext::resume(GC::Ref<WebIDL::Promise> promise)
{
    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = relevant_window().associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create("Document is not fully active"_utf16);

    // 3. If the [[control thread state]] on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == AudioContextState::Closed) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Audio context is already closed."_utf16));
        return {};
    }

    // 4. Set [[suspended by user]] to true.
    m_suspended_by_user = true;

    // 5. If the context is not allowed to start, append promise to [[pending promises]] and [[pending resume promises]] and abort these steps, returning promise.
    if (m_allowed_to_start) {
        m_pending_promises.append(promise);
        m_pending_resume_promises.append(promise);
    }

    // 6. Set the [[control thread state]] on the AudioContext to running.
    set_control_state(AudioContextState::Running);

    // 7. Queue a control message to resume the AudioContext.
    // 7.1: Attempt to acquire system resources.
    // NB: The renderer acquires system resources and starts rendering in a single resume, performed in step 7.3.
    if (m_renderer)
        set_renderer_callbacks();

    // 7.2: Set the [[rendering thread state]] on the AudioContext to running.
    set_rendering_state(AudioContextState::Running);

    // 7.3: Start rendering the audio graph.
    if (!start_rendering_audio_graph()) {
        // 7.4: In case of failure, queue a media element task to execute the following steps:
        queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this]() {
            // 7.4.1: Reject all promises from [[pending resume promises]] in order, then clear [[pending resume promises]].
            for (auto const& promise : m_pending_resume_promises) {
                auto& realm = WebIDL::promise_realm(promise);
                HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                WebIDL::reject_promise(promise, JS::js_null());

                // 7.4.2: Additionally, remove those promises from [[pending promises]].
                m_pending_promises.remove_first_matching([&promise](auto& pending_promise) {
                    return pending_promise == promise;
                });
            }
            m_pending_resume_promises.clear();
        }));
    }

    // 7.5: queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(GC::Heap::the(), [promise, this]() {
        // 7.5.1: Resolve all promises from [[pending resume promises]] in order.
        // 7.5.2: Clear [[pending resume promises]]. Additionally, remove those promises from
        //        [[pending promises]].
        for (auto const& pending_resume_promise : m_pending_resume_promises) {
            auto& realm = WebIDL::promise_realm(pending_resume_promise);
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::resolve_promise(pending_resume_promise);
            m_pending_promises.remove_first_matching([&pending_resume_promise](auto& pending_promise) {
                return pending_promise == pending_resume_promise;
            });
        }
        m_pending_resume_promises.clear();

        // 7.5.3: Resolve promise.
        auto& realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        WebIDL::resolve_promise(promise);

        // 7.5.4: If the state attribute of the AudioContext is not already "running":
        if (state() != AudioContextState::Running) {
            // 7.5.4.1: Set the state attribute of the AudioContext to "running".
            set_control_state(AudioContextState::Running);

            // 7.5.4.2: queue a media element task to fire an event named statechange at the AudioContext.
            queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this]() {
                this->dispatch_event(create_associated_event(HTML::EventNames::statechange));
            }));
        }
    }));
    return {};
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-suspend
WebIDL::ExceptionOr<void> AudioContext::suspend(GC::Ref<WebIDL::Promise> promise)
{
    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = relevant_window().associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create("Document is not fully active"_utf16);

    // 3. If the [[control thread state]] on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == AudioContextState::Closed) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Audio context is already closed."_utf16));
        return {};
    }

    // 4. Append promise to [[pending promises]].
    m_pending_promises.append(promise);

    // 5. Set [[suspended by user]] to true.
    m_suspended_by_user = true;

    // 6. Set the [[control thread state]] on the AudioContext to suspended.
    set_control_state(AudioContextState::Suspended);

    // 7. Queue a control message to suspend the AudioContext.
    // 7.1: Attempt to release system resources.
    if (m_renderer) {
        m_renderer->suspend();
        m_renderer->clear_callbacks();
    }

    // 7.2: Set the [[rendering thread state]] on the AudioContext to suspended.
    set_rendering_state(AudioContextState::Suspended);

    // 7.3: queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(GC::Heap::the(), [promise, this]() {
        auto& realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 7.3.1: Resolve promise.
        WebIDL::resolve_promise(promise);

        // 7.3.2: If the state attribute of the AudioContext is not already "suspended":
        if (state() != AudioContextState::Suspended) {
            // 7.3.2.1: Set the state attribute of the AudioContext to "suspended".
            set_control_state(AudioContextState::Suspended);

            // 7.3.2.2: queue a media element task to fire an event named statechange at the AudioContext.
            queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this]() {
                this->dispatch_event(create_associated_event(HTML::EventNames::statechange));
            }));
        }
    }));
    return {};
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-close
WebIDL::ExceptionOr<void> AudioContext::close(GC::Ref<WebIDL::Promise> promise)
{
    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = relevant_window().associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create("Document is not fully active"_utf16);

    // 3. If the [[control thread state]] flag on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == AudioContextState::Closed) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Audio context is already closed."_utf16));
        return {};
    }

    // 4. Set the [[control thread state]] flag on the AudioContext to closed.
    set_control_state(AudioContextState::Closed);

    // 5. Queue a control message to close the AudioContext.
    // 5.1: Attempt to release system resources.
    if (m_renderer) {
        m_renderer->stop();
        m_renderer->clear_callbacks();
    }

    // 5.2: Set the [[rendering thread state]] to "suspended".
    set_rendering_state(AudioContextState::Suspended);

    // FIXME: 5.3: If this control message is being run in a reaction to the document being unloaded, abort this algorithm.

    // 5.4: queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(GC::Heap::the(), [promise, this]() {
        auto& realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 5.4.1: Resolve promise.
        WebIDL::resolve_promise(promise);

        // 5.4.2: If the state attribute of the AudioContext is not already "closed":
        if (state() != AudioContextState::Closed) {
            // 5.4.2.1: Set the state attribute of the AudioContext to "closed".
            set_control_state(AudioContextState::Closed);
        }

        // 5.4.2.2: queue a media element task to fire an event named statechange at the AudioContext.
        // FIXME: Attempting to queue another task in here causes an assertion fail at Vector.h:148
        this->dispatch_event(create_associated_event(HTML::EventNames::statechange));
    }));

    return {};
}

bool AudioContext::start_rendering_audio_graph()
{
    if (m_renderer)
        m_renderer->resume();
    return true;
}

// The renderer's callback captures a GC root on this context, keeping it alive while it renders. That root is released
// by clearing the callbacks when the context is suspended, closed, or its document becomes inactive, so this is called
// again to re-establish it whenever rendering resumes.
void AudioContext::set_renderer_callbacks()
{
    VERIFY(m_renderer);
    m_renderer->set_on_sources_ended([self = GC::make_root(*this)](Vector<NodeID> const& ended_nodes) {
        self->handle_ended_sources(ended_nodes);
    });
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-createmediaelementsource
WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> AudioContext::create_media_element_source(GC::Ptr<HTML::HTMLMediaElement> media_element)
{
    return MediaElementAudioSourceNode::create(*this, *media_element);
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-createmediastreamsource
WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> AudioContext::create_media_stream_source(GC::Ptr<MediaCapture::MediaStream> media_stream)
{
    MediaStreamAudioSourceOptions options { .media_stream = *media_stream };
    return MediaStreamAudioSourceNode::create(*this, options);
}

}
