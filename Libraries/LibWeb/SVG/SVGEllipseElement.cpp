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

Gfx::Path SVGEllipseElement::get_path(CSSPixelSize viewport_size)
{
    auto computed_values = this->computed_style();
    VERIFY(computed_values);

    auto computed_rx = computed_values->rx();
    auto computed_ry = computed_values->ry();

    float rx = computed_rx.to_px_or_zero(viewport_size.width()).to_float();
    float ry = computed_ry.to_px_or_zero(viewport_size.height()).to_float();

    // https://svgwg.org/svg2-draft/geometry.html#RxProperty
    // When the computed value of ‘rx’ is auto, the used radius is equal to the absolute length used for ry, creating a
    // circular arc. If both ‘rx’ and ‘ry’ have a computed value of auto, the used value is 0.
    if (computed_rx.is_auto())
        rx = computed_ry.to_px_or_zero(viewport_size.height()).to_float();

    // When the computed value of ‘ry’ is auto, the used radius is equal to the absolute length used for rx, creating a
    // circular arc. If both ‘rx’ and ‘ry’ have a computed value of auto, the used value is 0.
    if (computed_ry.is_auto())
        ry = computed_rx.to_px_or_zero(viewport_size.width()).to_float();

    float cx = computed_values->cx().to_px(viewport_size.width()).to_float();
    float cy = computed_values->cy().to_px(viewport_size.height()).to_float();
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
