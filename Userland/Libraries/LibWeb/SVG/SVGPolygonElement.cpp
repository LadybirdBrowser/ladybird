/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGPolygonElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGPolygonElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGPolygonElement);

SVGPolygonElement::SVGPolygonElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGPolygonElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGPolygonElement);
}

void SVGPolygonElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    SVGGeometryElement::attribute_changed(name, old_value, value);

    if (name == SVG::AttributeNames::points)
        m_points = AttributeParser::parse_points(value.value_or(String {}));
}

Gfx::Path SVGPolygonElement::get_path(CSSPixelSize)
{
    Gfx::Path path;

    if (m_points.is_empty())
        return path;

    // 1. perform an absolute moveto operation to the first coordinate pair in the list of points
    path.move_to(m_points.first());

    // 2. for each subsequent coordinate pair, perform an absolute lineto operation to that coordinate pair.
    for (size_t point_index = 1; point_index < m_points.size(); ++point_index)
        path.line_to(m_points[point_index]);

    // 3. perform a closepath command
    path.close();

    return path;
}

}
