/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGRectElement.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGRectElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGRectElement);

SVGRectElement::SVGRectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGRectElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGRectElement);
    Base::initialize(realm);
}

Gfx::Path SVGRectElement::get_path(CSSPixelSize viewport_size)
{
    auto node = unsafe_layout_node();
    if (!node) {
        dbgln("FIXME: Null layout node in SVGRectElement::get_path");
        return {};
    }

    float width = node->computed_values().width().to_px(*node, viewport_size.width()).to_float();
    float height = node->computed_values().height().to_px(*node, viewport_size.height()).to_float();
    float x = node->computed_values().x().to_px(*node, viewport_size.width()).to_float();
    float y = node->computed_values().y().to_px(*node, viewport_size.height()).to_float();

    Gfx::Path path;
    // If width or height is zero, rendering is disabled.
    if (width == 0 || height == 0)
        return path;

    auto corner_radii = calculate_used_corner_radius_values(*node, viewport_size, width, height);
    float rx = corner_radii.width();
    float ry = corner_radii.height();

    // 1. perform an absolute moveto operation to location (x+rx,y);
    path.move_to({ x + rx, y });

    // 2, perform an absolute horizontal lineto with parameter x+width-rx;
    path.horizontal_line_to(x + width - rx);

    // 3. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation to coordinate (x+width,y+ry),
    //    where rx and ry are used as the equivalent parameters to the elliptical arc command,
    //    the x-axis-rotation and large-arc-flag are set to zero,
    //    the sweep-flag is set to one;
    double x_axis_rotation = 0;
    bool large_arc_flag = false;
    bool sweep_flag = true;
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x + width, y + ry }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    // 4. perform an absolute vertical lineto parameter y+height-ry;
    path.vertical_line_to(y + height - ry);

    // 5. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation to coordinate (x+width-rx,y+height),
    //    using the same parameters as previously;
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x + width - rx, y + height }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    // 6. perform an absolute horizontal lineto parameter x+rx;
    path.horizontal_line_to(x + rx);

    // 7. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation to coordinate (x,y+height-ry),
    //    using the same parameters as previously;
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x, y + height - ry }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    // 8. perform an absolute vertical lineto parameter y+ry
    path.vertical_line_to(y + ry);

    // 9. if both rx and ry are greater than zero,
    //    perform an absolute elliptical arc operation with a segment-completing close path operation,
    //    using the same parameters as previously.
    if (rx > 0 && ry > 0)
        path.elliptical_arc_to({ x + rx, y }, corner_radii, x_axis_rotation, large_arc_flag, sweep_flag);

    path.close();

    return path;
}

Gfx::FloatSize SVGRectElement::calculate_used_corner_radius_values(Layout::Node const& node, CSSPixelSize viewport_size, float width, float height) const
{
    auto const& computed_values = node.computed_values();

    // 1. Let rx and ry be length values.
    float rx = 0;
    float ry = 0;

    // 2. If neither ‘rx’ nor ‘ry’ are properly specified, then set both rx and ry to 0. (This will result in square corners.)
    if (computed_values.rx().is_auto() && computed_values.ry().is_auto()) {
        rx = 0;
        ry = 0;
    }
    // 3. Otherwise, if a properly specified value is provided for ‘rx’, but not for ‘ry’, then set both rx and ry to the value of ‘rx’.
    else if (!computed_values.rx().is_auto() && computed_values.ry().is_auto()) {
        rx = computed_values.rx().to_px_or_zero(node, viewport_size.width()).to_float();
        ry = rx;
    }
    // 4. Otherwise, if a properly specified value is provided for ‘ry’, but not for ‘rx’, then set both rx and ry to the value of ‘ry’.
    else if (!computed_values.ry().is_auto() && computed_values.rx().is_auto()) {
        ry = computed_values.ry().to_px_or_zero(node, viewport_size.height()).to_float();
        rx = ry;
    }
    // 5. Otherwise, both ‘rx’ and ‘ry’ were specified properly. Set rx to the value of ‘rx’ and ry to the value of ‘ry’.
    else {
        rx = computed_values.rx().to_px_or_zero(node, viewport_size.width()).to_float();
        ry = computed_values.ry().to_px_or_zero(node, viewport_size.height()).to_float();
    }

    // 6. If rx is greater than half of ‘width’, then set rx to half of ‘width’.
    auto half_width = width / 2;
    if (rx > half_width)
        rx = half_width;

    // 7. If ry is greater than half of ‘height’, then set ry to half of ‘height’.
    auto half_height = height / 2;
    if (ry > half_height)
        ry = half_height;

    // 8. The effective values of ‘rx’ and ‘ry’ are rx and ry, respectively.
    return { rx, ry };
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementXAttribute
GC::Ref<SVGAnimatedLength> SVGRectElement::x() const
{
    return svg_animated_length_for_property(CSS::PropertyID::X);
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementYAttribute
GC::Ref<SVGAnimatedLength> SVGRectElement::y() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Y);
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementWidthAttribute
GC::Ref<SVGAnimatedLength> SVGRectElement::width() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Width);
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementHeightAttribute
GC::Ref<SVGAnimatedLength> SVGRectElement::height() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Height);
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementRXAttribute
GC::Ref<SVGAnimatedLength> SVGRectElement::rx() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Rx);
}

// https://www.w3.org/TR/SVG11/shapes.html#RectElementRYAttribute
GC::Ref<SVGAnimatedLength> SVGRectElement::ry() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Ry);
}

}
