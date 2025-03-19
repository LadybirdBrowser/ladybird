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

GC_DEFINE_ALLOCATOR(HTMLSourceElement);

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

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:html-element-insertion-steps
void HTMLSourceElement::inserted()
{
    // The source HTML element insertion steps, given insertedNode, are:
    Base::inserted();

    // 1. Let parent be insertedNode's parent.
    auto* parent = this->parent();

    // 2. If parent is a media element that has no src attribute and whose networkState has the value NETWORK_EMPTY,
    //    then invoke that media element's resource selection algorithm.
    if (auto* media_element = as_if<HTMLMediaElement>(parent); media_element
        && !media_element->has_attribute(HTML::AttributeNames::src)
        && media_element->network_state() == HTMLMediaElement::NetworkState::Empty) {
        media_element->select_resource().release_value_but_fixme_should_propagate_errors();
    }

    // FIXME: 3. If parent is a picture element, then for each child of parent's children, if child is an img element, then
    //           count this as a relevant mutation for child.
}

// https://html.spec.whatwg.org/multipage/embedded-content.html#the-source-element:html-element-removing-steps
void HTMLSourceElement::removed_from(DOM::Node* old_parent, DOM::Node& old_root)
{
    // The source HTML element removing steps, given removedNode and oldParent, are:
    Base::removed_from(old_parent, old_root);

    // FIXME: 1. If oldParent is a picture element, then for each child of oldParent's children, if child is an img
    //           element, then count this as a relevant mutation for child.
}

}
