/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(MediaSource);

WebIDL::ExceptionOr<GC::Ref<MediaSource>> MediaSource::construct_impl(JS::Realm& realm)
{
    return realm.create<MediaSource>(realm);
}

MediaSource::MediaSource(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

MediaSource::~MediaSource() = default;

void MediaSource::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSource);
    Base::initialize(realm);

    // Initialize source buffer lists
    m_source_buffers = realm.create<SourceBufferList>(realm);
    m_active_source_buffers = realm.create<SourceBufferList>(realm);
}

void MediaSource::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source_buffers);
    visitor.visit(m_active_source_buffers);
    visitor.visit(m_media_element);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceopen
void MediaSource::set_onsourceopen(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceopen, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceopen
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceopen()
{
    return event_handler_attribute(EventNames::sourceopen);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceended
void MediaSource::set_onsourceended(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceended, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceended
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceended()
{
    return event_handler_attribute(EventNames::sourceended);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceclose
void MediaSource::set_onsourceclose(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceclose, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceclose
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceclose()
{
    return event_handler_attribute(EventNames::sourceclose);
}

// https://w3c.github.io/media-source/#dom-mediasource-istypesupported
bool MediaSource::is_type_supported(JS::VM&, String const& type)
{
    // 1. If type is an empty string, then return false.
    if (type.is_empty())
        return false;

    // 2. If type does not contain a valid MIME type string, then return false.
    auto mime_type = MimeSniff::MimeType::parse(type);
    if (!mime_type.has_value())
        return false;

    // 3. If type contains a media type or media subtype that the MediaSource does not support, then return false.
    auto const& essence = mime_type->essence();
    bool container_supported = essence.starts_with_bytes("video/webm"sv) ||
        essence.starts_with_bytes("audio/webm"sv) ||
        essence.starts_with_bytes("video/mp4"sv) ||
        essence.starts_with_bytes("audio/mp4"sv);

    if (!container_supported)
        return false;

    // FIXME: 4. If type contains a codec that the MediaSource does not support, then return false.
    // TODO: Parse codec string and check with FFmpeg capabilities

    // FIXME: 5. If the MediaSource does not support the specified combination of media type, media
    //    subtype, and codecs then return false.

    // 6. Return true.
    return true;
}

// https://w3c.github.io/media-source/#dom-mediasource-addsourcebuffer
WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> MediaSource::add_source_buffer(String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Type cannot be empty"_string };

    // 2. If type contains a MIME type that is not supported, throw a NotSupportedError and abort these steps.
    if (!is_type_supported(realm().vm(), type)) {
        auto message = MUST(String::formatted("Type '{}' is not supported", type));
        return WebIDL::NotSupportedError::create(realm(), Utf16String::from_utf8(message));
    }

    // 3. If the readyState attribute is not in the "open" state then throw an InvalidStateError and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 4. Create a new SourceBuffer object and associated resources.
    auto mime_type = MimeSniff::MimeType::parse(type).value();
    auto source_buffer = realm().create<SourceBuffer>(realm(), *this, mime_type);

    // 5. Add the SourceBuffer to sourceBuffers and fire addsourcebuffer event.
    m_source_buffers->add(source_buffer);

    // 6. Return the created SourceBuffer.
    return source_buffer;
}

// https://w3c.github.io/media-source/#dom-mediasource-removesourcebuffer
WebIDL::ExceptionOr<void> MediaSource::remove_source_buffer(SourceBuffer& buffer)
{
    // 1. If sourceBuffer specifies an object that is not in sourceBuffers then throw a NotFoundError and abort these steps.
    if (!m_source_buffers->contains(buffer))
        return WebIDL::NotFoundError::create(realm(), "SourceBuffer not found in sourceBuffers list"_utf16);

    // 2. If the sourceBuffer.updating attribute equals true, throw an InvalidStateError and abort these steps.
    if (buffer.updating())
        return WebIDL::InvalidStateError::create(realm(), "Cannot remove SourceBuffer while updating"_utf16);

    // 3. Let SourceBuffer audioTracks list equal the AudioTrackList object returned by sourceBuffer.audioTracks.
    // 4. If the SourceBuffer audioTracks list is not empty, then run the following steps:
    // FIXME: Implement track removal

    // 5. Let SourceBuffer videoTracks list equal the VideoTrackList object returned by sourceBuffer.videoTracks.
    // 6. If the SourceBuffer videoTracks list is not empty, then run the following steps:
    // FIXME: Implement track removal

    // 7. Let SourceBuffer textTracks list equal the TextTrackList object returned by sourceBuffer.textTracks.
    // 8. If the SourceBuffer textTracks list is not empty, then run the following steps:
    // FIXME: Implement track removal

    // 9. Remove sourceBuffer from sourceBuffers and fire a removesourcebuffer event.
    m_source_buffers->remove(buffer);
    m_active_source_buffers->remove(buffer);

    return {};
}

// https://w3c.github.io/media-source/#dom-mediasource-endofstream
WebIDL::ExceptionOr<void> MediaSource::end_of_stream(Optional<Bindings::EndOfStreamError> error)
{
    // 1. If the readyState attribute is not in the "open" state then throw an InvalidStateError and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 2. If the updating attribute equals true on any SourceBuffer in sourceBuffers, throw an InvalidStateError and abort these steps.
    for (size_t i = 0; i < m_source_buffers->length(); ++i) {
        auto source_buffer = m_source_buffers->item(i);
        if (source_buffer && source_buffer->updating())
            return WebIDL::InvalidStateError::create(realm(), "A SourceBuffer is updating"_utf16);
    }

    // 3. Run the duration change algorithm with new duration set to the largest track buffer ranges end time across all track buffers.
    // FIXME: Calculate actual duration from track buffers

    // 4. Notify the media element that it now has all of the media data.
    // FIXME: Implement

    // 5. If error is set, then run the following steps:
    if (error.has_value()) {
        // 5.1. Update duration attribute to NaN.
        m_duration = NAN;

        // 5.2. If error is "network", fire an error event named "error" at the media element with code MEDIA_ERR_NETWORK.
        // 5.3. If error is "decode", fire an error event named "error" at the media element with code MEDIA_ERR_DECODE.
        // FIXME: Fire appropriate error event on media element
    }

    // 6. Set the readyState attribute to "ended".
    set_ready_state(ReadyState::Ended);

    return {};
}

// https://w3c.github.io/media-source/#dom-mediasource-setliveseekablerange
WebIDL::ExceptionOr<void> MediaSource::set_live_seekable_range(double start, double end)
{
    // 1. If the readyState attribute is not "open" throw an InvalidStateError and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 2. If start is negative or greater than end, throw a TypeError and abort these steps.
    if (start < 0 || start > end)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid range"_string };

    // 3. Set live seekable range to be a new normalized TimeRanges object containing a single range whose start position is start and end position is end.
    m_has_live_seekable_range = true;
    m_live_seekable_range_start = start;
    m_live_seekable_range_end = end;

    return {};
}

// https://w3c.github.io/media-source/#dom-mediasource-clearliveseekablerange
WebIDL::ExceptionOr<void> MediaSource::clear_live_seekable_range()
{
    // 1. If the readyState attribute is not "open" throw an InvalidStateError and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 2. If live seekable range contains a range, clear live seekable range.
    m_has_live_seekable_range = false;
    m_live_seekable_range_start = 0;
    m_live_seekable_range_end = 0;

    return {};
}

// https://w3c.github.io/media-source/#dom-mediasource-duration
WebIDL::ExceptionOr<void> MediaSource::set_duration(double new_duration)
{
    // 1. If the value being set is negative or NaN then throw a TypeError and abort these steps.
    // NOTE: Infinity is explicitly allowed for live streams
    if ((new_duration < 0 && !isinf(new_duration)) || isnan(new_duration))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Duration must be non-negative (or Infinity) and not NaN"_string };

    // 2. If the readyState attribute is not "open" then throw an InvalidStateError and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource readyState is not 'open'"_utf16);

    // 3. If the updating attribute equals true on any SourceBuffer in sourceBuffers, throw an InvalidStateError and abort these steps.
    for (size_t i = 0; i < m_source_buffers->length(); ++i) {
        auto source_buffer = m_source_buffers->item(i);
        if (source_buffer && source_buffer->updating())
            return WebIDL::InvalidStateError::create(realm(), "A SourceBuffer is updating"_utf16);
    }

    // 4. Run the duration change algorithm with new duration set to the value being assigned to this attribute.
    m_duration = new_duration;

    // Update media element's duration and fire durationchange event
    if (m_media_element) {
        m_media_element->set_duration_from_media_source({}, new_duration);
    }

    return {};
}

// Internal methods

void MediaSource::set_ready_state(ReadyState new_state)
{
    if (m_ready_state == new_state)
        return;

    auto old_state = m_ready_state;
    m_ready_state = new_state;

    // Fire appropriate events
    if (new_state == ReadyState::Open && old_state == ReadyState::Closed) {
        dispatch_event(DOM::Event::create(realm(), EventNames::sourceopen));
    } else if (new_state == ReadyState::Ended && old_state == ReadyState::Open) {
        dispatch_event(DOM::Event::create(realm(), EventNames::sourceended));
    } else if (new_state == ReadyState::Closed && old_state != ReadyState::Closed) {
        dispatch_event(DOM::Event::create(realm(), EventNames::sourceclose));
    }
}

void MediaSource::attach_to_media_element(HTML::HTMLMediaElement& element)
{
    m_media_element = &element;

    // Transition to "open" state asynchronously
    // Queue a task to fire sourceopen event
    element.queue_a_media_element_task([this] {
        set_ready_state(ReadyState::Open);
    });
}

void MediaSource::detach_from_media_element()
{
    // Abort all SourceBuffers
    for (size_t i = 0; i < m_source_buffers->length(); ++i) {
        auto source_buffer = m_source_buffers->item(i);
        if (source_buffer) {
            // FIXME: Call abort() on each source buffer
        }
    }

    set_ready_state(ReadyState::Closed);
    m_media_element = nullptr;
}

void MediaSource::source_buffer_data_appended()
{
    // This is called when a SourceBuffer successfully appends data
    // We need to notify the HTMLMediaElement to update its state

    if (!m_media_element)
        return;

    // Update the media element's duration from the MediaSource
    if (!isnan(m_duration)) {
        m_media_element->set_duration_from_media_source({}, m_duration);
    }

    // Update the media element's readyState based on buffered data
    // This will fire the appropriate events (loadedmetadata, loadeddata, canplay)
    m_media_element->update_ready_state_from_media_source({});
}

}
