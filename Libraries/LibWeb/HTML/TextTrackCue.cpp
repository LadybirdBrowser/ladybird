/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TextTrackCuePrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/TextTrackCue.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(TextTrackCue);

TextTrackCue::TextTrackCue(JS::Realm& realm, JS::GCPtr<TextTrack> track)
    : DOM::EventTarget(realm)
    , m_track(track)
{
}

TextTrackCue::~TextTrackCue() = default;

void TextTrackCue::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TextTrackCue);
}

void TextTrackCue::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-starttime
void TextTrackCue::set_start_time(double start_time)
{
    // On setting, the text track cue start time must be set to the new value, interpreted in seconds;
    m_start_time = start_time;

    // FIXME: then, if the TextTrackCue object's text track cue is in a text track's list of cues, and that text track is in a media
    //        element's list of text tracks, and the media element's show poster flag is not set, then run the time marches on steps
    //        for that media element.
}

// https://html.spec.whatwg.org/multipage/media.html#dom-texttrackcue-endtime
WebIDL::ExceptionOr<void> TextTrackCue::set_end_time(double end_time)
{
    // On setting, if the new value is negative Infinity or a Not-a-Number (NaN) value, then throw a TypeError exception.
    if (end_time == -AK::Infinity<double> || isnan(end_time))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value is negative infinity or NaN"_string };

    // Otherwise, the text track cue end time must be set to the new value.
    m_end_time = end_time;

    // FIXME: Then, if the TextTrackCue object's text track cue is in a text track's list of cues, and that text track is in a media
    //        element's list of text tracks, and the media element's show poster flag is not set, then run the time marches on steps
    //        for that media element.
    return {};
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrackcue-onenter
WebIDL::CallbackType* TextTrackCue::onenter()
{
    return event_handler_attribute(HTML::EventNames::enter);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrackcue-onenter
void TextTrackCue::set_onenter(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::enter, event_handler);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrackcue-onexit
WebIDL::CallbackType* TextTrackCue::onexit()
{
    return event_handler_attribute(HTML::EventNames::exit);
}

// https://html.spec.whatwg.org/multipage/media.html#handler-texttrackcue-onexit
void TextTrackCue::set_onexit(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::exit, event_handler);
}

}
