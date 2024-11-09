/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(TextTrack);

JS::NonnullGCPtr<TextTrack> TextTrack::create(JS::Realm& realm)
{
    return realm.heap().allocate<TextTrack>(realm, realm);
}

TextTrack::TextTrack(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

TextTrack::~TextTrack() = default;

void TextTrack::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextTrack);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-kind
Bindings::TextTrackKind TextTrack::kind()
{
    return m_kind;
}

void TextTrack::set_kind(Bindings::TextTrackKind kind)
{
    m_kind = kind;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-label
String TextTrack::label()
{
    return m_label;
}

void TextTrack::set_label(String label)
{
    m_label = label;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-language
String TextTrack::language()
{
    return m_language;
}

void TextTrack::set_language(String language)
{
    m_language = language;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-id
String TextTrack::id()
{
    return m_id;
}

void TextTrack::set_id(String id)
{
    m_id = id;
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-mode
Bindings::TextTrackMode TextTrack::mode()
{
    return m_mode;
}

void TextTrack::set_mode(Bindings::TextTrackMode mode)
{
    m_mode = mode;
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrack-oncuechange
void TextTrack::set_oncuechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::cuechange, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrack-oncuechange
WebIDL::CallbackType* TextTrack::oncuechange()
{
    return event_handler_attribute(HTML::EventNames::cuechange);
}

Bindings::TextTrackKind text_track_kind_from_string(String value)
{
    // https://html.spec.whatwg.org/multipage/media.html#attr-track-kind

    if (value.is_empty() || value.equals_ignoring_ascii_case("subtitles"sv)) {
        return Bindings::TextTrackKind::Subtitles;
    }
    if (value.equals_ignoring_ascii_case("captions"sv)) {
        return Bindings::TextTrackKind::Captions;
    }
    if (value.equals_ignoring_ascii_case("descriptions"sv)) {
        return Bindings::TextTrackKind::Descriptions;
    }
    if (value.equals_ignoring_ascii_case("chapters"sv)) {
        return Bindings::TextTrackKind::Chapters;
    }
    if (value.equals_ignoring_ascii_case("metadata"sv)) {
        return Bindings::TextTrackKind::Metadata;
    }

    return Bindings::TextTrackKind::Metadata;
}

}
