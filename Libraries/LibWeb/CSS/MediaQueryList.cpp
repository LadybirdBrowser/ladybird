/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/MediaQueryList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaQueryList);

GC::Ref<MediaQueryList> MediaQueryList::create(DOM::Document& document, RustMediaList media)
{
    return GC::Heap::the().allocate<MediaQueryList>(document, move(media));
}

MediaQueryList::MediaQueryList(DOM::Document& document, RustMediaList media)
    : DOM::EventTarget()
    , m_document(document)
    , m_media(move(media))
{
    evaluate();
}

GC::Ptr<Bindings::Wrappable> MediaQueryList::relevant_global_impl() const
{
    return m_document->window();
}

void MediaQueryList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://drafts.csswg.org/cssom-view/#dom-mediaquerylist-media
Utf16String MediaQueryList::media() const
{
    return m_media.media_text();
}

// https://drafts.csswg.org/cssom-view/#dom-mediaquerylist-matches
bool MediaQueryList::matches() const
{
    bool did_match = m_media.matches();

    // NOTE: If our document is inside a frame, we need to update layout
    //       since that may cause our frame (and thus viewport) to resize.
    if (auto container_document = m_document->container_document()) {
        container_document->update_layout(DOM::UpdateLayoutReason::MediaQueryListMatches);
        m_media.evaluate(m_document);
    }

    bool now_matches = m_media.matches();
    if (did_match != now_matches)
        m_has_changed_state = true;

    return now_matches;
}

bool MediaQueryList::evaluate()
{
    return m_media.evaluate(m_document);
}

// https://www.w3.org/TR/cssom-view/#dom-mediaquerylist-addlistener
void MediaQueryList::add_listener(GC::Ptr<DOM::IDLEventListener> listener)
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
void MediaQueryList::remove_listener(GC::Ptr<DOM::IDLEventListener> listener)
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
