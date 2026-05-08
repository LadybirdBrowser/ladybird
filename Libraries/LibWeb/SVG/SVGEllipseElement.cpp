/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGEllipseElement.h>
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
    // NB: Called during SVG layout.
    auto node = unsafe_layout_node();
    if (!node) {
        dbgln("FIXME: Null layout node in SVGEllipseElement::get_path");
        return {};
    }

    auto const& rx_computed = node->computed_values().rx();
    auto const& ry_computed = node->computed_values().ry();

    // A value of auto for both dimensions disables rendering.
    // https://svgwg.org/svg2-draft/geometry.html#RX
    if (rx_computed.is_auto() && ry_computed.is_auto())
        return {};

    auto cx = float(node->computed_values().cx().to_px(*node, viewport_size.width()));
    auto cy = float(node->computed_values().cy().to_px(*node, viewport_size.height()));

    float rx, ry;
    // When rx is auto it equals ry, and when ry is auto it equals rx.
    if (rx_computed.is_auto()) {
        ry = float(ry_computed.length_percentage().to_px(*node, viewport_size.height()));
        rx = ry;
    } else if (ry_computed.is_auto()) {
        rx = float(rx_computed.length_percentage().to_px(*node, viewport_size.width()));
        ry = rx;
    } else {
        rx = float(rx_computed.length_percentage().to_px(*node, viewport_size.width()));
        ry = float(ry_computed.length_percentage().to_px(*node, viewport_size.height()));
    }

    // A computed value of zero for either dimension disables rendering.
    if (rx == 0 || ry == 0)
        return {};

    Gfx::Path path;
    Gfx::FloatSize radii = { rx, ry };
    double x_axis_rotation = 0;
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

// https://svgwg.org/svg2-draft/shapes.html#__svg__SVGEllipseElement__cx
GC::Ref<SVGAnimatedLength> SVGEllipseElement::cx() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Cx);
}

// https://svgwg.org/svg2-draft/shapes.html#__svg__SVGEllipseElement__cy
GC::Ref<SVGAnimatedLength> SVGEllipseElement::cy() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Cy);
}

// https://svgwg.org/svg2-draft/shapes.html#__svg__SVGEllipseElement__rx
GC::Ref<SVGAnimatedLength> SVGEllipseElement::rx() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Rx);
}

// https://svgwg.org/svg2-draft/shapes.html#__svg__SVGEllipseElement__ry
GC::Ref<SVGAnimatedLength> SVGEllipseElement::ry() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Ry);
}

}
