/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/DecoderRegistry.h>
#include <LibMedia/PlaybackManager.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TextTrackList.h>
#include <LibWeb/HTML/VideoTrackList.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/ISOBMFFByteStreamParser.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MediaSourceExtensions/WebMByteStreamParser.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(MediaSource);

GC::Ref<MediaSource> MediaSource::create(GC::Ref<DOM::EventTarget> relevant_global_object)
{
    return GC::Heap::the().allocate<MediaSource>(relevant_global_object);
}

GC::Ref<MediaSource> MediaSource::create_for_constructor(JS::Object& relevant_global_object)
{
    auto* global_scope = HTML::window_or_worker_global_scope_from_global_object(relevant_global_object);
    VERIFY(global_scope);
    return create(global_scope->this_impl());
}

MediaSource::MediaSource(GC::Ref<DOM::EventTarget> relevant_global_object)
    : DOM::EventTarget()
    , m_source_buffers(GC::Heap::the().allocate<SourceBufferList>(*this))
    , m_active_source_buffers(GC::Heap::the().allocate<SourceBufferList>(*this))
    , m_global_object(relevant_global_object)
{
}

GC::Ptr<Bindings::Wrappable> MediaSource::relevant_global_impl() const
{
    return m_global_object;
}

MediaSource::~MediaSource() = default;

void MediaSource::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element_assigned_to);
    visitor.visit(m_source_buffers);
    visitor.visit(m_active_source_buffers);
    visitor.visit(m_global_object);
}

JS::Object& MediaSource::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

GC::Ref<DOM::Event> MediaSource::create_associated_event(Utf16FlyString const& event_name) const
{
    return DOM::Event::create(event_name,
        HighResolutionTime::current_high_resolution_time(relevant_global_object()));
}

void MediaSource::queue_a_media_source_task(GC::Ref<GC::Function<void()>> task)
{
    // FIXME: The MSE spec does not say what task source to use for its tasks. Should this use the media element's
    //        task source? We may not have access to it if we're in a worker.
    GC::Ptr<DOM::Document> document = nullptr;
    if (media_element_assigned_to() != nullptr)
        document = media_element_assigned_to()->document();

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), document, task);
}

bool MediaSource::ready_state_is_closed() const
{
    return m_ready_state == ReadyState::Closed;
}

void MediaSource::set_has_ever_been_attached()
{
    m_has_ever_been_attached = true;
}

void MediaSource::set_ready_state_to_open_and_fire_sourceopen_event()
{
    m_ready_state = ReadyState::Open;

    // AD-HOC: Notify all demuxers that we have new data coming in and cannot consider the end of the buffer to be
    //         the end of the stream.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        auto& source_buffer = *m_source_buffers->item(i);
        source_buffer.clear_reached_end_of_stream({});
    }

    queue_a_media_source_task(GC::create_function(GC::Heap::the(), [this] {
        auto event = create_associated_event(EventNames::sourceopen);
        dispatch_event(event);
    }));
}

void MediaSource::set_assigned_to_media_element(Badge<HTML::HTMLMediaElement>, HTML::HTMLMediaElement& media_element)
{
    m_media_element_assigned_to = media_element;
}

// https://w3c.github.io/media-source/#mediasource-detach
void MediaSource::detach_from_media_element(Badge<HTML::HTMLMediaElement>)
{
    // FIXME: 1. If the MediaSource was constructed in a DedicatedWorkerGlobalScope:
    //               1. Notify the MediaSource using an internal detach message posted to [[port to worker]].
    //               2. Set [[port to worker]] null.
    //               3. Set [[channel with worker]] null.
    //               4. The implicit message handler for this detach notification runs the remainder of these
    //                  steps in the DedicatedWorkerGlobalScope MediaSource.
    //           Otherwise, the MediaSource was constructed in a Window:
    //               Continue the remainder of these steps on the Window MediaSource.
    // FIXME: 2. Set [[port to main]] null.

    // 3. Set the readyState attribute to "closed".
    m_ready_state = ReadyState::Closed;

    // FIXME: 4. If this is a ManagedMediaSource, then set streaming attribute to false.

    // 5. Update duration to NaN.
    m_duration = NAN;

    // AD-HOC: Abort the buffer-append algorithm of every SourceBuffer removed below. The spec steps (copied in below)
    //         don't say to do that, but other engines implement them by running the removeSourceBuffer() steps on every
    //         SourceBuffer — and those steps abort the buffer-append algorithm when the updating attribute is true.
    //         Without this, a buffer-append task queued before detaching would still run afterwards — against a media
    //         element that the media element load algorithm has reset.
    //         https://github.com/w3c/media-source/issues/378
    for (size_t i = 0; i < m_source_buffers->length(); i++)
        m_source_buffers->item(i)->abort_if_updating({});

    // 6. Remove all the SourceBuffer objects from activeSourceBuffers.
    m_active_source_buffers->remove_all_buffers({});

    // 7. Queue a task to fire an event named removesourcebuffer at activeSourceBuffers.
    queue_a_media_source_task(GC::create_function(GC::Heap::the(), [media_source = GC::Ref(*this), source_buffers = m_active_source_buffers] {
        source_buffers->dispatch_event(media_source->create_associated_event(EventNames::removesourcebuffer));
    }));

    // 8. Remove all the SourceBuffer objects from sourceBuffers.
    m_source_buffers->remove_all_buffers({});

    // 9. Queue a task to fire an event named removesourcebuffer at sourceBuffers.
    queue_a_media_source_task(GC::create_function(GC::Heap::the(), [media_source = GC::Ref(*this), source_buffers = m_source_buffers] {
        source_buffers->dispatch_event(media_source->create_associated_event(EventNames::removesourcebuffer));
    }));

    // 10. Queue a task to fire an event named sourceclose at the MediaSource.
    queue_a_media_source_task(GC::create_function(GC::Heap::the(), [this] {
        dispatch_event(create_associated_event(EventNames::sourceclose));
    }));

    // AD-HOC: Sever the media element assignment that was established when this MediaSource was attached.
    m_media_element_assigned_to = nullptr;
}

GC::Ref<SourceBufferList> MediaSource::source_buffers()
{
    return m_source_buffers;
}

GC::Ref<SourceBufferList> MediaSource::active_source_buffers()
{
    return m_active_source_buffers;
}

Utf16String MediaSource::next_track_id()
{
    return Utf16String::number(m_next_track_id++);
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

// https://w3c.github.io/media-source/#addsourcebuffer-method
WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> MediaSource::add_source_buffer(Utf16String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty()) {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "SourceBuffer type must not be empty"_utf16
        };
    }

    // 2. If type contains a MIME type that is not supported or contains a MIME type that is not
    //    supported with the types specified for the other SourceBuffer objects in sourceBuffers,
    //    then throw a NotSupportedError exception and abort these steps.
    if (!is_type_supported(type.utf16_view())) {
        return WebIDL::NotSupportedError::create("Unsupported MIME type"_utf16);
    }

    // FIXME: 3. If the user agent can't handle any more SourceBuffer objects or if creating a SourceBuffer
    //           based on type would result in an unsupported SourceBuffer configuration, then throw a
    //           QuotaExceededError exception and abort these steps.

    // 4. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create("MediaSource is not open"_utf16);

    // 5. Let buffer be a new instance of a ManagedSourceBuffer if this is a ManagedMediaSource, or
    //    a SourceBuffer otherwise, with their respective associated resources.
    auto buffer = SourceBuffer::create(
        *this,
        HTML::AudioTrackList::create(),
        HTML::VideoTrackList::create(),
        HTML::TextTrackList::create());
    buffer->set_content_type(type.utf16_view());

    // FIXME: 6. Set buffer's [[generate timestamps flag]] to the value in the "Generate Timestamps Flag"
    //           column of the Media Source Extensions™ Byte Stream Format Registry entry that is
    //           associated with type.
    // FIXME: 7. If buffer's [[generate timestamps flag]] is true, set buffer's mode to "sequence".
    //           Otherwise, set buffer's mode to "segments".
    // 8. Append buffer to this's sourceBuffers.
    // 9. Queue a task to fire an event named addsourcebuffer at this's sourceBuffers.
    m_source_buffers->append(buffer);

    // 10. Return buffer.
    return buffer;
}

// https://w3c.github.io/media-source/#dom-mediasource-endofstream
WebIDL::ExceptionOr<void> MediaSource::end_of_stream(Optional<EndOfStreamError> const& error)
{
    // 1. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception
    //    and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create("MediaSource is not open"_utf16);

    // 2. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an
    //    InvalidStateError exception and abort these steps.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        if (m_source_buffers->item(i)->updating())
            return WebIDL::InvalidStateError::create("A SourceBuffer is still updating"_utf16);
    }

    // 3. Run the end of stream algorithm with the error parameter set to error.
    run_end_of_stream_algorithm(error);

    return {};
}

// https://w3c.github.io/media-source/#end-of-stream-algorithm
void MediaSource::run_end_of_stream_algorithm(Optional<EndOfStreamError> const& error)
{
    // 1. Change the readyState attribute value to "ended".
    m_ready_state = ReadyState::Ended;

    // 2. Queue a task to fire an event named sourceended at the MediaSource.
    queue_a_media_source_task(GC::create_function(GC::Heap::the(), [this] {
        dispatch_event(create_associated_event(EventNames::sourceended));
    }));

    // AD-HOC: Notify all demuxers that end of stream was reached, so that they can return the requisite error and
    //         allow decoders to flush all their frames.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        auto& source_buffer = *m_source_buffers->item(i);
        source_buffer.set_reached_end_of_stream({});
    }

    // 3. If error is not set:
    if (!error.has_value()) {
        // 1. Run the duration change algorithm with new duration set to the largest track buffer ranges
        //    end time across all the track buffers across all SourceBuffer objects in sourceBuffers.
        // FIXME: Implement duration change based on track buffer ranges.

        // 2. Notify the media element that it now has all of the media data.
        // FIXME: Signal to the HTMLMediaElement that all data has been provided.
        return;
    }

    // 4. If error is set to "network":
    if (error.value() == EndOfStreamError::Network) {
        // FIXME: If the HTMLMediaElement's readyState attribute equals HAVE_NOTHING:
        //            Run the "If the media data cannot be fetched at all" steps of the resource fetch algorithm.
        //        Otherwise:
        //            Run the "If the connection is interrupted after some media data has been received" steps
        //            of the resource fetch algorithm.
        return;
    }

    // 5. If error is set to "decode":
    if (error.value() == EndOfStreamError::Decode) {
        // FIXME: If the HTMLMediaElement's readyState attribute equals HAVE_NOTHING:
        //            Run the "If the media data can be fetched but is found by inspection to be in an
        //            unsupported format" steps of the resource fetch algorithm.
        //        Otherwise:
        //            Run the "If the media data can be fetched but has fatal network errors" steps of the
        //            resource fetch algorithm.
        return;
    }
}

// https://w3c.github.io/media-source/#dom-mediasource-duration
double MediaSource::duration() const
{
    // 1. If the readyState attribute is "closed" then return NaN and abort these steps.
    if (m_ready_state == ReadyState::Closed)
        return NAN;

    // 2. Return the current value of the attribute.
    return m_duration;
}

// https://w3c.github.io/media-source/#dom-mediasource-duration
WebIDL::ExceptionOr<void> MediaSource::set_duration(double new_duration)
{
    // 1. If the value being set is negative or NaN then throw a TypeError exception and abort these steps.
    if (new_duration < 0 || isnan(new_duration))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "duration must not be negative or NaN"_utf16 };

    // 2. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception
    //    and abort these steps.
    if (m_ready_state != ReadyState::Open)
        return WebIDL::InvalidStateError::create("MediaSource is not open"_utf16);

    // 3. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an
    //    InvalidStateError exception and abort these steps.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        if (m_source_buffers->item(i)->updating())
            return WebIDL::InvalidStateError::create("A SourceBuffer is still updating"_utf16);
    }

    // 4. Run the duration change algorithm with new duration set to the value being assigned to this attribute.
    run_duration_change_algorithm(new_duration);

    return {};
}

// https://w3c.github.io/media-source/#duration-change-algorithm
void MediaSource::run_duration_change_algorithm(double new_duration)
{
    // 1. If the current value of duration is equal to new duration, then return.
    if (m_duration == new_duration)
        return;

    // 2. If new duration is less than the highest presentation timestamp of any buffered coded frames
    //    for all SourceBuffer objects in sourceBuffers, then throw an InvalidStateError exception and
    //    abort these steps.
    // FIXME: Check highest presentation timestamp across all track buffers.

    // 3. Let highest end time be the largest track buffer ranges end time across all the track buffers
    //    across all SourceBuffer objects in sourceBuffers.
    // 4. If new duration is less than highest end time, then update new duration to equal highest end time.
    // FIXME: Clamp new_duration to highest end time.

    // 5. Update duration to new duration.
    m_duration = new_duration;

    // 6. Use the mirror if necessary algorithm to run the following steps in Window to update the
    //    media element's duration:
    // FIXME: Mirror to the Window when workers are supported.
    //        Update the media element's duration to new duration.
    //        Run the HTMLMediaElement duration change algorithm.
    media_element_assigned_to()->set_duration({}, new_duration);
    media_element_assigned_to()->playback_manager().set_duration(AK::Duration::from_seconds_f64(new_duration));
}

// https://w3c.github.io/media-source/#dom-mediasource-istypesupported
// AD-HOC: Returns the capabilities of the decoders that would play the type, so that the Media Capabilities API can report
//         them without asking a second time.
Optional<Media::DecoderCapabilities> MediaSource::decoder_capabilities_for_type(Utf16View type)
{
    // 1. If type is an empty string, then return false.
    if (type.is_empty())
        return {};

    // 2. If type does not contain a valid MIME type string, then return false.
    auto mime_type = MimeSniff::MimeType::parse(type);
    if (!mime_type.has_value())
        return {};

    // 3. If type contains a media type or media subtype that the MediaSource does not support, then return false.
    if (mime_type->type() != "video" && mime_type->type() != "audio")
        return {};

    using SupportsCodec = bool (*)(StringView, Media::CodecID);
    auto supports_codec = [&]() -> SupportsCodec {
        if (mime_type->subtype() == "webm")
            return WebMByteStreamParser::supports_codec;
        if (mime_type->subtype() == "mp4")
            return ISOBMFFByteStreamParser::supports_codec;
        return nullptr;
    }();
    // NB: A subtype that no byte stream format handles is the media subtype half of the step above.
    if (!supports_codec)
        return {};

    // 4. If type contains a codec that the MediaSource does not support, then return false.
    // 5. If the MediaSource does not support the specified combination of media type, media subtype, and codecs then
    //    return false.
    auto codecs_iter = mime_type->parameters().find("codecs"sv);
    if (codecs_iter == mime_type->parameters().end())
        return {};

    auto codec_strings = codecs_iter->value.bytes_as_string_view().split_view(',', SplitBehavior::KeepEmpty);
    if (codec_strings.is_empty())
        return {};

    Media::DecoderCapabilities capabilities { .smooth = true, .power_efficient = true };
    for (auto codec_string : codec_strings) {
        codec_string = codec_string.trim_whitespace();
        auto codec = Media::parse_codec_parameters_string(codec_string);
        if (!codec.has_value())
            return {};

        // AD-HOC: An underspecified codec string names a family rather than a specific codec, so we cannot confirm
        //         support in this case.
        if (!codec->is_fully_specified())
            return {};

        if (mime_type->type() == "audio" && Media::track_type_from_codec_id(codec->codec_id()) != Media::TrackType::Audio)
            return {};

        if (!supports_codec(codec_string, codec->codec_id()))
            return {};

        auto codec_capabilities = Media::decoder_capabilities(*codec);
        if (!codec_capabilities.has_value())
            return {};

        capabilities.smooth &= codec_capabilities->smooth;
        capabilities.power_efficient &= codec_capabilities->power_efficient;
    }

    // 6. Return true.
    return capabilities;
}

bool MediaSource::is_type_supported(Utf16View type)
{
    return decoder_capabilities_for_type(type).has_value();
}

}
