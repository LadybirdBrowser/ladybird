/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ResolvedCSSFilter.h>
#include <LibWeb/Painting/SVGClipPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/SVGPaintable.h>
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
    if (auto* mask_box = get_mask_box(graphics_element))
        return mask_box->dom_node().resolve_masking_area(mask_box->paintable_box()->absolute_border_box_rect());
    return {};
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

static void build_nested_svg_visual_context_tree_for_subtree(AccumulatedVisualContextTree& visual_context_tree, PaintableBox& paintable_box, VisualContextIndex inherited_state, float pixel_ratio)
{
    auto const& computed_values = paintable_box.computed_values();
    if (computed_values.filter().has_filters())
        paintable_box.set_filter(resolve_css_filter(computed_values.filter(), paintable_box));
    else
        paintable_box.set_filter({});

    auto gfx_filter = to_gfx_filter(paintable_box.filter(), pixel_ratio);
    EffectsData effects {
        computed_values.opacity(),
        mix_blend_mode_to_compositing_and_blending_operator(computed_values.mix_blend_mode()),
        move(gfx_filter)
    };

    auto own_state = inherited_state;
    if (effects.needs_layer())
        own_state = visual_context_tree.append(move(effects), inherited_state);

    paintable_box.set_accumulated_visual_context(own_state);
    paintable_box.set_accumulated_visual_context_for_descendants(own_state);

    paintable_box.for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
        build_nested_svg_visual_context_tree_for_subtree(visual_context_tree, child, own_state, pixel_ratio);
        return IterationDecision::Continue;
    });
}

static AccumulatedVisualContextTree build_nested_svg_visual_context_tree(PaintableBox& root_paintable)
{
    auto visual_context_tree = AccumulatedVisualContextTree::create();
    auto pixel_ratio = root_paintable.document().page().client().device_pixels_per_css_pixel();
    build_nested_svg_visual_context_tree_for_subtree(visual_context_tree, root_paintable, {}, pixel_ratio);

    return visual_context_tree;
}

Optional<Gfx::MaskKind> SVGMaskable::get_svg_mask_type() const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    if (auto* mask_box = get_mask_box(graphics_element))
        return mask_type_to_gfx_mask_kind(mask_box->computed_values().mask_type());
    return {};
}

static Optional<DisplayListResource> paint_mask_or_clip_to_display_list(
    DisplayListRecordingContext& context,
    Gfx::AffineTransform const& target_svg_transform,
    PaintableBox const& paintable,
    CSSPixelRect const& area,
    bool is_clip_path)
{
    auto mask_rect = context.enclosing_device_rect(area);
    auto visual_context_tree = build_nested_svg_visual_context_tree(const_cast<PaintableBox&>(paintable));
    auto display_list = DisplayList::create(visual_context_tree);
    DisplayListRecorder display_list_recorder(*display_list, visual_context_tree, context.display_list_recorder().resource_storage());
    display_list_recorder.translate(-mask_rect.location().to_type<int>());
    auto paint_context = context.clone(display_list_recorder);
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

Optional<DisplayListResource> SVGMaskable::calculate_svg_mask_display_list(DisplayListRecordingContext& context, CSSPixelRect const& mask_area) const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* mask_box = get_mask_box(graphics_element);
    if (!mask_box)
        return {};
    auto& mask_paintable = static_cast<PaintableBox const&>(*mask_box->first_paintable());
    return paint_mask_or_clip_to_display_list(context, target_svg_transform(), mask_paintable, mask_area, false);
}

Optional<DisplayListResource> SVGMaskable::calculate_svg_clip_display_list(DisplayListRecordingContext& context, CSSPixelRect const& clip_area) const
{
    auto const& graphics_element = as<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto* clip_box = get_clip_box(graphics_element);
    if (!clip_box)
        return {};
    auto& clip_paintable = static_cast<PaintableBox const&>(*clip_box->first_paintable());
    return paint_mask_or_clip_to_display_list(context, target_svg_transform(), clip_paintable, clip_area, true);
}

}
