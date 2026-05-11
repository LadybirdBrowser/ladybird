/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Bindings/SVGEllipseElement.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGEllipseElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGEllipseElement);

SVGEllipseElement::SVGEllipseElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGEllipseElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGEllipseElement);
    Base::initialize(realm);
}

Gfx::Path SVGEllipseElement::get_path(CSSPixelSize viewport_size)
{
    auto node = unsafe_layout_node();
    if (!node) {
        dbgln("FIXME: Null layout node in SVGEllipseElement::get_path");
        return {};
    }

    float rx = node->computed_values().rx().to_px_or_zero(*node, viewport_size.width()).to_float();
    float ry = node->computed_values().ry().to_px_or_zero(*node, viewport_size.height()).to_float();
    float cx = node->computed_values().cx().to_px(*node, viewport_size.width()).to_float();
    float cy = node->computed_values().cy().to_px(*node, viewport_size.height()).to_float();
    Gfx::Path path;

    // A computed value of zero for either dimension, or a computed value of auto for both dimensions, disables rendering of the element.
    if (rx == 0 || ry == 0)
        return path;

    Gfx::FloatSize radii = { rx, ry };
    float x_axis_rotation = 0;
    bool large_arc = false;
    bool sweep = true; // Note: Spec says it should be false, but it's wrong. https://github.com/w3c/svgwg/issues/765

    // 1. A move-to command to the point cx+rx,cy;
    path.move_to({ cx + rx, cy });

    // 2. arc to cx,cy+ry;
    path.elliptical_arc_to({ cx, cy + ry }, radii, x_axis_rotation, large_arc, sweep);

    // 3. arc to cx-rx,cy;
    path.elliptical_arc_to({ cx - rx, cy }, radii, x_axis_rotation, large_arc, sweep);

    // 4. arc to cx,cy-ry;
    path.elliptical_arc_to({ cx, cy - ry }, radii, x_axis_rotation, large_arc, sweep);

    // 5. arc with a segment-completing close path operation.
    path.elliptical_arc_to({ cx + rx, cy }, radii, x_axis_rotation, large_arc, sweep);

    return path;
}

// https://www.w3.org/TR/SVG11/shapes.html#EllipseElementCXAttribute
GC::Ref<SVGAnimatedLength> SVGEllipseElement::cx() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Cx);
}

// https://www.w3.org/TR/SVG11/shapes.html#EllipseElementCYAttribute
GC::Ref<SVGAnimatedLength> SVGEllipseElement::cy() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Cy);
}

// https://www.w3.org/TR/SVG11/shapes.html#EllipseElementRXAttribute
GC::Ref<SVGAnimatedLength> SVGEllipseElement::rx() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Rx);
}

// https://www.w3.org/TR/SVG11/shapes.html#EllipseElementRYAttribute
GC::Ref<SVGAnimatedLength> SVGEllipseElement::ry() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Ry);
}

}
