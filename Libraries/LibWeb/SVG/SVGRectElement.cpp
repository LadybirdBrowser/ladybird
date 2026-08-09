/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/SVG/SVGRectElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGRectElement);

SVGRectElement::SVGRectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

Gfx::Path SVGRectElement::get_path(CSSPixelSize viewport_size)
{
    auto computed_values = this->computed_style();
    VERIFY(computed_values);

    auto computed_width = computed_values->width();
    auto computed_height = computed_values->height();

    // FIXME: to_px rounds prematurely here - we shouldn't round to fixed point CSSPixels until converting to CSS pixel
    //        space from SVG user space - this likely extends to other SVG geometry elements as well.
    auto width = computed_width.is_length_percentage() ? computed_width.length_percentage().to_px(viewport_size.width()).to_double() : 0.0;
    auto height = computed_height.is_length_percentage() ? computed_height.length_percentage().to_px(viewport_size.height()).to_double() : 0.0;

    auto x = computed_values->x().to_px(viewport_size.width()).to_double();
    auto y = computed_values->y().to_px(viewport_size.height()).to_double();

    Gfx::Path path;
    // Non-positive dimensions disable rendering. In particular, a negative
    // width or height is an invalid geometry value rather than a rectangle
    // extending in the opposite direction.
    if (width <= 0 || height <= 0)
        return path;

    auto corner_radii = calculate_used_corner_radius_values(width, height);
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

Gfx::FloatSize SVGRectElement::calculate_used_corner_radius_values(float used_width, float used_height) const
{
    // The used values for rx and ry are determined from the computed values by following these steps in order:

    auto computed_values = this->computed_style();
    VERIFY(computed_values);

    auto computed_rx = computed_values->rx();
    auto computed_ry = computed_values->ry();

    // 1. If both rx and ry have a computed value of auto (since auto is the initial value for both properties, this
    //    will also occur if neither are specified by the author or if all author-supplied values are invalid), then
    //    the used value of both rx and ry is 0. (This will result in square corners.)
    if (computed_rx.is_auto() && computed_ry.is_auto())
        return { 0, 0 };

    // 2. Otherwise, convert specified values to absolute values as follows:
    float used_rx;
    float used_ry;

    {
        // 1. If rx is set to a length value or a percentage, but ry is auto, calculate an absolute length equivalent
        //    for rx, resolving percentages against the used width of the rectangle; the absolute value for ry is the
        //    same.
        if (!computed_rx.is_auto() && computed_ry.is_auto()) {
            used_rx = computed_rx.to_px_or_zero(CSSPixels { used_width }).to_float();
            used_ry = used_rx;
        }

        // 2. If ry is set to a length value or a percentage, but rx is auto, calculate the absolute length equivalent
        //    for ry, resolving percentages against the used height of the rectangle; the absolute value for rx is the
        //    same.
        if (!computed_ry.is_auto() && computed_rx.is_auto()) {
            used_ry = computed_ry.to_px_or_zero(CSSPixels { used_height }).to_float();
            used_rx = used_ry;
        }

        // 3. If both rx and ry were set to lengths or percentages, absolute values are generated individually,
        //    resolving rx percentages against the used width, and resolving ry percentages against the used height.
        if (!computed_rx.is_auto() && !computed_ry.is_auto()) {
            used_rx = computed_rx.to_px_or_zero(CSSPixels { used_width }).to_float();
            used_ry = computed_ry.to_px_or_zero(CSSPixels { used_height }).to_float();
        }
    }

    // 3. Finally, apply clamping to generate the used values:
    {
        // 1. If the absolute rx (after the above steps) is greater than half of the used width, then the used value of
        //    rx is half of the used width.
        if (used_rx > used_width / 2)
            used_rx = used_width / 2;

        // 2. If the absolute ry (after the above steps) is greater than half of the used height, then the used value of
        //    ry is half of the used height.
        if (used_ry > used_height / 2)
            used_ry = used_height / 2;

        // 3. Otherwise, the used values of rx and ry are the absolute values computed previously.
    }

    return { used_rx, used_ry };
}

// Reflected length accessors are generated by SVGElement's reflection macro.

}
