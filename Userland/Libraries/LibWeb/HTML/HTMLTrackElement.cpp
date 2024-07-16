/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTrackElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLTrackElement.h>
#include <LibWeb/HTML/TextTrack.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLTrackElement);

HTMLTrackElement::HTMLTrackElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    m_track = TextTrack::create(document.realm());
}

HTMLTrackElement::~HTMLTrackElement() = default;

void HTMLTrackElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTrackElement);
}

void HTMLTrackElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

void HTMLTrackElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, old_value, value);

    // https://html.spec.whatwg.org/multipage/media.html#sourcing-out-of-band-text-tracks
    // As the kind, label, and srclang attributes are set, changed, or removed, the text track must update accordingly, as per the definitions above.
    if (name.equals_ignoring_ascii_case("kind"sv)) {
        m_track->set_kind(text_track_kind_from_string(value.value_or({})));
    } else if (name.equals_ignoring_ascii_case("label"sv)) {
        m_track->set_label(value.value_or({}));
    } else if (name.equals_ignoring_ascii_case("srclang"sv)) {
        m_track->set_language(value.value_or({}));
    }

    // https://html.spec.whatwg.org/multipage/media.html#dom-texttrack-id
    // For tracks that correspond to track elements, the track's identifier is the value of the element's id attribute, if any.
    if (name.equals_ignoring_ascii_case("id"sv)) {
        m_track->set_id(value.value_or({}));
    }
}

}
