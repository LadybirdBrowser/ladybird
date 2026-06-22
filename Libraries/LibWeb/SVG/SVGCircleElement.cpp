/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGCircleElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGCircleElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGCircleElement);

SVGCircleElement::SVGCircleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGCircleElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGCircleElement);
    Base::initialize(realm);
}

static CSSPixels normalized_diagonal_length(CSSPixelSize viewport_size)
{
    if (viewport_size.width() == viewport_size.height())
        return viewport_size.width();
    return sqrt(((viewport_size.width() * viewport_size.width()) + (viewport_size.height() * viewport_size.height())) / 2);
}

Gfx::Path SVGCircleElement::get_path(CSSPixelSize viewport_size)
{
    // NB: Called during SVG layout.
    auto node = unsafe_layout_node();
    if (!node) {
        dbgln("FIXME: Null layout node in SVGCircleElement::get_path");
        return {};
    }

    auto cx = float(node->computed_values().cx().to_px(viewport_size.width()));
    auto cy = float(node->computed_values().cy().to_px(viewport_size.height()));
    // Percentages refer to the normalized diagonal of the current SVG viewport
    // (see Units: https://svgwg.org/svg2-draft/coords.html#Units)
    auto r = float(node->computed_values().r().to_px(normalized_diagonal_length(viewport_size)));

    // A zero radius disables rendering.
    if (r == 0)
        return {};

    Gfx::Path path;
    bool large_arc = false;
    bool sweep = true;

    // 1. A move-to command to the point cx+r,cy;
    path.move_to({ cx + r, cy });

    // 2. arc to cx,cy+r;
    path.arc_to({ cx, cy + r }, r, large_arc, sweep);

    // 3. arc to cx-r,cy;
    path.arc_to({ cx - r, cy }, r, large_arc, sweep);

    // 4. arc to cx,cy-r;
    path.arc_to({ cx, cy - r }, r, large_arc, sweep);

    // 5. arc with a segment-completing close path operation.
    path.arc_to({ cx + r, cy }, r, large_arc, sweep);

    return path;
}

// https://www.w3.org/TR/SVG11/shapes.html#CircleElementCXAttribute
GC::Ref<SVGAnimatedLength> SVGCircleElement::cx() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Cx);
}

// https://www.w3.org/TR/SVG11/shapes.html#CircleElementCYAttribute
GC::Ref<SVGAnimatedLength> SVGCircleElement::cy() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Cy);
}

// https://www.w3.org/TR/SVG11/shapes.html#CircleElementRAttribute
GC::Ref<SVGAnimatedLength> SVGCircleElement::r() const
{
    return svg_animated_length_for_property(CSS::PropertyID::R);
}

}
