/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/URL.h>
#include <LibWeb/Bindings/SVGTextPathElementPrototype.h>
#include <LibWeb/Layout/SVGTextPathBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGTextPathElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTextPathElement);

SVGTextPathElement::SVGTextPathElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextContentElement(document, move(qualified_name))
{
}

GC::Ptr<SVGGeometryElement const> SVGTextPathElement::path_or_shape() const
{
    auto href = get_attribute(AttributeNames::href);
    if (!href.has_value())
        return {};
    auto url = document().url().complete_url(*href);
    if (!url.has_value())
        return {};
    return try_resolve_url_to<SVGGeometryElement const>(*url);
}

void SVGTextPathElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextPathElement);
}

void SVGTextPathElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

GC::Ptr<Layout::Node> SVGTextPathElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGTextPathBox>(document(), *this, move(style));
}

};
