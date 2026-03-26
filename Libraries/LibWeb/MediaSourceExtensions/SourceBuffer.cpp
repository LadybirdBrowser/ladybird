/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferProcessor.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBuffer);

SourceBuffer::SourceBuffer(JS::Realm& realm, MediaSource& media_source)
    : DOM::EventTarget(realm)
    , m_media_source(media_source)
    , m_processor(adopt_ref(*new SourceBufferProcessor()))
{
    m_processor->set_append_error_callback([self = GC::Weak(*this)]() {
        if (!self)
            return;
        self->run_append_error_algorithm();
    });
}

SourceBuffer::~SourceBuffer() = default;

void SourceBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBuffer);
    Base::initialize(realm);
}

void SourceBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_source);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdatestart
void SourceBuffer::set_onupdatestart(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::updatestart, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdatestart
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdatestart()
{
    return event_handler_attribute(EventNames::updatestart);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdate
void SourceBuffer::set_onupdate(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::update, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdate
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdate()
{
    return event_handler_attribute(EventNames::update);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdateend
void SourceBuffer::set_onupdateend(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::updateend, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onupdateend
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onupdateend()
{
    return event_handler_attribute(EventNames::updateend);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onerror
void SourceBuffer::set_onerror(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::error, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onerror
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onerror()
{
    return event_handler_attribute(EventNames::error);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onabort
void SourceBuffer::set_onabort(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::abort, event_handler);
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-onabort
GC::Ptr<WebIDL::CallbackType> SourceBuffer::onabort()
{
    return event_handler_attribute(EventNames::abort);
}

bool SourceBuffer::updating() const
{
    return m_processor->updating();
}

// https://w3c.github.io/media-source/#sourcebuffer-prepare-append
WebIDL::ExceptionOr<void> SourceBuffer::prepare_append()
{
    // FIXME: Support MediaSourceExtensions in workers.
    if (!m_media_source->media_element_assigned_to())
        return WebIDL::InvalidStateError::create(realm(), "Unsupported in workers"_utf16);

    // 1. If the SourceBuffer has been removed from the sourceBuffers attribute of the parent media source then throw an
    //    InvalidStateError exception and abort these steps.
    if (!m_media_source->source_buffers()->contains(*this))
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (updating())
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is already updating"_utf16);

    // 3. Let recent element error be determined as follows:
    auto recent_element_error = [&] {
        // If the MediaSource was constructed in a Window
        if (m_media_source->media_element_assigned_to()) {
            // Let recent element error be true if the HTMLMediaElement's error attribute is not null.
            // If that attribute is null, then let recent element error be false.
            return m_media_source->media_element_assigned_to()->error() != nullptr;
        }
        // Otherwise
        // FIXME: Let recent element error be the value resulting from the steps for the Window case,
        //        but run on the Window HTMLMediaElement on any change to its error attribute and
        //        communicated by using [[port to worker]] implicit messages.
        //        If such a message has not yet been received, then let recent element error be false.
        VERIFY_NOT_REACHED();
    }();

    // 4. If recent element error is true, then throw an InvalidStateError exception and abort these steps.
    if (recent_element_error)
        return WebIDL::InvalidStateError::create(realm(), "Element has a recent error"_utf16);

    // 5. If the readyState attribute of the parent media source is in the "ended" state then run the following steps:
    if (m_media_source->ready_state() == Bindings::ReadyState::Ended) {
        // 1. Set the readyState attribute of the parent media source to "open"
        // 2. Queue a task to fire an event named sourceopen at the parent media source.
        m_media_source->set_ready_state_to_open_and_fire_sourceopen_event();
    }

    // 6. Run the coded frame eviction algorithm.
    m_processor->run_coded_frame_eviction();

    // 7. If the [[buffer full flag]] equals true, then throw a QuotaExceededError exception and abort these steps.
    if (m_processor->is_buffer_full())
        return WebIDL::QuotaExceededError::create(realm(), "Buffer is full"_utf16);

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendbuffer
WebIDL::ExceptionOr<void> SourceBuffer::append_buffer(GC::Root<WebIDL::BufferSource> const& data)
{
    // 1. Run the prepare append algorithm.
    TRY(prepare_append());

    // 2. Add data to the end of the [[input buffer]].
    if (auto array_buffer = data->viewed_array_buffer(); array_buffer && !array_buffer->is_detached()) {
        auto bytes = array_buffer->buffer().bytes().slice(data->byte_offset(), data->byte_length());
        m_processor->append_to_input_buffer(bytes);
    }

    // 3. Set the updating attribute to true.
    m_processor->set_updating(true);

    // 4. Queue a task to fire an event named updatestart at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));
    }));

    // 5. Asynchronously run the buffer append algorithm.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        run_buffer_append_algorithm();
    }));

    return {};
}

// https://w3c.github.io/media-source/#sourcebuffer-buffer-append
void SourceBuffer::run_buffer_append_algorithm()
{
    // 1. Run the segment parser loop algorithm.
    // NB: Steps 2-5 (error handling, updating flag, update/updateend events) are handled by the segment parser loop
    //     done callback set up in the constructor.
    m_processor->run_segment_parser_loop();
}

// https://w3c.github.io/media-source/#sourcebuffer-append-error
void SourceBuffer::run_append_error_algorithm()
{
    // 1. Run the reset parser state algorithm.
    m_processor->reset_parser_state();

    // 2. Set the updating attribute to false.
    m_processor->set_updating(false);

    // 3. Queue a task to fire an event named error at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::error));
    }));

    // 4. Queue a task to fire an event named updateend at this SourceBuffer object.
    m_media_source->queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));

    // 5. Run the end of stream algorithm with the error parameter set to "decode".
    m_media_source->run_end_of_stream_algorithm({}, Bindings::EndOfStreamError::Decode);
}

}
