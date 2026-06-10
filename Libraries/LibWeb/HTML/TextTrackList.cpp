/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrackList.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrackList);

TextTrackList::TextTrackList()
    : DOM::EventTarget()
{
}

TextTrackList::~TextTrackList() = default;

GC::Ref<TextTrackList> TextTrackList::create()
{
    return GC::Heap::the().allocate<TextTrackList>();
}

void TextTrackList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_text_tracks);
}

GC::Ptr<TextTrack> TextTrackList::item(size_t index) const
{
    // To determine the value of an indexed property of a TextTrackList object for a given index index, the user
    // agent must return the indexth text track in the list represented by the TextTrackList object.
    if (index >= m_text_tracks.size())
        return nullptr;

    return m_text_tracks.at(index);
}

void TextTrackList::add_track(GC::Ref<TextTrack> text_track)
{
    m_text_tracks.append(text_track);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttracklist-length
size_t TextTrackList::length() const
{
    return m_text_tracks.size();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttracklist-gettrackbyid
GC::Ptr<TextTrack> TextTrackList::get_track_by_id(StringView id) const
{
    // The getTrackById(id) method must return the first TextTrack in the TextTrackList object whose id
    // IDL attribute would return a value equal to the value of the id argument.
    auto it = m_text_tracks.find_if([&](auto const& text_track) {
        return text_track->id() == id;
    });

    // When no tracks match the given argument, the method must return null.
    if (it == m_text_tracks.end())
        return nullptr;

    return *it;
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttracklist-onchange
void TextTrackList::set_onchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttracklist-onchange
WebIDL::CallbackType* TextTrackList::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttracklist-onaddtrack
void TextTrackList::set_onaddtrack(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::addtrack, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttracklist-onaddtrack
WebIDL::CallbackType* TextTrackList::onaddtrack()
{
    return event_handler_attribute(HTML::EventNames::addtrack);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttracklist-onremovetrack
void TextTrackList::set_onremovetrack(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::removetrack, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttracklist-onremovetrack
WebIDL::CallbackType* TextTrackList::onremovetrack()
{
    return event_handler_attribute(HTML::EventNames::removetrack);
}

}
