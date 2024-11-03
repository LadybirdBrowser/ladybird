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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGGeometryElement);
}

JS::GCPtr<Layout::Node> SVGGeometryElement::create_layout_node(CSS::StyleProperties style)
{
    return heap().allocate_without_realm<Layout::SVGGeometryBox>(document(), *this, move(style));
}

float SVGGeometryElement::get_total_length()
{
    return 0;
}

JS::NonnullGCPtr<Geometry::DOMPoint> SVGGeometryElement::get_point_at_length(float distance)
{
    (void)distance;
    return Geometry::DOMPoint::construct_impl(realm(), 0, 0, 0, 0);
}

}
