/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OfflineAudioCompletionEvent.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioContext);

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm, Bindings::OfflineAudioContextOptions const& context_options)
{
    // AD-HOC: This spec text is currently only mentioned in the constructor overload that takes separate arguments,
    //         but these parameters should be validated for both constructors.
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    TRY(verify_audio_options_inside_nominal_range(realm, context_options.number_of_channels, context_options.length, context_options.sample_rate));

    // Let c be a new OfflineAudioContext object. Initialize c as follows:
    auto c = realm.create<OfflineAudioContext>(realm, context_options.number_of_channels, context_options.length, context_options.sample_rate);

    // 1. Set the [[control thread state]] for c to "suspended".
    c->set_control_state(Bindings::AudioContextState::Suspended);

    // 2. Set the [[rendering thread state]] for c to "suspended".
    c->set_rendering_state(Bindings::AudioContextState::Suspended);

    // FIXME: 3. Determine the [[render quantum size]] for this OfflineAudioContext, based on the value of the renderSizeHint:

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
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(
    JS::Realm& realm,
    WebIDL::UnsignedLong number_of_channels,
    WebIDL::UnsignedLong length,
    float sample_rate)
{
    Bindings::OfflineAudioContextOptions options {};
    options.number_of_channels = number_of_channels;
    options.length = length;
    options.sample_rate = sample_rate;
    return construct_impl(realm, options);
}

OfflineAudioContext::~OfflineAudioContext() = default;

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

void OfflineAudioContext::begin_offline_rendering(GC::Ref<WebIDL::Promise> promise)
{
    // To begin offline rendering, the following steps MUST happen on a rendering thread that is created for the occasion.
    // 1: Given the current connections and scheduled changes, start rendering length sample-frames of audio into [[rendered buffer]]
    // 2: For every render quantum, check and suspend rendering if necessary.
    // 3: If a suspended context is resumed, continue to render the buffer.
    m_renderer = Rendering::OfflineAudioRenderer::create(control_message_queue(), destination()->node_id(), m_number_of_channels, m_length, sample_rate(), render_quantum_size());
    m_renderer->set_on_complete([self = GC::make_root(this), promise = GC::make_root(promise)] {
        self->finish_rendering(*promise);
    });
    m_renderer->set_on_suspended([self = GC::make_root(this)](double suspend_time) {
        self->handle_suspended(suspend_time);
    });
    m_renderer->set_on_sources_ended([self = GC::make_root(this)](Vector<NodeID> const& ended_nodes) {
        self->handle_ended_sources(ended_nodes);
    });
    for (auto const& [frame, suspend_promise] : m_suspend_promises)
        m_renderer->request_suspend(frame);

    // AD-HOC: Other engines transition the context to "running" once rendering starts.
    set_control_state(Bindings::AudioContextState::Running);
    set_rendering_state(Bindings::AudioContextState::Running);
    queue_a_statechange_event();

    m_renderer->start_rendering();
}

void OfflineAudioContext::finish_rendering(GC::Ref<WebIDL::Promise> promise)
{
    // NB: Copy the rendered samples into [[rendered buffer]].
    auto const& rendered_channels = m_renderer->rendered_channels();
    for (size_t channel_index = 0; channel_index < rendered_channels.size(); ++channel_index) {
        auto channel_data = MUST(m_rendered_buffer->get_channel_data(channel_index));
        auto const& samples = rendered_channels[channel_index];
        channel_data->viewed_array_buffer()->overwrite(channel_data->byte_offset(), samples.data(), samples.size() * sizeof(float));
    }

    set_current_time(m_renderer->frames_rendered() / static_cast<double>(sample_rate()));

    // AD-HOC: Other engines transition the context to "closed" once rendering completes.
    set_control_state(Bindings::AudioContextState::Closed);
    set_rendering_state(Bindings::AudioContextState::Closed);
    queue_a_statechange_event();

    // NB: Break the reference cycle between the renderer's callbacks and this context.
    m_renderer->clear_callbacks();

    // 4: Once the rendering is complete, queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(heap(), [promise, this]() {
        HTML::TemporaryExecutionContext context(this->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 4.1 Resolve the promise created by startRendering() with [[rendered buffer]].
        WebIDL::resolve_promise(this->realm(), promise, this->m_rendered_buffer);

        // AD-HOC: Remove resolved promise from [[pending promises]]
        // https://github.com/WebAudio/web-audio-api/issues/2648
        m_pending_promises.remove_all_matching([promise](GC::Ref<WebIDL::Promise> const& p) {
            return p.ptr() == promise.ptr();
        });

        // 4.2: Queue a media element task to fire an event named complete at the OfflineAudioContext using OfflineAudioCompletionEvent
        //      whose renderedBuffer property is set to [[rendered buffer]].
        queue_a_media_element_task(GC::create_function(heap(), [this]() {
            auto event_init = Bindings::OfflineAudioCompletionEventInit {
                {
                    .bubbles = false,
                    .cancelable = false,
                    .composed = false,
                },
                *this->m_rendered_buffer,
            };
            auto event = MUST(OfflineAudioCompletionEvent::construct_impl(this->realm(), HTML::EventNames::complete, event_init));
            this->dispatch_event(event);
        }));
    }));
}

// Invoked on the control thread once the rendering thread has reached a scheduled suspension.
void OfflineAudioContext::handle_suspended(double suspend_time)
{
    set_control_state(Bindings::AudioContextState::Suspended);
    set_rendering_state(Bindings::AudioContextState::Suspended);
    set_current_time(suspend_time);

    auto frame = static_cast<u64>(AK::round(suspend_time * sample_rate()));
    auto suspend_promise = m_suspend_promises.take(frame);

    queue_a_media_element_task(GC::create_function(heap(), [this, suspend_promise]() {
        HTML::TemporaryExecutionContext context(this->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        if (suspend_promise.has_value())
            WebIDL::resolve_promise(this->realm(), *suspend_promise, JS::js_undefined());
        this->dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::statechange));
    }));
}

void OfflineAudioContext::queue_a_statechange_event()
{
    queue_a_media_element_task(GC::create_function(heap(), [this]() {
        this->dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::statechange));
    }));
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-resume
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::resume()
{
    auto& realm = this->realm();

    // FIXME: 1. If this's relevant global object's associated Document is not fully active then return a promise
    //           rejected with "InvalidStateError" DOMException.

    // 2. Let promise be a new Promise.
    // 3. Abort these steps and reject promise with InvalidStateError when any of following conditions is true:
    //    - The [[control thread state]] on the OfflineAudioContext is closed.
    //    - The [[rendering started]] slot on the OfflineAudioContext is false.
    if (state() == Bindings::AudioContextState::Closed) {
        auto error = WebIDL::InvalidStateError::create(realm, "Context is closed"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }
    if (!m_rendering_started) {
        auto error = WebIDL::InvalidStateError::create(realm, "Rendering has not started"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    auto promise = WebIDL::create_promise(realm);

    // 4. Set the [[control thread state]] flag on the OfflineAudioContext to running.
    set_control_state(Bindings::AudioContextState::Running);

    // 5. Queue a control message to resume the OfflineAudioContext.
    // AD-HOC: The control message's steps below run inline on the control thread instead.

    // 5.1. Set the [[rendering thread state]] on the OfflineAudioContext to running.
    set_rendering_state(Bindings::AudioContextState::Running);

    // 5.2. Start rendering the audio graph.
    if (m_renderer)
        m_renderer->resume();

    // AD-HOC: The promise is resolved and a statechange event is fired immediately instead of from queued media
    //         element tasks.
    queue_a_statechange_event();
    WebIDL::resolve_promise(realm, promise, JS::js_undefined());

    // 6. Return promise.
    return promise;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-suspend
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::suspend(double suspend_time)
{
    auto& realm = this->realm();

    // The specified suspension time is quantized and rounded up to the render quantum size. If the quantized frame
    // number is negative or is less than or equal to the current time or is greater than or equal to the total render
    // duration or is scheduled by another suspend for the same time, then the promise is rejected with
    // InvalidStateError.
    if (suspend_time < 0) {
        auto error = WebIDL::InvalidStateError::create(realm, "suspendTime must not be negative"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // NB: The comparison against the current time only applies once rendering has started, so suspensions can still
    //     be scheduled for the very beginning of the rendering beforehand.
    if (suspend_time <= current_time() && m_rendering_started) {
        auto error = WebIDL::InvalidStateError::create(realm, "suspendTime must be in the future"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // 4. Quantize suspendTime to the render quantum boundary that comes right after it.
    auto quantum_size = render_quantum_size();
    auto frame = static_cast<u64>(AK::ceil(suspend_time * sample_rate() / quantum_size)) * quantum_size;

    // 5. If frame is at or beyond the total duration of rendering, return a promise rejected with InvalidStateError.
    if (frame >= m_length) {
        auto error = WebIDL::InvalidStateError::create(realm, "Cannot suspend at or beyond the total duration of rendering"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // 6. If another suspend has been scheduled at frame, return a promise rejected with InvalidStateError.
    if (m_suspend_promises.contains(frame)) {
        auto error = WebIDL::InvalidStateError::create(realm, "A suspension is already scheduled at this frame"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // AD-HOC: Rendering may already have progressed past frame while this call was being made.
    if (m_renderer && !m_renderer->request_suspend(frame)) {
        auto error = WebIDL::InvalidStateError::create(realm, "Rendering has already progressed beyond the given suspendTime"_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    auto promise = WebIDL::create_promise(realm);
    m_suspend_promises.set(frame, promise);
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
{
}

void OfflineAudioContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OfflineAudioContext);
    Base::initialize(realm);
}

void OfflineAudioContext::document_became_inactive()
{
    // End rendering and release the GC roots held by the renderer's callbacks, so a context belonging to a
    // navigated-away document can eventually be collected even if it was suspended indefinitely.
    if (m_renderer) {
        m_renderer->stop();
        m_renderer->clear_callbacks();
    }
}

void OfflineAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);
    visitor.visit(m_suspend_promises);
}

}
