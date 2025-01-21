/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioContextPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/MessageChannel.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioContext);

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-audiocontext
WebIDL::ExceptionOr<GC::Ref<AudioContext>> AudioContext::construct_impl(JS::Realm& realm, Optional<AudioContextOptions> const& context_options)
{
    // If the current settings object’s responsible document is NOT fully active, throw an InvalidStateError and abort these steps.
    auto& settings = HTML::current_principal_settings_object();

    // FIXME: Not all settings objects currently return a responsible document.
    //        Therefore we only fail this check if responsible document is not null.
    if (!settings.responsible_document() || !settings.responsible_document()->is_fully_active()) {
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string);
    }

    // AD-HOC: The spec doesn't currently require the sample rate to be validated here,
    //         but other browsers do perform a check and there is a WPT test that expects this.
    if (context_options.has_value() && context_options->sample_rate.has_value())
        TRY(verify_audio_options_inside_nominal_range(realm, *context_options->sample_rate));

    // 1. Let context be a new AudioContext object.
    auto context = realm.create<AudioContext>(realm);
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

    // 10. If contextOptions is given, apply the options:
    if (context_options.has_value()) {
        // 1. If sinkId is specified, let sinkId be the value of contextOptions.sinkId and run the following substeps:

        // 2. Set the internal latency of context according to contextOptions.latencyHint, as described in latencyHint.
        switch (context_options->latency_hint) {
        case Bindings::AudioContextLatencyCategory::Balanced:
            // FIXME: Determine optimal settings for balanced.
            break;
        case Bindings::AudioContextLatencyCategory::Interactive:
            // FIXME: Determine optimal settings for interactive.
            break;
        case Bindings::AudioContextLatencyCategory::Playback:
            // FIXME: Determine optimal settings for playback.
            break;
        default:
            VERIFY_NOT_REACHED();
        }

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

    // FIXME: 11. If context is allowed to start, send a control message to start processing.
    // FIXME: Implement control message queue to run following steps on the rendering thread
    if (context->m_allowed_to_start) {
        // FIXME: 1. Let document be the current settings object's relevant global object's associated Document.
        // FIXME: 2. Attempt to acquire system resources to use a following audio output device based on [[sink ID]] for rendering

        // 2. Set this [[rendering thread state]] to running on the AudioContext.
        context->set_rendering_state(Bindings::AudioContextState::Running);

        // 3. Queue a media element task to execute the following steps:
        context->queue_a_media_element_task(GC::create_function(context->heap(), [&realm, context]() {
            // 1. Set the state attribute of the AudioContext to "running".
            context->set_control_state(Bindings::AudioContextState::Running);

            // 2. Fire an event named statechange at the AudioContext.
            context->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
        }));
    }

    // 12. Return context.
    return context;
}

AudioContext::~AudioContext() = default;

void AudioContext::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioContext);
}

void AudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_pending_resume_promises);
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-getoutputtimestamp
AudioTimestamp AudioContext::get_output_timestamp()
{
    dbgln("(STUBBED) getOutputTimestamp()");
    return {};
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-resume
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::resume()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string);

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. If the [[control thread state]] on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == Bindings::AudioContextState::Closed) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Audio context is already closed."_string));
        return promise;
    }

    // 4. Set [[suspended by user]] to true.
    m_suspended_by_user = true;

    // 5. If the context is not allowed to start, append promise to [[pending promises]] and [[pending resume promises]] and abort these steps, returning promise.
    if (m_allowed_to_start) {
        m_pending_promises.append(promise);
        m_pending_resume_promises.append(promise);
    }

    // 6. Set the [[control thread state]] on the AudioContext to running.
    set_control_state(Bindings::AudioContextState::Running);

    // 7. Queue a control message to resume the AudioContext.
    // FIXME: Implement control message queue to run following steps on the rendering thread

    // FIXME: 7.1: Attempt to acquire system resources.

    // 7.2: Set the [[rendering thread state]] on the AudioContext to running.
    set_rendering_state(Bindings::AudioContextState::Running);

    // 7.3: Start rendering the audio graph.
    if (!start_rendering_audio_graph()) {
        // 7.4: In case of failure, queue a media element task to execute the following steps:
        queue_a_media_element_task(GC::create_function(heap(), [&realm, this]() {
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            // 7.4.1: Reject all promises from [[pending resume promises]] in order, then clear [[pending resume promises]].
            for (auto const& promise : m_pending_resume_promises) {
                WebIDL::reject_promise(realm, promise, JS::js_null());

                // 7.4.2: Additionally, remove those promises from [[pending promises]].
                m_pending_promises.remove_first_matching([&promise](auto& pending_promise) {
                    return pending_promise == promise;
                });
            }
            m_pending_resume_promises.clear();
        }));
    }

    // 7.5: queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 7.5.1: Resolve all promises from [[pending resume promises]] in order.
        // 7.5.2: Clear [[pending resume promises]]. Additionally, remove those promises from
        //        [[pending promises]].
        for (auto const& pending_resume_promise : m_pending_resume_promises) {
            WebIDL::resolve_promise(realm, pending_resume_promise, JS::js_undefined());
            m_pending_promises.remove_first_matching([&pending_resume_promise](auto& pending_promise) {
                return pending_promise == pending_resume_promise;
            });
        }
        m_pending_resume_promises.clear();

        // 7.5.3: Resolve promise.
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());

        // 7.5.4: If the state attribute of the AudioContext is not already "running":
        if (state() != Bindings::AudioContextState::Running) {
            // 7.5.4.1: Set the state attribute of the AudioContext to "running".
            set_control_state(Bindings::AudioContextState::Running);

            // 7.5.4.2: queue a media element task to fire an event named statechange at the AudioContext.
            queue_a_media_element_task(GC::create_function(heap(), [&realm, this]() {
                this->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
            }));
        }
    }));

    // 8. Return promise.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-suspend
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::suspend()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string);

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. If the [[control thread state]] on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == Bindings::AudioContextState::Closed) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Audio context is already closed."_string));
        return promise;
    }

    // 4. Append promise to [[pending promises]].
    m_pending_promises.append(promise);

    // 5. Set [[suspended by user]] to true.
    m_suspended_by_user = true;

    // 6. Set the [[control thread state]] on the AudioContext to suspended.
    set_control_state(Bindings::AudioContextState::Suspended);

    // 7. Queue a control message to suspend the AudioContext.
    // FIXME: Implement control message queue to run following steps on the rendering thread

    // FIXME: 7.1: Attempt to release system resources.

    // 7.2: Set the [[rendering thread state]] on the AudioContext to suspended.
    set_rendering_state(Bindings::AudioContextState::Suspended);

    // 7.3: queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 7.3.1: Resolve promise.
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());

        // 7.3.2: If the state attribute of the AudioContext is not already "suspended":
        if (state() != Bindings::AudioContextState::Suspended) {
            // 7.3.2.1: Set the state attribute of the AudioContext to "suspended".
            set_control_state(Bindings::AudioContextState::Suspended);

            // 7.3.2.2: queue a media element task to fire an event named statechange at the AudioContext.
            queue_a_media_element_task(GC::create_function(heap(), [&realm, this]() {
                this->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
            }));
        }
    }));

    // 8. Return promise.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-close
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> AudioContext::close()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string);

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. If the [[control thread state]] flag on the AudioContext is closed reject the promise with InvalidStateError, abort these steps, returning promise.
    if (state() == Bindings::AudioContextState::Closed) {
        WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Audio context is already closed."_string));
        return promise;
    }

    // 4. Set the [[control thread state]] flag on the AudioContext to closed.
    set_control_state(Bindings::AudioContextState::Closed);

    // 5. Queue a control message to close the AudioContext.
    // FIXME: Implement control message queue to run following steps on the rendering thread

    // FIXME: 5.1: Attempt to release system resources.

    // 5.2: Set the [[rendering thread state]] to "suspended".
    set_rendering_state(Bindings::AudioContextState::Suspended);

    // FIXME: 5.3: If this control message is being run in a reaction to the document being unloaded, abort this algorithm.

    // 5.4: queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 5.4.1: Resolve promise.
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());

        // 5.4.2: If the state attribute of the AudioContext is not already "closed":
        if (state() != Bindings::AudioContextState::Closed) {
            // 5.4.2.1: Set the state attribute of the AudioContext to "closed".
            set_control_state(Bindings::AudioContextState::Closed);
        }

        // 5.4.2.2: queue a media element task to fire an event named statechange at the AudioContext.
        // FIXME: Attempting to queue another task in here causes an assertion fail at Vector.h:148
        this->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
    }));

    // 6. Return promise
    return promise;
}

// FIXME: Actually implement the rendering thread
bool AudioContext::start_rendering_audio_graph()
{
    bool render_result = true;
    return render_result;
}

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-createmediaelementsource
WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> AudioContext::create_media_element_source(GC::Ptr<HTML::HTMLMediaElement> media_element)
{
    MediaElementAudioSourceOptions options;
    options.media_element = media_element;
    return MediaElementAudioSourceNode::create(realm(), *this, options);
}

}
