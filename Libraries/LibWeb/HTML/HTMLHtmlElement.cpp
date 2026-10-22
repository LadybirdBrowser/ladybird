/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/Node.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLHtmlElement);

HTMLHtmlElement::HTMLHtmlElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLHtmlElement::~HTMLHtmlElement() = default;

void HTMLHtmlElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    publish_body_construction_facts();
}

void HTMLHtmlElement::publish_body_construction_facts()
{
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        if (is<HTMLBodyElement>(*child) || is<HTMLFrameSetElement>(*child))
            CSS::record_element_construction_facts(as<DOM::Element>(*child));
    }
}

}
