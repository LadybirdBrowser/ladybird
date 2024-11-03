/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGTextElementPrototype.h>
#include <LibWeb/Layout/SVGTextBox.h>
#include <LibWeb/SVG/SVGTextElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGTextElement);

SVGTextElement::SVGTextElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextPositioningElement(document, move(qualified_name))
{
}

void SVGTextElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextElement);
}

JS::GCPtr<Layout::Node> SVGTextElement::create_layout_node(CSS::StyleProperties style)
{
    return heap().allocate_without_realm<Layout::SVGTextBox>(document(), *this, move(style));
}

}
