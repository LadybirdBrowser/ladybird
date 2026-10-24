/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGPolygonElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPolygonElement);

SVGPolygonElement::SVGPolygonElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGPolygonElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGAnimatedPoints::visit_edges(visitor);
}

void SVGPolygonElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    SVGAnimatedPoints::attribute_changed(name, namespace_);

    if (name == SVG::AttributeNames::points) {
        m_points = parse_points(value.value_or({}));
        set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
    }
}

}
