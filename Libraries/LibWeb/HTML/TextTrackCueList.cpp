/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextTrackCueList.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrackCueList.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TextTrackCueList);

TextTrackCueList::TextTrackCueList()
    : DOM::EventTarget()
{
}

TextTrackCueList::~TextTrackCueList() = default;

void TextTrackCueList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cues);
}

Optional<JS::Value> TextTrackCueList::item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const
{
    // To determine the value of an indexed property for a given index index, the user agent must return the indexth text track cue in the list
    // represented by the TextTrackCueList object.
    if (index >= m_cues.size())
        return {};

    return Bindings::wrap(wrapper_world, realm, m_cues.at(index));
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-length
size_t TextTrackCueList::length() const
{
    // The length attribute must return the number of cues in the list represented by the TextTrackCueList object.
    return m_cues.size();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-getcuebyid
GC::Ptr<TextTrackCue> TextTrackCueList::get_cue_by_id(StringView id) const
{
    // The getCueById(id) method, when called with an argument other than the empty string, must return the first text track cue in the list
    // represented by the TextTrackCueList object whose text track cue identifier is id, if any, or null otherwise. If the argument is the
    // empty string, then the method must return null.
    if (id.is_empty())
        return nullptr;

    auto it = m_cues.find_if([&](auto const& cue) {
        return cue->id() == id;
    });

    if (it == m_cues.end())
        return nullptr;

    return *it;
}

}
