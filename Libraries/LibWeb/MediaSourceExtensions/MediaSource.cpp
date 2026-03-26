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
