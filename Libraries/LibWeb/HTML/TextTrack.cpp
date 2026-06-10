/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TextTrackObserver.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrack);

GC::Ref<TextTrack> TextTrack::create()
{
    return GC::Heap::the().allocate<TextTrack>();
}

TextTrack::TextTrack()
    : DOM::EventTarget()
{
}

TextTrack::~TextTrack() = default;

void TextTrack::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_observers);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-kind
TextTrackKind TextTrack::kind()
{
    return m_kind;
}

void TextTrack::set_kind(TextTrackKind kind)
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
TextTrackMode TextTrack::mode()
{
    return m_mode;
}

void TextTrack::set_mode(TextTrackMode mode)
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

TextTrackKind text_track_kind_from_string(String value)
{
    // https://html.spec.whatwg.org/multipage/media.html#attr-track-kind

    if (value.is_empty() || value.equals_ignoring_ascii_case("subtitles"sv)) {
        return TextTrackKind::Subtitles;
    }
    if (value.equals_ignoring_ascii_case("captions"sv)) {
        return TextTrackKind::Captions;
    }
    if (value.equals_ignoring_ascii_case("descriptions"sv)) {
        return TextTrackKind::Descriptions;
    }
    if (value.equals_ignoring_ascii_case("chapters"sv)) {
        return TextTrackKind::Chapters;
    }
    if (value.equals_ignoring_ascii_case("metadata"sv)) {
        return TextTrackKind::Metadata;
    }

    return TextTrackKind::Metadata;
}

}
