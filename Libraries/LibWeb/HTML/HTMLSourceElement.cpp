/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSourceElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLSourceElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLSourceElement);

HTMLSourceElement::HTMLSourceElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLSourceElement::~HTMLSourceElement() = default;

void HTMLSourceElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLSourceElement);
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:the-source-element-15
void HTMLSourceElement::inserted()
{
    // The source HTML element insertion steps, given insertedNode, are:
    Base::inserted();

    // 1. If insertedNode's parent is a media element that has no src attribute and whose networkState has the value
    //    NETWORK_EMPTY, then invoke that media element's resource selection algorithm.
    if (is<HTMLMediaElement>(parent())) {
        auto& media_element = static_cast<HTMLMediaElement&>(*parent());

        if (!media_element.has_attribute(HTML::AttributeNames::src) && media_element.network_state() == HTMLMediaElement::NetworkState::Empty)
            media_element.select_resource().release_value_but_fixme_should_propagate_errors();
    }

    // FIXME: 2. If insertedNode's next sibling is an img element and its parent is a picture element, then, count this as a
    //           relevant mutation for the img element.
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:the-source-element-16
void HTMLSourceElement::removed_from(DOM::Node* old_parent)
{
    // The source HTML element removing steps, given removedNode and oldParent, are:
    Base::removed_from(old_parent);

    // FIXME: 1. If removedNode's next sibling was an img element and oldParent is a picture element, then, count this as a
    //           relevant mutation for the img element.
}

}
