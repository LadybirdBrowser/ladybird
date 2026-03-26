/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSourcePrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::MediaSourceExtensions {

using Bindings::ReadyState;

GC_DEFINE_ALLOCATOR(MediaSource);

WebIDL::ExceptionOr<GC::Ref<MediaSource>> MediaSource::construct_impl(JS::Realm& realm)
{
    return realm.create<MediaSource>(realm);
}

MediaSource::MediaSource(JS::Realm& realm)
    : DOM::EventTarget(realm)
    , m_source_buffers(realm.create<SourceBufferList>(realm))
{
}

MediaSource::~MediaSource() = default;

void MediaSource::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSource);
    Base::initialize(realm);
}

void MediaSource::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element_assigned_to);
    visitor.visit(m_source_buffers);
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

ReadyState MediaSource::ready_state() const
{
    return m_ready_state;
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

    queue_a_media_source_task(GC::create_function(heap(), [this] {
        auto event = DOM::Event::create(realm(), EventNames::sourceopen);
        dispatch_event(event);
    }));
}

void MediaSource::set_assigned_to_media_element(Badge<HTML::HTMLMediaElement>, HTML::HTMLMediaElement& media_element)
{
    m_media_element_assigned_to = media_element;
}

void MediaSource::unassign_from_media_element(Badge<HTML::HTMLMediaElement>)
{
    m_media_element_assigned_to = nullptr;
}

GC::Ref<SourceBufferList> MediaSource::source_buffers()
{
    return m_source_buffers;
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
WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> MediaSource::add_source_buffer(String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty()) {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "SourceBuffer type must not be empty"sv
        };
    }

    // 2. If type contains a MIME type that is not supported or contains a MIME type that is not
    //    supported with the types specified for the other SourceBuffer objects in sourceBuffers,
    //    then throw a NotSupportedError exception and abort these steps.
    if (!is_type_supported(type)) {
        return WebIDL::NotSupportedError::create(realm(), "Unsupported MIME type"_utf16);
    }

    // FIXME: 3. If the user agent can't handle any more SourceBuffer objects or if creating a SourceBuffer
    //           based on type would result in an unsupported SourceBuffer configuration, then throw a
    //           QuotaExceededError exception and abort these steps.

    // 4. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (ready_state() != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 5. Let buffer be a new instance of a ManagedSourceBuffer if this is a ManagedMediaSource, or
    //    a SourceBuffer otherwise, with their respective associated resources.
    auto buffer = realm().create<SourceBuffer>(realm());

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

// https://w3c.github.io/media-source/#dom-mediasource-istypesupported
bool MediaSource::is_type_supported(String const& type)
{
    // 1. If type is an empty string, then return false.
    if (type.is_empty())
        return false;

    // 2. If type does not contain a valid MIME type string, then return false.
    auto mime_type = MimeSniff::MimeType::parse(type);
    if (!mime_type.has_value())
        return false;

    // FIXME: Ask LibMedia about what it supports instead of hardcoding this.

    // 3. If type contains a media type or media subtype that the MediaSource does not support, then
    //    return false.
    auto type_and_subtype_are_supported = [&] {
        if (mime_type->type() == "video" && mime_type->subtype() == "webm")
            return true;
        if (mime_type->type() == "audio" && mime_type->subtype() == "webm")
            return true;
        return false;
    }();
    if (!type_and_subtype_are_supported)
        return false;

    // 4. If type contains a codec that the MediaSource does not support, then return false.
    // 5. If the MediaSource does not support the specified combination of media type, media
    //    subtype, and codecs then return false.
    auto codecs_iter = mime_type->parameters().find("codecs"sv);
    if (codecs_iter == mime_type->parameters().end())
        return false;
    auto codecs = codecs_iter->value.bytes_as_string_view();
    auto had_unsupported_codec = false;
    codecs.for_each_split_view(',', SplitBehavior::Nothing, [&](auto const& codec) {
        if (!codec.starts_with("vp9"sv) && !codec.starts_with("vp09"sv) && !codec.starts_with("opus"sv)) {
            had_unsupported_codec = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (had_unsupported_codec)
        return false;

    // 6. Return true.
    return true;
}

}
