/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TextTrackObserver.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrack);

GC::Ref<TextTrack> TextTrack::create(JS::Realm& realm)
{
    return realm.create<TextTrack>(realm);
}

TextTrack::TextTrack(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

TextTrack::~TextTrack() = default;

void TextTrack::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextTrack);
    Base::initialize(realm);
}

void TextTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_observers);
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
Utf16String const& TextTrack::label()
{
    return m_label;
}

void TextTrack::set_label(Utf16String label)
{
    m_label = move(label);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-language
Utf16String const& TextTrack::language()
{
    return m_language;
}

void TextTrack::set_language(Utf16String language)
{
    m_language = move(language);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-id
Utf16String const& TextTrack::id()
{
    return m_id;
}

void TextTrack::set_id(Utf16String id)
{
    m_id = move(id);
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

void TextTrack::set_readiness_state(ReadinessState readiness_state)
{
    m_readiness_state = readiness_state;

    for (auto observer : m_observers) {
        if (auto callback = observer->track_readiness_observer())
            callback->function()(m_readiness_state);
    }
}

void TextTrack::register_observer(Badge<TextTrackObserver>, TextTrackObserver& observer)
{
    auto result = m_observers.set(observer);
    VERIFY(result == AK::HashSetResult::InsertedNewEntry);
}

void TextTrack::unregister_observer(Badge<TextTrackObserver>, TextTrackObserver& observer)
{
    bool was_removed = m_observers.remove(observer);
    VERIFY(was_removed);
}

static bool equals_ignoring_ascii_case(Utf16View string, StringView ascii_string)
{
    if (string.length_in_code_units() != ascii_string.length())
        return false;

    for (size_t i = 0; i < string.length_in_code_units(); ++i) {
        if (AK::to_ascii_lowercase(string.code_unit_at(i)) != AK::to_ascii_lowercase(ascii_string[i]))
            return false;
    }

    return true;
}

Bindings::TextTrackKind text_track_kind_from_string(Utf16View value)
{
    // https://html.spec.whatwg.org/multipage/media.html#attr-track-kind

    if (value.is_empty() || equals_ignoring_ascii_case(value, "subtitles"sv)) {
        return Bindings::TextTrackKind::Subtitles;
    }
    if (equals_ignoring_ascii_case(value, "captions"sv)) {
        return Bindings::TextTrackKind::Captions;
    }
    if (equals_ignoring_ascii_case(value, "descriptions"sv)) {
        return Bindings::TextTrackKind::Descriptions;
    }
    if (equals_ignoring_ascii_case(value, "chapters"sv)) {
        return Bindings::TextTrackKind::Chapters;
    }
    if (equals_ignoring_ascii_case(value, "metadata"sv)) {
        return Bindings::TextTrackKind::Metadata;
    }

    return Bindings::TextTrackKind::Metadata;
}

}
