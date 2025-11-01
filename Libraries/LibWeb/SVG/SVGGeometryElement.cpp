/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGGeometryElementPrototype.h>
#include <LibWeb/Layout/SVGGeometryBox.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

SVGGeometryElement::SVGGeometryElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

void SVGGeometryElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGGeometryElement);
    Base::initialize(realm);
}

void SVGGeometryElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_path_length);
}

GC::Ptr<Layout::Node> SVGGeometryElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGGeometryBox>(document(), *this, move(style));
}

float SVGGeometryElement::get_total_length()
{
    return 0;
}

GC::Ref<Geometry::DOMPoint> SVGGeometryElement::get_point_at_length(float distance)
{
    (void)distance;
    return Geometry::DOMPoint::construct_impl(realm(), 0, 0, 0, 0);
}

GC::Ref<SVGAnimatedNumber> SVGGeometryElement::path_length()
{
    if (!m_path_length)
        m_path_length = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::pathLength, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_path_length;
}

}
