/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGTextElement.h>
#include <LibWeb/Layout/SVGTextBox.h>
#include <LibWeb/SVG/SVGTextElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTextElement);

SVGTextElement::SVGTextElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextPositioningElement(document, move(qualified_name))
{
}

void SVGTextElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextElement);
    Base::initialize(realm);
}

RefPtr<Layout::Node> SVGTextElement::create_layout_node(CSS::ComputedProperties const& style)
{
    return make_ref_counted<Layout::SVGTextBox>(document(), *this, style);
}

}
