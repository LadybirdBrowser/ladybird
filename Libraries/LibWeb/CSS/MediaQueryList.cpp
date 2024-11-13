/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaQueryListPrototype.h>
#include <LibWeb/CSS/MediaQueryList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventHandler.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(MediaQueryList);

JS::NonnullGCPtr<MediaQueryList> MediaQueryList::create(DOM::Document& document, Vector<NonnullRefPtr<MediaQuery>>&& media)
{
    return document.heap().allocate<MediaQueryList>(document.realm(), document, move(media));
}

MediaQueryList::MediaQueryList(DOM::Document& document, Vector<NonnullRefPtr<MediaQuery>>&& media)
    : DOM::EventTarget(document.realm())
    , m_document(document)
    , m_media(move(media))
{
    evaluate();
}

void MediaQueryList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaQueryList);
}

void MediaQueryList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://drafts.csswg.org/cssom-view/#dom-mediaquerylist-media
String MediaQueryList::media() const
{
    return serialize_a_media_query_list(m_media);
}

// https://drafts.csswg.org/cssom-view/#dom-mediaquerylist-matches
bool MediaQueryList::matches() const
{
    if (m_media.is_empty())
        return true;

    for (auto& media : m_media) {
        if (media->matches())
            return true;
    }

    return false;
}

bool MediaQueryList::evaluate()
{
    auto window = m_document->window();
    if (!window)
        return false;

    if (m_media.is_empty())
        return true;

    bool now_matches = false;
    for (auto& media : m_media) {
        now_matches = now_matches || media->evaluate(*window);
    }

    return now_matches;
}

// https://www.w3.org/TR/cssom-view/#dom-mediaquerylist-addlistener
void MediaQueryList::add_listener(JS::GCPtr<DOM::IDLEventListener> listener)
{
    // 1. If listener is null, terminate these steps.
    if (!listener)
        return;

    // 2. Append an event listener to the associated list of event listeners with type set to change,
    //    callback set to listener, and capture set to false, unless there already is an event listener
    //    in that list with the same type, callback, and capture.
    //    (NOTE: capture is set to false by default)
    add_event_listener_without_options(HTML::EventNames::change, *listener);
}

// https://www.w3.org/TR/cssom-view/#dom-mediaquerylist-removelistener
void MediaQueryList::remove_listener(JS::GCPtr<DOM::IDLEventListener> listener)
{
    // 1. Remove an event listener from the associated list of event listeners, whose type is change, callback is listener, and capture is false.
    // NOTE: While the spec doesn't technically use remove_event_listener and instead manipulates the list directly, every major engine uses remove_event_listener.
    //       This means if an event listener removes another event listener that comes after it, the removed event listener will not be invoked.
    if (listener)
        remove_event_listener_without_options(HTML::EventNames::change, *listener);
}

void MediaQueryList::set_onchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

WebIDL::CallbackType* MediaQueryList::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

}
