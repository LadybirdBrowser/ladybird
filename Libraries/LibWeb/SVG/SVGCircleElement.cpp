/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
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

static CSSPixels normalized_diagonal_length(CSSPixelSize viewport_size)
{
    if (viewport_size.width() == viewport_size.height())
        return viewport_size.width();
    return sqrt(((viewport_size.width() * viewport_size.width()) + (viewport_size.height() * viewport_size.height())) / 2);
}

Gfx::Path SVGCircleElement::get_path(CSSPixelSize viewport_size, CSS::ComputedValues const& computed_values)
{
    auto cx = float(computed_values.cx().to_px(viewport_size.width()));
    auto cy = float(computed_values.cy().to_px(viewport_size.height()));
    // Percentages refer to the normalized diagonal of the current SVG viewport
    // (see Units: https://svgwg.org/svg2-draft/coords.html#Units)
    auto r = float(computed_values.r().to_px(normalized_diagonal_length(viewport_size)));

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

}
