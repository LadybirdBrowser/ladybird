/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/BoundingBox.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

static bool kind_supports_svg_masking(Layout::RustFFI::PaintableKind kind)
{
    switch (kind) {
    case Layout::RustFFI::PaintableKind::SVGGraphicsPaintable:
    case Layout::RustFFI::PaintableKind::SVGPathPaintable:
    case Layout::RustFFI::PaintableKind::SVGImagePaintable:
    case Layout::RustFFI::PaintableKind::SVGMaskPaintable:
    case Layout::RustFFI::PaintableKind::SVGForeignObjectPaintable:
        return true;
    default:
        return false;
    }
}

static Layout::Box const* first_child_layout_box_of_kind(SVG::SVGGraphicsElement const& graphics_element, Layout::RustFFI::NodeKind kind)
{
    // NB: Called during painting.
    if (!graphics_element.unsafe_layout_node())
        return nullptr;
    for (auto child = graphics_element.unsafe_layout_node()->first_child(); child; child = child->next_sibling()) {
        if (child->kind() == kind)
            return static_cast<Layout::Box const*>(child.ptr());
    }
    return nullptr;
}

static Layout::Box const* get_mask_box(SVG::SVGGraphicsElement const& graphics_element)
{
    return first_child_layout_box_of_kind(graphics_element, Layout::RustFFI::NodeKind::SVGMaskBox);
}

static Layout::Box const* get_clip_box(SVG::SVGGraphicsElement const& graphics_element)
{
    return first_child_layout_box_of_kind(graphics_element, Layout::RustFFI::NodeKind::SVGClipBox);
}

// The object bounding box covers the target's geometry alone. The paintable's border box is not
// that: SVG layout inflates it by the visible stroke width. So, take the bounding box from the
// target's geometry path, which carries no stroke.
// AD-HOC: A group or a foreign object has no single geometry path, and we have no object bounding
//         box for it — so its border box still stands in there.
static CSSPixelRect target_user_space_object_bounding_box(Paintable const& target_paintable)
{
    if (auto const* committed_path = target_paintable.committed_svg_path())
        return committed_path->bounding_box().to_type<CSSPixels>();
    return target_paintable.absolute_border_box_rect();
}

// https://drafts.csswg.org/css-masking-1/#ClipPathElement
static bool contributes_to_clip_path(Paintable const& paintable)
{
    // If a child element is made invisible by display or visibility it does not contribute to the clipping path.
    return paintable.layout_node().visibility() == CSS::Visibility::Visible && !paintable.display().is_none();
}

// https://drafts.csswg.org/css-masking-1/#ClipPathElement
static Optional<CSSPixelRect> svg_clip_path_geometry_bounds(Paintable const& paintable, Gfx::AffineTransform const& additional_transform)
{
    if (!contributes_to_clip_path(paintable))
        return {};

    if (paintable.is_svg_path_paintable()) {
        auto const* committed_path = paintable.committed_svg_path();
        if (!committed_path)
            return {};
        auto path = committed_path->copy_transformed(additional_transform);
        return path.bounding_box().to_type<CSSPixels>();
    }

    // When the clipPath element contains multiple child elements, the silhouettes of the child elements are
    // logically OR'd together to create a single silhouette which is then used to restrict the region onto
    // which paint can be applied. Thus, a point is inside the clipping path if it is inside any of the
    // children of the clipPath.
    Gfx::BoundingBox<CSSPixels> bounding_box;
    paintable.for_each_child_of_type<Paintable>([&](Paintable const& child) {
        if (!child.is_svg_paintable())
            return IterationDecision::Continue;

        auto child_transform = Gfx::AffineTransform { additional_transform }.multiply(child.layout_node().used_svg_element_transform());
        auto child_bounds = svg_clip_path_geometry_bounds(child, child_transform);
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

static Gfx::AffineTransform object_bounding_box_content_units_transform(SVG::SVGGraphicsElement const& graphics_element)
{
    auto target_paintable = as<Layout::Box>(*graphics_element.unsafe_layout_node()).paintable_box();
    if (!target_paintable)
        return {};
    auto bounding_box = target_user_space_object_bounding_box(*target_paintable);
    return Gfx::AffineTransform {}
        .translate(bounding_box.location().to_type<float>())
        .scale({ bounding_box.width().to_float(), bounding_box.height().to_float() });
}

Optional<CSSPixelRect> Paintable::get_mask_area() const
{
    if (!kind_supports_svg_masking(kind()))
        return {};
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node());
    auto* mask_box = get_mask_box(graphics_element);
    if (!mask_box)
        return {};

    auto target_paintable = as<Layout::Box>(*graphics_element.unsafe_layout_node()).paintable_box();
    if (!target_paintable)
        return {};

    // Percentages in a userSpaceOnUse masking area resolve against the SVG viewport. The whole
    // computation stays in the target's user space: that is the coordinate space of the mask node
    // in the visual context tree.
    Gfx::FloatSize viewport_size {};
    if (auto const* viewport_paintable = nearest_svg_viewport_paintable_of(*mask_box))
        viewport_size = svg_viewport_user_rect(*viewport_paintable).size();

    auto target_object_bounding_box = target_user_space_object_bounding_box(*target_paintable);
    return as<SVG::SVGMaskElement>(*mask_box->dom_node()).resolve_masking_area(target_object_bounding_box, viewport_size, Gfx::AffineTransform {});
}

static Gfx::MaskKind mask_type_to_gfx_mask_kind(CSS::MaskType mask_type)
{
    switch (mask_type) {
    case CSS::MaskType::Alpha:
        return Gfx::MaskKind::Alpha;
    case CSS::MaskType::Luminance:
        return Gfx::MaskKind::Luminance;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<Gfx::MaskKind> Paintable::get_mask_type() const
{
    if (!kind_supports_svg_masking(kind()))
        return {};
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node());
    if (auto* mask_box = get_mask_box(graphics_element))
        return mask_type_to_gfx_mask_kind(mask_box->mask_type());
    return {};
}

Optional<CSSPixelRect> Paintable::get_clip_area() const
{
    if (!kind_supports_svg_masking(kind()))
        return {};
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node());
    auto const* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};

    auto const& clip_paintable = *clip_box->paintable_box();

    // The area must cover the same space calculate_svg_clip_display_list paints the content in.
    auto clip_path_transform = clip_paintable.layout_node().used_svg_element_transform();
    if (as<SVG::SVGClipPathElement>(*clip_box->dom_node()).clip_path_units() == SVG::SVGUnits::ObjectBoundingBox)
        clip_path_transform = object_bounding_box_content_units_transform(graphics_element).multiply(clip_path_transform);
    // An empty clipping path will completely clip away the element that had the clip-path property applied.
    return svg_clip_path_geometry_bounds(clip_paintable, clip_path_transform).value_or(CSSPixelRect {});
}

}
