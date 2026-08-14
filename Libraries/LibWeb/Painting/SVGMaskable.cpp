/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/SVGClipPaintable.h>
#include <LibWeb/Painting/SVGForeignObjectPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/SVGPaintable.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

template<typename T>
static T const* first_child_layout_node_of_type(SVG::SVGGraphicsElement const& graphics_element)
{
    // NB: Called during painting.
    if (!graphics_element.unsafe_layout_node())
        return nullptr;
    return graphics_element.unsafe_layout_node()->first_child_of_type<T>();
}

static auto get_mask_box(SVG::SVGGraphicsElement const& graphics_element)
{
    return first_child_layout_node_of_type<Layout::SVGMaskBox>(graphics_element);
}

static auto get_clip_box(SVG::SVGGraphicsElement const& graphics_element)
{
    return first_child_layout_node_of_type<Layout::SVGClipBox>(graphics_element);
}

// The object bounding box covers the target's geometry alone. The paintable's border box is not
// that: SVG layout inflates it by the visible stroke width. So, take the bounding box from the
// target's geometry path, which carries no stroke.
// AD-HOC: A group or a foreign object has no single geometry path, and we have no object bounding
//         box for it — so its border box still stands in there.
static CSSPixelRect target_user_space_object_bounding_box(Paintable const& target_paintable)
{
    if (auto const* path_paintable = as_if<SVGPathPaintable>(target_paintable); path_paintable && path_paintable->computed_path().has_value())
        return path_paintable->computed_path()->bounding_box().to_type<CSSPixels>();
    return target_paintable.absolute_border_box_rect();
}

Optional<CSSPixelRect> SVGMaskable::get_svg_mask_area() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
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
    return mask_box->dom_node().resolve_masking_area(target_object_bounding_box, viewport_size, Gfx::AffineTransform {});
}

Optional<CSSPixelRect> SVGMaskable::get_svg_clip_area() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto const* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};

    auto const& clip_paintable = as<SVGPaintable>(*clip_box->paintable_box());

    // The area must cover the same space calculate_svg_clip_display_list paints the content in.
    auto clip_path_transform = clip_paintable.layout_node().used_svg_element_transform();
    if (clip_box->dom_node().clip_path_units() == SVG::SVGUnits::ObjectBoundingBox)
        clip_path_transform = object_bounding_box_content_units_transform().multiply(clip_path_transform);
    // An empty clipping path will completely clip away the element that had the clip-path property applied.
    return clip_paintable.clip_path_geometry_bounds(clip_path_transform).value_or(CSSPixelRect {});
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

Optional<Gfx::MaskKind> SVGMaskable::get_svg_mask_type() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    if (auto* mask_box = get_mask_box(graphics_element))
        return mask_type_to_gfx_mask_kind(mask_box->mask_type());
    return {};
}

static Optional<DisplayListResource> paint_mask_or_clip_to_display_list(
    DisplayListRecordingContext& context,
    Gfx::AffineTransform const& content_units_transform,
    Paintable const& paintable,
    CSSPixelRect const& area,
    bool is_clip_path)
{
    auto mask_rect = context.enclosing_device_rect(area);
    auto device_scale = static_cast<float>(context.device_pixels_per_css_pixel());
    // Content records in the resource's own units scaled by the device pixel ratio; the root maps
    // it through the content-units transform into the target's user space and rebases onto the
    // mask surface origin.
    auto content_units_transform_in_recorded_space = content_units_transform;
    content_units_transform_in_recorded_space.set_translation({ content_units_transform.translation().x() * device_scale, content_units_transform.translation().y() * device_scale });
    auto surface_origin = mask_rect.location().to_type<int>().to_type<float>();
    auto recorded_to_surface = Gfx::AffineTransform {}
                                   .translate(-surface_origin)
                                   .multiply(content_units_transform_in_recorded_space);
    TransformData root_transform { recorded_to_surface.to_matrix(), {} };
    NestedVisualContextAssignments nested_visual_context_assignments;
    auto visual_context_tree = build_nested_svg_visual_context_tree(const_cast<Paintable&>(paintable), move(root_transform), nested_visual_context_assignments);
    auto display_list = DisplayList::create(visual_context_tree);
    DisplayListRecorder display_list_recorder(*display_list, visual_context_tree, context.display_list_recorder().resource_storage());
    auto paint_context = context.clone(display_list_recorder);
    paint_context.set_nested_visual_context_assignments(move(nested_visual_context_assignments));
    paint_context.set_draw_svg_geometry_for_clip_path(is_clip_path);
    StackingContext::paint_svg(paint_context, paintable, PaintPhase::Foreground);
    return DisplayListResource { *display_list, move(visual_context_tree) };
}

Gfx::AffineTransform SVGMaskable::object_bounding_box_content_units_transform() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto target_paintable = as<Layout::Box>(*graphics_element.unsafe_layout_node()).paintable_box();
    if (!target_paintable)
        return {};
    auto bounding_box = target_user_space_object_bounding_box(*target_paintable);
    return Gfx::AffineTransform {}
        .translate(bounding_box.location().to_type<float>())
        .scale({ bounding_box.width().to_float(), bounding_box.height().to_float() });
}

Optional<DisplayListResource> SVGMaskable::calculate_svg_mask_display_list(DisplayListRecordingContext& context, CSSPixelRect const& mask_area) const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* mask_box = get_mask_box(graphics_element);
    if (!mask_box)
        return {};
    auto& mask_paintable = static_cast<Paintable const&>(*mask_box->paintable());
    auto content_units_transform = Gfx::AffineTransform {};
    if (mask_box->dom_node().mask_content_units() == SVG::SVGUnits::ObjectBoundingBox)
        content_units_transform = object_bounding_box_content_units_transform();
    return paint_mask_or_clip_to_display_list(context, content_units_transform, mask_paintable, mask_area, false);
}

Optional<DisplayListResource> SVGMaskable::calculate_svg_clip_display_list(DisplayListRecordingContext& context, CSSPixelRect const& clip_area) const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};
    auto& clip_paintable = static_cast<Paintable const&>(*clip_box->paintable());
    auto content_units_transform = Gfx::AffineTransform {};
    if (clip_box->dom_node().clip_path_units() == SVG::SVGUnits::ObjectBoundingBox)
        content_units_transform = object_bounding_box_content_units_transform();
    return paint_mask_or_clip_to_display_list(context, content_units_transform, clip_paintable, clip_area, true);
}

}
