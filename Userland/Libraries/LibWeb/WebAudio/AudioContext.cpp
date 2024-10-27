/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioContextPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioContext);

// https://webaudio.github.io/web-audio-api/#dom-audiocontext-audiocontext
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioContext>> AudioContext::construct_impl(JS::Realm& realm, AudioContextOptions const& context_options)
{
    return realm.heap().allocate<AudioContext>(realm, realm, context_options);
}

AudioContext::AudioContext(JS::Realm& realm, AudioContextOptions const& context_options)
    : BaseAudioContext(realm)
{
    // FIXME: If the current settings object’s responsible document is NOT fully active, throw an InvalidStateError and abort these steps.

    // 1: Set a [[control thread state]] to suspended on the AudioContext.
    BaseAudioContext::set_control_state(Bindings::AudioContextState::Suspended);

    // 2: Set a [[rendering thread state]] to suspended on the AudioContext.
    BaseAudioContext::set_rendering_state(Bindings::AudioContextState::Suspended);

    // 3: Let [[pending resume promises]] be a slot on this AudioContext, that is an initially empty ordered list of promises.

    // 4: If contextOptions is given, apply the options:
    // 4.1: Set the internal latency of this AudioContext according to contextOptions.latencyHint, as described in latencyHint.
    switch (context_options.latency_hint) {
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

    // 4.2: If contextOptions.sampleRate is specified, set the sampleRate of this AudioContext to this value. Otherwise,
    //      use the sample rate of the default output device. If the selected sample rate differs from the sample rate of the output device,
    //      this AudioContext MUST resample the audio output to match the sample rate of the output device.
    if (context_options.sample_rate.has_value()) {
        BaseAudioContext::set_sample_rate(context_options.sample_rate.value());
    } else {
        // FIXME: This would ideally be coming from the default output device, but we can only get this on Serenity
        // For now we'll just have to resample
        BaseAudioContext::set_sample_rate(44100);
    }

    // FIXME: 5: If the context is allowed to start, send a control message to start processing.
    // FIXME: Implement control message queue to run following steps on the rendering thread
    if (m_allowed_to_start) {
        // FIXME: 5.1: Attempt to acquire system resources. In case of failure, abort the following steps.

        // 5.2: Set the [[rendering thread state]] to "running" on the AudioContext.
        BaseAudioContext::set_rendering_state(Bindings::AudioContextState::Running);

        // 5.3: queue a media element task to execute the following steps:
        queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, this]() {
            // 5.3.1: Set the state attribute of the AudioContext to "running".
            BaseAudioContext::set_control_state(Bindings::AudioContextState::Running);

            // 5.3.2: queue a media element task to fire an event named statechange at the AudioContext.
            this->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
        }));
    }
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
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> AudioContext::resume()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
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
        queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, this]() {
            HTML::TemporaryExecutionContext context(Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

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
    queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

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
            queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, this]() {
                this->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
            }));
        }
    }));

    // 8. Return promise.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-suspend
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> AudioContext::suspend()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
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
    queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 7.3.1: Resolve promise.
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());

        // 7.3.2: If the state attribute of the AudioContext is not already "suspended":
        if (state() != Bindings::AudioContextState::Suspended) {
            // 7.3.2.1: Set the state attribute of the AudioContext to "suspended".
            set_control_state(Bindings::AudioContextState::Suspended);

            // 7.3.2.2: queue a media element task to fire an event named statechange at the AudioContext.
            queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, this]() {
                this->dispatch_event(DOM::Event::create(realm, HTML::EventNames::statechange));
            }));
        }
    }));

    // 8. Return promise.
    return promise;
}

// https://www.w3.org/TR/webaudio/#dom-audiocontext-close
WebIDL::ExceptionOr<JS::NonnullGCPtr<WebIDL::Promise>> AudioContext::close()
{
    auto& realm = this->realm();

    // 1. If this's relevant global object's associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = verify_cast<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
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
    queue_a_media_element_task(JS::create_heap_function(heap(), [&realm, promise, this]() {
        HTML::TemporaryExecutionContext context(Bindings::host_defined_environment_settings_object(realm), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

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

}
