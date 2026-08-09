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

Optional<CSSPixelRect> SVGMaskable::get_svg_mask_area() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* mask_box = get_mask_box(graphics_element);
    if (!mask_box)
        return {};

    auto target_paintable = as<Layout::Box>(*graphics_element.unsafe_layout_node()).paintable_box();
    if (!target_paintable)
        return {};

    // Percentages in a userSpaceOnUse masking area resolve against the SVG viewport — and the resulting user-space
    // rectangle maps to CSS pixels relative to the containing svg box.
    Gfx::FloatSize viewport_size {};
    auto user_space_to_css_pixels = Gfx::AffineTransform {};
    if (auto const* svg_box = mask_box->first_ancestor_of_type<Layout::SVGSVGBox>(); svg_box && svg_box->paintable_box()) {
        viewport_size = svg_box->view_box_or_viewport_rect().size();
        user_space_to_css_pixels.translate(svg_box->paintable_box()->absolute_position().to_type<float>());
    }
    user_space_to_css_pixels.multiply(target_svg_to_css_pixels_transform());

    // objectBoundingBox units resolve against the target's object bounding box — which covers its geometry alone. The
    // paintable's border box is not that: SVG layout inflates it by the visible stroke width. So, take the bounding box
    // from the target's geometry path, which carries no stroke, and map it into CSS pixels, the same way painting does.
    // AD-HOC: A group or a foreign object has no single geometry path, and we have no object bounding box for it — so
    //         its border box still stands in there.
    auto target_object_bounding_box = target_paintable->absolute_border_box_rect();
    if (auto const* path_paintable = as_if<SVGPathPaintable>(*target_paintable); path_paintable && path_paintable->computed_path().has_value())
        target_object_bounding_box = user_space_to_css_pixels.map(path_paintable->computed_path()->bounding_box()).to_type<CSSPixels>();

    return mask_box->dom_node().resolve_masking_area(target_object_bounding_box, viewport_size, user_space_to_css_pixels);
}

Optional<CSSPixelRect> SVGMaskable::get_svg_clip_area() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto const* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};

    auto const& clip_paintable = as<SVGPaintable>(*clip_box->paintable_box());

    auto clip_path_transform = Gfx::AffineTransform { target_svg_transform() }.multiply(clip_box->dom_node().element_transform());
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

static void build_nested_svg_visual_context_tree_for_subtree(AccumulatedVisualContextTree& visual_context_tree, NestedMaskNodeAssignments& mask_node_assignments, DevicePixelConverter const& converter, Paintable& paintable_box, VisualContextIndex inherited_state, float pixel_ratio)
{
    auto const& style_source = paintable_box.layout_node();
    if (style_source.filter().has_filters())
        paintable_box.set_filter(resolve_css_filter(style_source.filter(), paintable_box));
    else
        paintable_box.set_filter({});

    auto gfx_filter = to_gfx_filter(paintable_box.filter(), pixel_ratio);
    EffectsData effects {
        style_source.opacity(),
        mix_blend_mode_to_compositing_and_blending_operator(style_source.mix_blend_mode()),
        move(gfx_filter)
    };

    auto own_state = inherited_state;
    if (effects.needs_layer())
        own_state = visual_context_tree.append(move(effects), inherited_state);

    for (auto const& mask_layer : paintable_box.mask_layer_presence(MaskLayerSet::SvgOnly)) {
        own_state = visual_context_tree.append(MaskData { converter.enclosing_device_rect(mask_layer.area), mask_layer.kind, mask_layer.origin }, own_state);
        mask_node_assignments.ensure(&paintable_box).append(own_state);
    }

    paintable_box.set_accumulated_visual_context(own_state);
    paintable_box.set_accumulated_visual_context_for_descendants(own_state);

    paintable_box.for_each_child_of_type<Paintable>([&](Paintable& child) {
        build_nested_svg_visual_context_tree_for_subtree(visual_context_tree, mask_node_assignments, converter, child, own_state, pixel_ratio);
        return IterationDecision::Continue;
    });
}

static AccumulatedVisualContextTree build_nested_svg_visual_context_tree(Paintable& root_paintable, Gfx::IntPoint content_offset, NestedMaskNodeAssignments& mask_node_assignments)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create_with_content_offset(content_offset);
    auto pixel_ratio = root_paintable.document().page().client().device_pixels_per_css_pixel();
    DevicePixelConverter converter(pixel_ratio);
    build_nested_svg_visual_context_tree_for_subtree(visual_context_tree, mask_node_assignments, converter, root_paintable, {}, pixel_ratio);
    return visual_context_tree;
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
    Gfx::AffineTransform const& target_svg_transform,
    Paintable const& paintable,
    CSSPixelRect const& area,
    bool is_clip_path)
{
    auto mask_rect = context.enclosing_device_rect(area);
    NestedMaskNodeAssignments mask_node_assignments;
    auto visual_context_tree = build_nested_svg_visual_context_tree(const_cast<Paintable&>(paintable), -mask_rect.location().to_type<int>(), mask_node_assignments);
    auto display_list = DisplayList::create(visual_context_tree);
    DisplayListRecorder display_list_recorder(*display_list, visual_context_tree, context.display_list_recorder().resource_storage());
    auto paint_context = context.clone(display_list_recorder);
    paint_context.set_nested_mask_node_assignments(move(mask_node_assignments));
    auto const& mask_element = as<SVG::SVGGraphicsElement const>(*paintable.dom_node());
    // Layout computes transforms only within the mask/clip subtree, so prepend the target's accumulated transform here.
    auto svg_transform = Gfx::AffineTransform { target_svg_transform }.multiply(mask_element.element_transform());
    paint_context.set_svg_transform(svg_transform);
    paint_context.set_draw_svg_geometry_for_clip_path(is_clip_path);
    StackingContext::paint_svg(paint_context, paintable, PaintPhase::Foreground);
    return DisplayListResource { *display_list, move(visual_context_tree) };
}

Gfx::AffineTransform SVGMaskable::target_svg_transform() const
{
    // Only SVGGraphicsPaintable carries an SVG transform; other targets (e.g. foreign objects) use identity.
    if (auto const* svg_graphics_paintable = as_if<SVGGraphicsPaintable>(*this))
        return svg_graphics_paintable->computed_transforms().svg_transform();
    return {};
}

Gfx::AffineTransform SVGMaskable::target_svg_to_css_pixels_transform() const
{
    if (auto const* svg_graphics_paintable = as_if<SVGGraphicsPaintable>(*this))
        return svg_graphics_paintable->computed_transforms().svg_to_css_pixels_transform();
    // The contents of a mask applied to a foreign object are painted without the foreign object's SVG transform (see
    // target_svg_transform above) — so the masking area likewise maps through the viewbox transform alone.
    if (auto const* foreign_object_paintable = as_if<SVGForeignObjectPaintable>(*this))
        return foreign_object_paintable->computed_transforms().svg_to_viewbox_transform();
    return {};
}

Optional<DisplayListResource> SVGMaskable::calculate_svg_mask_display_list(DisplayListRecordingContext& context, CSSPixelRect const& mask_area) const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* mask_box = get_mask_box(graphics_element);
    if (!mask_box)
        return {};
    auto& mask_paintable = static_cast<Paintable const&>(*mask_box->paintable());
    return paint_mask_or_clip_to_display_list(context, target_svg_transform(), mask_paintable, mask_area, false);
}

Optional<DisplayListResource> SVGMaskable::calculate_svg_clip_display_list(DisplayListRecordingContext& context, CSSPixelRect const& clip_area) const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};
    auto& clip_paintable = static_cast<Paintable const&>(*clip_box->paintable());
    return paint_mask_or_clip_to_display_list(context, target_svg_transform(), clip_paintable, clip_area, true);
}

}
