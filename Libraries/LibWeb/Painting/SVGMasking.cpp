/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/BoundingBox.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

static Layout::Box const* first_child_layout_box_of_kind(SVG::SVGGraphicsElement const& graphics_element, Layout::RustFFI::NodeKind kind)
{
    // NB: Called during painting.
    if (!graphics_element.unsafe_layout_node())
        return nullptr;
    for (auto const* child = graphics_element.unsafe_layout_node()->first_child(); child; child = child->next_sibling()) {
        if (child->kind() == kind)
            return static_cast<Layout::Box const*>(child);
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
static CSSPixelRect target_user_space_object_bounding_box(Layout::Node const& target)
{
    if (auto const* committed_path = Painting::committed_svg_path(target))
        return committed_path->bounding_box().to_type<CSSPixels>();
    return Painting::absolute_border_box_rect(target);
}

// https://drafts.csswg.org/css-masking-1/#ClipPathElement
static bool contributes_to_clip_path(Layout::Node const& node)
{
    // If a child element is made invisible by display or visibility it does not contribute to the clipping path.
    return as<Layout::NodeWithStyle>(node).visibility() == CSS::Visibility::Visible && !Painting::display(node).is_none();
}

// https://drafts.csswg.org/css-masking-1/#ClipPathElement
static Optional<CSSPixelRect> svg_clip_path_geometry_bounds(Layout::Node const& node, Gfx::AffineTransform const& additional_transform)
{
    if (!contributes_to_clip_path(node))
        return {};

    if (Painting::is_svg_path_paintable(node)) {
        auto const* committed_path = Painting::committed_svg_path(node);
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
    for (auto const* child = node.first_child_ptr(); child; child = child->next_sibling_ptr()) {
        switch (child->kind()) {
        case Layout::RustFFI::NodeKind::SVGMaskBox:
        case Layout::RustFFI::NodeKind::SVGClipBox:
        case Layout::RustFFI::NodeKind::SVGPatternBox:
            continue;
        default:
            break;
        }
        if (!Painting::has_committed_box(*child) || !Painting::is_svg_paintable(*child))
            continue;

        auto child_transform = Gfx::AffineTransform { additional_transform }.multiply(as<Layout::NodeWithStyle>(*child).used_svg_element_transform());
        auto child_bounds = svg_clip_path_geometry_bounds(*child, child_transform);
        if (!child_bounds.has_value())
            continue;

        bounding_box.add_point(child_bounds->left(), child_bounds->top());
        bounding_box.add_point(child_bounds->right(), child_bounds->bottom());
    }

    if (bounding_box.has_no_points())
        return {};
    return bounding_box.to_rect();
}

static Gfx::AffineTransform object_bounding_box_content_units_transform(SVG::SVGGraphicsElement const& graphics_element)
{
    auto const& target = as<Layout::Box>(*graphics_element.unsafe_layout_node());
    if (!Painting::has_committed_box(target))
        return {};
    auto bounding_box = target_user_space_object_bounding_box(target);
    return Gfx::AffineTransform {}
        .translate(bounding_box.location().to_type<float>())
        .scale({ bounding_box.width().to_float(), bounding_box.height().to_float() });
}

Optional<CSSPixelRect> mask_area(Layout::Node const& node)
{
    if (!Layout::RustFFI::layout_arena_paintable_supports_svg_masking(node.arena_handle(), committed_row_slot(node)))
        return {};
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*node.dom_node());
    auto* mask_box = get_mask_box(graphics_element);
    if (!mask_box)
        return {};

    auto const& target = as<Layout::Box>(*graphics_element.unsafe_layout_node());
    if (!Painting::has_committed_box(target))
        return {};

    // Percentages in a userSpaceOnUse masking area resolve against the SVG viewport. The whole
    // computation stays in the target's user space: that is the coordinate space of the mask node
    // in the visual context tree.
    Gfx::FloatSize viewport_size {};
    auto viewport_rect = Layout::RustFFI::layout_arena_paintable_svg_viewport_user_rect(mask_box->arena_handle(), committed_row_slot(*mask_box));
    if (viewport_rect.has_value())
        viewport_size = viewport_rect.value().size().to_type<float>();

    auto target_object_bounding_box = target_user_space_object_bounding_box(target);
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

Optional<Gfx::MaskKind> mask_type(Layout::Node const& node)
{
    if (!Layout::RustFFI::layout_arena_paintable_supports_svg_masking(node.arena_handle(), committed_row_slot(node)))
        return {};
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*node.dom_node());
    if (auto* mask_box = get_mask_box(graphics_element))
        return mask_type_to_gfx_mask_kind(mask_box->mask_type());
    return {};
}

Optional<CSSPixelRect> clip_area(Layout::Node const& node)
{
    if (!Layout::RustFFI::layout_arena_paintable_supports_svg_masking(node.arena_handle(), committed_row_slot(node)))
        return {};
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*node.dom_node());
    auto const* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};

    if (!Painting::has_committed_box(*clip_box))
        return {};

    // The area must cover the same space calculate_svg_clip_display_list paints the content in.
    auto clip_path_transform = clip_box->used_svg_element_transform();
    if (as<SVG::SVGClipPathElement>(*clip_box->dom_node()).clip_path_units() == SVG::SVGUnits::ObjectBoundingBox)
        clip_path_transform = object_bounding_box_content_units_transform(graphics_element).multiply(clip_path_transform);
    // An empty clipping path will completely clip away the element that had the clip-path property applied.
    return svg_clip_path_geometry_bounds(*clip_box, clip_path_transform).value_or(CSSPixelRect {});
}

}
