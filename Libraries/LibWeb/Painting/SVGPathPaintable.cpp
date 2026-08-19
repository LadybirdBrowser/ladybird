/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Quad.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::Painting {

NonnullRefPtr<SVGPathPaintable> SVGPathPaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGPathPaintable(layout_box));
}

SVGPathPaintable::SVGPathPaintable(Layout::Box const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

Optional<CSSPixelRect> SVGPathPaintable::clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const
{
    if (!contributes_to_clip_path() || !computed_path().has_value())
        return {};

    auto path = computed_path()->copy_transformed(additional_transform);
    return path.bounding_box().to_type<CSSPixels>();
}

}
