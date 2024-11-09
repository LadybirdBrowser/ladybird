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

// https://html.spec.whatwg.org/multipage/media.html#dom-track-readystate
WebIDL::UnsignedShort HTMLTrackElement::ready_state()
{
    // The readyState attribute must return the numeric value corresponding to the text track readiness state of the track element's text track, as defined by the following list:
    switch (m_track->readiness_state()) {
    case TextTrack::ReadinessState::NotLoaded:
        // NONE (numeric value 0)
        //    The text track not loaded state.
        return 0;
    case TextTrack::ReadinessState::Loading:
        // LOADING (numeric value 1)
        //    The text track loading state.
        return 1;
    case TextTrack::ReadinessState::Loaded:
        // LOADED (numeric value 2)
        //    The text track loaded state.
        return 2;
    case TextTrack::ReadinessState::FailedToLoad:
        // ERROR (numeric value 3)
        //    The text track failed to load state.
        return 3;
    }

    VERIFY_NOT_REACHED();
}

}
