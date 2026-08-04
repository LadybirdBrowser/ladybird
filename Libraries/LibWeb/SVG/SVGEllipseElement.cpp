/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGEllipseElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGEllipseElement);

SVGEllipseElement::SVGEllipseElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGEllipseElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::cx) {
        m_center_x = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::cy) {
        m_center_y = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::rx) {
        m_radius_x = AttributeParser::parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::ry) {
        m_radius_y = AttributeParser::parse_number_percentage(value.value_or({}));
    }
}

Gfx::Path SVGEllipseElement::get_path(CSSPixelSize viewport_size)
{
    float rx = m_radius_x.value_or(NumberPercentage::create_number(0)).resolve_relative_to(viewport_size.width().to_float());
    float ry = m_radius_y.value_or(NumberPercentage::create_number(0)).resolve_relative_to(viewport_size.height().to_float());
    float cx = m_center_x.value_or(NumberPercentage::create_number(0)).resolve_relative_to(viewport_size.width().to_float());
    float cy = m_center_y.value_or(NumberPercentage::create_number(0)).resolve_relative_to(viewport_size.height().to_float());
    Gfx::Path path;

    // A negative radius is invalid. If only one radius is invalid, SVG uses
    // the other valid radius for both axes; if both are invalid, rendering is
    // disabled. A computed value of zero for either dimension also disables
    // rendering.
    if (rx < 0 && ry >= 0)
        rx = ry;
    else if (ry < 0 && rx >= 0)
        ry = rx;

    if (rx <= 0 || ry <= 0)
        return path;

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

}
