/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLFrameSetElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLFrameSetElement);

HTMLFrameSetElement::HTMLFrameSetElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFrameSetElement::~HTMLFrameSetElement() = default;

void HTMLFrameSetElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFrameSetElement);
}

void HTMLFrameSetElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, old_value, value);

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                     \
    if (name == HTML::AttributeNames::attribute_name) {             \
        element_event_handler_attribute_changed(event_name, value); \
    }
    ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
}

JS::GCPtr<DOM::EventTarget> HTMLFrameSetElement::global_event_handlers_to_event_target(FlyString const& event_name)
{
    // NOTE: This is a little weird, but IIUC document.body.onload actually refers to window.onload
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    if (DOM::is_window_reflecting_body_element_event_handler(event_name))
        return document().window();

    return *this;
}

JS::GCPtr<DOM::EventTarget> HTMLFrameSetElement::window_event_handlers_to_event_target()
{
    // All WindowEventHandlers on HTMLFrameSetElement (e.g. document.body.onrejectionhandled) are mapped to window.on{event}.
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    return document().window();
}

}
