/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025-2026, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>
#include <LibWeb/WebAudio/RenderGraphSnapshot.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioContext);

// https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-offlineaudiocontext
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm, OfflineAudioContextOptions const& context_options)
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
WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> OfflineAudioContext::construct_impl(JS::Realm& realm,
    WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    return construct_impl(realm, { number_of_channels, length, sample_rate });
}

OfflineAudioContext::~OfflineAudioContext()
{
    if (m_render_thread) {
        m_render_thread.clear();
    }

    if (m_render_completion_notifier) {
        m_render_completion_notifier->set_enabled(false);
        m_render_completion_notifier = nullptr;
    }

    if (m_render_completion_read_fd >= 0) {
        MUST(Core::System::close(m_render_completion_read_fd));
        m_render_completion_read_fd = -1;
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

void OfflineAudioContext::begin_offline_rendering(GC::Ref<WebIDL::Promise> promise)
{
    // To begin offline rendering, the following steps MUST happen on a rendering thread that is created for the occasion.

    // 1: Given the current connections and scheduled changes, start rendering length sample-frames of audio into [[rendered buffer]]

    // NB: The WebAudio DOM objects are GC-managed and must remain on the control thread,
    //     so we snapshot the relevant graph state into a render-thread-friendly structure.
    auto graph = Web::WebAudio::snapshot_render_graph(destination(), sample_rate());

    OfflineAudioRenderRequest request;
    request.graph = move(graph);
    request.number_of_channels = static_cast<u32>(m_number_of_channels);
    request.length_in_sample_frames = static_cast<u32>(length());
    request.sample_rate = static_cast<f32>(sample_rate());

    // Store control-thread-only state so the render thread can post a single completion notification.
    m_pending_render_promise = promise;

    // Create a one-shot completion signal. The render thread will write a byte to the pipe when it finishes.
    // Keep completion logic on the control thread.
    auto completion_pipe_fds = MUST(Core::System::pipe2(O_CLOEXEC));
    m_render_completion_read_fd = completion_pipe_fds[0];
    int completion_write_fd = completion_pipe_fds[1];

    m_render_completion_notifier = Core::Notifier::construct(m_render_completion_read_fd, Core::Notifier::Type::Read);
    m_render_completion_notifier->on_activation = [this] {
        handle_offline_render_completion();
    };
    m_render_completion_notifier->set_enabled(true);

    m_render_thread = make<OfflineAudioRenderThread>(move(request), completion_write_fd);
    m_render_thread->start();
}

void OfflineAudioContext::handle_offline_render_completion()
{
    auto& realm = this->realm();

    // The completion notifier can fire outside any JS execution context. We need an active execution context
    // for queuing media element tasks via BaseAudioContext::queue_a_media_element_task().
    HTML::TemporaryExecutionContext temporary_execution_context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    if (m_render_completion_read_fd >= 0) {
        u8 buffer[64];
        (void)Core::System::read(m_render_completion_read_fd, { buffer, sizeof(buffer) });
    }

    if (m_render_completion_notifier) {
        m_render_completion_notifier->set_enabled(false);
        m_render_completion_notifier = nullptr;
    }

    if (m_render_completion_read_fd >= 0) {
        MUST(Core::System::close(m_render_completion_read_fd));
        m_render_completion_read_fd = -1;
    }

    if (!m_render_thread || !m_render_thread->is_finished() || !m_pending_render_promise.has_value())
        return;

    GC::Ref<WebIDL::Promise> promise = m_pending_render_promise.release_value();
    Optional<OfflineAudioRenderResult> render_result = m_render_thread->take_result();
    m_render_thread.clear();

    if (!render_result.has_value()) {
        queue_a_media_element_task(GC::create_function(heap(), [promise, this] {
            auto& realm = this->realm();
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::reject_promise(realm, promise, WebIDL::AbortError::create(realm, "Offline rendering was aborted"_utf16));
        }));
        return;
    }

    OfflineAudioRenderResult result = render_result.release_value();

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
    size_t const quantum_size = BaseAudioContext::render_quantum_size();
    size_t const rendered_frames = length();
    size_t const processed_frames = quantum_size == 0
        ? rendered_frames
        : ((rendered_frames + quantum_size - 1) / quantum_size) * quantum_size;
    set_current_frame(processed_frames);
    // 4. Once the rendering is complete, queue a media element task to execute the following steps.
    queue_a_media_element_task(GC::create_function(heap(), [promise, this]() {
        HTML::TemporaryExecutionContext context(this->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // After rendering completes, OfflineAudioContext transitions to "closed".
        // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-startrendering
        set_control_state(Bindings::AudioContextState::Closed);
        set_rendering_state(Bindings::AudioContextState::Closed);
        dispatch_event(DOM::Event::create(this->realm(), HTML::EventNames::statechange));

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

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::resume()
{
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::resume"_utf16);
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> OfflineAudioContext::suspend(double suspend_time)
{
    (void)suspend_time;
    return WebIDL::NotSupportedError::create(realm(), "FIXME: Implement OfflineAudioContext::suspend"_utf16);
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

void OfflineAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);

    if (m_pending_render_promise.has_value())
        visitor.visit(m_pending_render_promise.value());
}

}
