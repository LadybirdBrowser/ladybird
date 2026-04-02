/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/BoundingBox.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Painting {

SVGPaintable::SVGPaintable(Layout::SVGBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::SVGBox const& SVGPaintable::layout_box() const
{
    return static_cast<Layout::SVGBox const&>(layout_node());
}

// https://drafts.csswg.org/css-masking-1/#ClipPathElement
bool SVGPaintable::contributes_to_clip_path() const
{
    // If a child element is made invisible by display or visibility it does not contribute to the clipping path.
    return computed_values().visibility() == CSS::Visibility::Visible && !display().is_none();
}

// https://drafts.csswg.org/css-masking-1/#ClipPathElement
Optional<CSSPixelRect> SVGPaintable::clip_path_geometry_bounds(Gfx::AffineTransform const& additional_transform) const
{
    if (!contributes_to_clip_path())
        return {};

    // When the clipPath element contains multiple child elements, the silhouettes of the child elements are
    // logically OR'd together to create a single silhouette which is then used to restrict the region onto
    // which paint can be applied. Thus, a point is inside the clipping path if it is inside any of the
    // children of the clipPath.
    Gfx::BoundingBox<CSSPixels> bounding_box;
    for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
        auto const* svg_paintable = as_if<SVGPaintable>(child);
        if (!svg_paintable)
            return IterationDecision::Continue;

        auto child_bounds = svg_paintable->clip_path_geometry_bounds(additional_transform);
        if (!child_bounds.has_value())
            return IterationDecision::Continue;

        bounding_box.add_point(child_bounds->left(), child_bounds->top());
        bounding_box.add_point(child_bounds->right(), child_bounds->bottom());
        return IterationDecision::Continue;
    });

    if (bounding_box.has_no_points())
        return {};
    return bounding_box.to_rect();
}

CSSPixelRect SVGPaintable::compute_absolute_rect() const
{
    if (auto* svg_svg_box = layout_box().first_ancestor_of_type<Layout::SVGSVGBox>()) {
        CSSPixelRect rect { offset(), content_size() };
        for (Layout::Box const* ancestor = svg_svg_box; ancestor; ancestor = ancestor->containing_block())
            rect.translate_by(ancestor->paintable_box()->offset());
        return rect;
    }
    return PaintableBox::compute_absolute_rect();
}

ShouldAntiAlias SVGPaintable::should_anti_alias() const
{
    auto shape_rendering = computed_values().shape_rendering();
    if (first_is_one_of(shape_rendering, CSS::ShapeRendering::Optimizespeed, CSS::ShapeRendering::Crispedges))
        return ShouldAntiAlias::No;
    return ShouldAntiAlias::Yes;
}

}
