/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SourceBufferPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TimeRanges.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(SourceBuffer);

SourceBuffer::SourceBuffer(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

SourceBuffer::SourceBuffer(JS::Realm& realm, MediaSource& media_source, MimeSniff::MimeType const&)
    : DOM::EventTarget(realm)
    , m_media_source(&media_source)
{
}

SourceBuffer::~SourceBuffer() = default;

void SourceBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SourceBuffer);
    Base::initialize(realm);

    // Initialize empty TimeRanges for buffered property
    m_buffered = realm.create<HTML::TimeRanges>(realm);
}

void SourceBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_source);
    visitor.visit(m_buffered);
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

// https://w3c.github.io/media-source/#dom-sourcebuffer-buffered
GC::Ref<HTML::TimeRanges> SourceBuffer::buffered() const
{
    // Return the buffered time ranges
    return *m_buffered;
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-appendbuffer
WebIDL::ExceptionOr<void> SourceBuffer::append_buffer(GC::Root<WebIDL::BufferSource> const& data)
{
    // 1. If data is null then throw a TypeError exception and abort these steps.
    if (!data.ptr())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Data cannot be null"_string };

    // 2. If this object has been removed from the sourceBuffers attribute of the parent media source then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed from MediaSource"_utf16);

    // 3. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is currently updating"_utf16);

    // 4. If the readyState attribute of the parent media source is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 5. If the HTMLMediaElement.error attribute is not null, then throw an InvalidStateError exception and abort these steps.
    // FIXME: Check media element error state

    // 6. Extract the byte buffer from data
    auto buffer_result = WebIDL::get_buffer_source_copy(*data->raw_object());
    if (buffer_result.is_error())
        return WebIDL::OperationError::create(realm(), "Failed to copy buffer data"_utf16);

    auto buffer = buffer_result.release_value();

    // 7. Add data to the end of the input buffer
    MUST(m_pending_buffers.try_append(move(buffer)));

    // 8. Set the updating attribute to true
    m_updating = true;

    // 9. Queue a task to fire an event named updatestart at this SourceBuffer object
    dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));

    // 10. Asynchronously run the buffer append algorithm
    schedule_update_end();

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-abort
WebIDL::ExceptionOr<void> SourceBuffer::abort()
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed from MediaSource"_utf16);

    // 2. If the readyState attribute of the parent media source is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 3. If the updating attribute equals true, then run the following steps:
    if (m_updating) {
        // 3.1. Abort the buffer append algorithm if it is running
        m_pending_buffers.clear();

        // 3.2. Set the updating attribute to false
        m_updating = false;

        // 3.3. Queue a task to fire an event named abort at this SourceBuffer object
        dispatch_event(DOM::Event::create(realm(), EventNames::abort));

        // 3.4. Queue a task to fire an event named updateend at this SourceBuffer object
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }

    // FIXME: 4. Run the reset parser state algorithm

    // FIXME: 5. Set appendWindowStart to the presentation start time
    // FIXME: 6. Set appendWindowEnd to positive Infinity

    return {};
}

// https://w3c.github.io/media-source/#dom-sourcebuffer-remove
WebIDL::ExceptionOr<void> SourceBuffer::remove(double start, double end)
{
    // 1. If this object has been removed from the sourceBuffers attribute of the parent media source then throw an InvalidStateError exception and abort these steps.
    if (!m_media_source)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer has been removed from MediaSource"_utf16);

    // 2. If the updating attribute equals true, then throw an InvalidStateError exception and abort these steps.
    if (m_updating)
        return WebIDL::InvalidStateError::create(realm(), "SourceBuffer is currently updating"_utf16);

    // 3. If duration equals NaN, then throw a TypeError exception and abort these steps.
    if (isnan(m_media_source->duration()))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "MediaSource duration is NaN"_string };

    // 4. If start is negative or greater than duration, then throw a TypeError exception and abort these steps.
    if (start < 0 || start > m_media_source->duration())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid start value"_string };

    // 5. If end is less than or equal to start or end equals NaN, then throw a TypeError exception and abort these steps.
    if (end <= start || isnan(end))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid end value"_string };

    // 6. If the readyState attribute of the parent media source is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_media_source->ready_state() != Bindings::ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 7. Set the updating attribute to true
    m_updating = true;

    // 8. Queue a task to fire an event named updatestart at this SourceBuffer object
    dispatch_event(DOM::Event::create(realm(), EventNames::updatestart));

    // 9. Return and run the coded frame removal algorithm asynchronously
    HTML::queue_a_microtask(nullptr, GC::create_function(heap(), [this] {
        // FIXME: Actually remove coded frames from track buffers in the given range
        // For now, just complete successfully

        m_updating = false;
        dispatch_event(DOM::Event::create(realm(), EventNames::update));
        dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
    }));

    return {};
}

void SourceBuffer::schedule_update_end()
{
    // Queue a microtask to process the buffer asynchronously
    // For now, we'll just immediately fire the events since we're not actually parsing the data yet
    HTML::queue_a_microtask(nullptr, GC::create_function(heap(), [this] {
        process_append_buffer();
    }));
}

void SourceBuffer::process_append_buffer()
{
    // FIXME: This is where we would:
    // 1. Parse the media segments
    // 2. Extract coded frames
    // 3. Add them to track buffers
    // 4. Update the buffered TimeRanges

    // For now, we just simulate success by:
    // 1. Clearing the pending buffers (they've been "processed")
    m_pending_buffers.clear();

    // 2. Set updating to false
    m_updating = false;

    // 3. Fire update event
    dispatch_event(DOM::Event::create(realm(), EventNames::update));

    // 4. Notify MediaSource that data was appended so it can update the HTMLMediaElement
    if (m_media_source) {
        m_media_source->source_buffer_data_appended();
    }

    // 5. Fire updateend event
    dispatch_event(DOM::Event::create(realm(), EventNames::updateend));
}

}
