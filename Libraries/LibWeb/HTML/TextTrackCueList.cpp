/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextTrackCueListPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrackCueList.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(TextTrackCueList);

TextTrackCueList::TextTrackCueList(JS::Realm& realm)
    : DOM::EventTarget(realm, MayInterfereWithIndexedPropertyAccess::Yes)
{
}

TextTrackCueList::~TextTrackCueList() = default;

void TextTrackCueList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextTrackCueList);
}

void TextTrackCueList::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cues);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-item
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> TextTrackCueList::internal_get_own_property(JS::PropertyKey const& property_name) const
{
    // To determine the value of an indexed property for a given index index, the user agent must return the indexth text track cue in the list
    // represented by the TextTrackCueList object.
    if (property_name.is_number()) {
        if (auto index = property_name.as_number(); index < m_cues.size()) {
            JS::PropertyDescriptor descriptor;
            descriptor.value = m_cues.at(index);

            return descriptor;
        }
    }

    return Base::internal_get_own_property(property_name);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-length
size_t TextTrackCueList::length() const
{
    // The length attribute must return the number of cues in the list represented by the TextTrackCueList object.
    return m_cues.size();
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcuelist-getcuebyid
JS::GCPtr<TextTrackCue> TextTrackCueList::get_cue_by_id(StringView id) const
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
