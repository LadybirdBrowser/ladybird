/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGTSpanElementPrototype.h>
#include <LibWeb/Layout/SVGTextBox.h>
#include <LibWeb/SVG/SVGTSpanElement.h>
#include <LibWeb/SVG/SVGTextElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTSpanElement);

SVGTSpanElement::SVGTSpanElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextPositioningElement(document, move(qualified_name))
{
}

void SVGTSpanElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTSpanElement);
    Base::initialize(realm);
}

GC::Ptr<Layout::Node> SVGTSpanElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    // Text must be within an SVG <text> element.
    if (first_flat_tree_ancestor_of_type<SVGTextElement>())
        return heap().allocate<Layout::SVGTextBox>(document(), *this, move(style));
    return {};
}

}
