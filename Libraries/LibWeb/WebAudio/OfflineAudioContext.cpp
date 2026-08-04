/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioContext);

WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::create_for_constructor(JS::Object& relevant_global_object, OfflineAudioContextOptions const& context_options)
{
    return create_for_constructor(HTML::relevant_window(relevant_global_object), context_options);
}

WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::create_for_constructor(JS::Object& relevant_global_object, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    return create_for_constructor(HTML::relevant_window(relevant_global_object), number_of_channels, length, sample_rate);
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::create_for_constructor(GC::Ref<DOM::EventTarget> relevant_global_object, OfflineAudioContextOptions const& context_options)
{
    // AD-HOC: This spec text is currently only mentioned in the constructor overload that takes separate arguments,
    //         but these parameters should be validated for both constructors.
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    TRY(verify_audio_options_inside_nominal_range(context_options.number_of_channels, context_options.length, context_options.sample_rate));

    // Let c be a new OfflineAudioContext object. Initialize c as follows:
    auto c = GC::Heap::the().allocate<OfflineAudioContext>(relevant_global_object, context_options.number_of_channels, context_options.length, context_options.sample_rate);
    c->set_listener(AudioListener::create(c));

    // 1. Set the [[control thread state]] for c to "suspended".
    c->set_control_state(AudioContextState::Suspended);

    // 2. Set the [[rendering thread state]] for c to "suspended".
    c->set_rendering_state(AudioContextState::Suspended);

    // FIXME: 3. Determine the [[render quantum size]] for this OfflineAudioContext, based on the value of the renderSizeHint:

    // 4. Construct an AudioDestinationNode with its channelCount set to contextOptions.numberOfChannels.
    c->m_destination = TRY(AudioDestinationNode::create(c, context_options.number_of_channels));

    // FIXME: 5. Let messageChannel be a new MessageChannel.
    // FIXME: 6. Let controlSidePort be the value of messageChannel’s port1 attribute.
    // FIXME: 7. Let renderingSidePort be the value of messageChannel’s port2 attribute.
    // FIXME: 8. Let serializedRenderingSidePort be the result of StructuredSerializeWithTransfer(renderingSidePort, « renderingSidePort »).
    // FIXME: 9. Set this audioWorklet's port to controlSidePort.
    // FIXME: 10. Queue a control message to set the MessagePort on the AudioContextGlobalScope, with serializedRenderingSidePort.

    return c;
}

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext-numberofchannels-length-samplerate
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::create_for_constructor(
    GC::Ref<DOM::EventTarget> relevant_global_object,
    WebIDL::UnsignedLong number_of_channels,
    WebIDL::UnsignedLong length,
    float sample_rate)
{
    OfflineAudioContextOptions options {};
    options.number_of_channels = number_of_channels;
    options.length = length;
    options.sample_rate = sample_rate;
    return create_for_constructor(relevant_global_object, options);
}

OfflineAudioContext::~OfflineAudioContext() = default;

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
WebIDL::ExceptionOr<void> OfflineAudioContext::start_rendering(GC::Ref<WebIDL::Promise> promise)
{
    // 1. If this’s relevant global object’s associated Document is not fully active then return a promise rejected with "InvalidStateError" DOMException.
    auto& window = relevant_window();
    auto const& associated_document = window.associated_document();

    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create("Document is not fully active"_utf16);

    // AD-HOC: Not in spec explicitly, but this should account for detached iframes too. See /the-offlineaudiocontext-interface/startrendering-after-discard.html WPT.
    auto navigable = window.navigable();
    if (navigable && navigable->has_been_destroyed())
        return WebIDL::InvalidStateError::create("The iframe has been detached"_utf16);

    // 2. If the [[rendering started]] slot on the OfflineAudioContext is true, return a rejected promise with InvalidStateError, and abort these steps.
    if (m_rendering_started) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Rendering is already started"_utf16));
        return {};
    }

    // 3. Set the [[rendering started]] slot of the OfflineAudioContext to true.
    m_rendering_started = true;

    // 5. Create a new AudioBuffer, with a number of channels, length and sample rate equal respectively to the
    //    numberOfChannels, length and sampleRate values passed to this instance’s constructor in the contextOptions
    //    parameter.
    auto buffer_result = create_buffer(m_number_of_channels, length(), sample_rate());

    // 6. If an exception was thrown during the preceding AudioBuffer constructor call, reject promise with this exception.
    if (buffer_result.is_exception()) {
        WebIDL::reject_promise_with_exception(promise, buffer_result.release_error());
        return {};
    }

    // Assign this buffer to an internal slot [[rendered buffer]] in the OfflineAudioContext.
    m_rendered_buffer = buffer_result.release_value();

    // 7. Otherwise, in the case that the buffer was successfully constructed, begin offline rendering.
    begin_offline_rendering(promise);

    // 8. Append promise to [[pending promises]].
    m_pending_promises.append(promise);

    return {};
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
        auto channel_data = MUST(m_rendered_buffer->get_channel_data(relevant_global_object(), channel_index));
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
    queue_a_media_element_task(GC::create_function(GC::Heap::the(), [promise, this]() {
        auto& realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 4.1 Resolve the promise created by startRendering() with [[rendered buffer]].
        resolve_audio_buffer_promise(realm, promise, *this->m_rendered_buffer);

        // AD-HOC: Remove resolved promise from [[pending promises]]
        // https://github.com/WebAudio/web-audio-api/issues/2648
        m_pending_promises.remove_all_matching([promise](GC::Ref<WebIDL::Promise> const& p) {
            return p.ptr() == promise.ptr();
        });

        // 4.2: Queue a media element task to fire an event named complete at the OfflineAudioContext using OfflineAudioCompletionEvent
        //      whose renderedBuffer property is set to [[rendered buffer]].
        queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this]() {
            OfflineAudioCompletionEventInit event_init { {}, *this->m_rendered_buffer };
            auto event = OfflineAudioCompletionEvent::create(HTML::EventNames::complete, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object()));
            this->dispatch_event(event);
        }));
    }));
}

void OfflineAudioContext::handle_suspended(double suspend_time)
{
    set_control_state(Bindings::AudioContextState::Suspended);
    set_rendering_state(Bindings::AudioContextState::Suspended);
    set_current_time(suspend_time);

    auto frame = static_cast<u64>(AK::round(suspend_time * sample_rate()));
    // Suspensions take effect at the beginning of the first render quantum at
    // or after the requested time. Keep the promise keyed by that same frame
    // so the rendering-thread callback can resolve it when it reaches the
    // boundary.
    frame = ((frame + render_quantum_size() - 1) / render_quantum_size()) * render_quantum_size();
    auto suspend_promise = m_suspend_promises.take(frame);
    queue_a_media_element_task(GC::create_function(heap(), [this, suspend_promise]() {
        auto& realm = HTML::relevant_realm(this->relevant_global_object());
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        if (suspend_promise.has_value())
            WebIDL::resolve_promise(*suspend_promise);
        this->dispatch_event(DOM::Event::create(this->relevant_global_object(), HTML::EventNames::statechange));
    }));
}

void OfflineAudioContext::queue_a_statechange_event()
{
    queue_a_media_element_task(GC::create_function(heap(), [this]() {
        this->dispatch_event(DOM::Event::create(this->relevant_global_object(), HTML::EventNames::statechange));
    }));
}

WebIDL::ExceptionOr<void> OfflineAudioContext::resume()
{
    if (!m_renderer || state() != AudioContextState::Suspended)
        return WebIDL::InvalidStateError::create("The OfflineAudioContext is not suspended"_utf16);

    m_renderer->resume();
    set_control_state(AudioContextState::Running);
    set_rendering_state(AudioContextState::Running);
    queue_a_statechange_event();
    return {};
}

WebIDL::ExceptionOr<void> OfflineAudioContext::resume(GC::Ref<WebIDL::Promise> promise)
{
    auto result = resume();
    if (result.is_error()) {
        WebIDL::reject_promise_with_exception(promise, result.release_error());
        return {};
    }
    WebIDL::resolve_promise(promise);
    return {};
}

WebIDL::ExceptionOr<void> OfflineAudioContext::suspend(double suspend_time)
{
    (void)suspend_time;
    return WebIDL::InvalidStateError::create("OfflineAudioContext::suspend() requires a promise"_utf16);
}

WebIDL::ExceptionOr<void> OfflineAudioContext::suspend(double suspend_time, GC::Ref<WebIDL::Promise> promise)
{
    if (!isfinite(suspend_time) || suspend_time < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Suspend time must be a finite, non-negative number"_utf16 };

    auto frame = static_cast<u64>(AK::round(suspend_time * sample_rate()));
    frame = ((frame + render_quantum_size() - 1) / render_quantum_size()) * render_quantum_size();
    if (frame >= m_length)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Suspend time must be less than the duration of the context"_utf16 };
    if (m_suspend_promises.contains(frame))
        return WebIDL::InvalidStateError::create("A suspension is already scheduled at this time"_utf16);

    m_suspend_promises.set(frame, promise);
    if (m_renderer && !m_renderer->request_suspend(frame)) {
        m_suspend_promises.remove(frame);
        return WebIDL::InvalidStateError::create("The rendering thread has already passed the requested suspend time"_utf16);
    }
    return {};
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

void OfflineAudioContext::document_became_inactive()
{
    if (m_renderer) {
        m_renderer->stop();
        m_renderer->clear_callbacks();
    }
}

OfflineAudioContext::OfflineAudioContext(GC::Ref<DOM::EventTarget> relevant_global_object, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
    : BaseAudioContext(relevant_global_object, sample_rate)
    , m_length(length)
    , m_number_of_channels(number_of_channels)
{
}

void OfflineAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);
    visitor.visit(m_suspend_promises);
}

}
