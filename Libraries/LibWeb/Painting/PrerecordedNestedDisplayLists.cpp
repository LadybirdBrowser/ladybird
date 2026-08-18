/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/PrerecordedNestedDisplayLists.h>
#include <LibWeb/Painting/SVGPathPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::Painting {

static bool record_mask_entry(DisplayListRecordingContext& context, PrerecordedNestedDisplayLists& prerecorded, Paintable const& paintable, MaskLayerSet mask_layer_set)
{
    auto presence = paintable.mask_layer_presence(mask_layer_set);
    if (presence.is_empty())
        return false;

    bool any_svg_mask_layer_area_is_empty = false;
    Vector<PrerecordedMaskLayerDisplayList, 3> layers;
    for (auto const& mask_layer : presence) {
        switch (mask_layer.origin) {
        case MaskLayerOrigin::CssMaskLayers: {
            auto visual_context_tree = AccumulatedVisualContextTree::create();
            auto mask_display_list = DisplayList::create(visual_context_tree);
            DisplayListRecorder display_list_recorder(*mask_display_list, visual_context_tree, context.display_list_recorder().resource_storage());
            auto mask_painting_context = context.clone(display_list_recorder);
            auto mask_rect = CSSPixelRect { {}, mask_layer.area.size() };
            auto resolved_mask = resolve_mask_layers(paintable.layout_node().mask_layers(), paintable, mask_rect);

            // FIXME: Respect `image-rendering` here.
            paint_background(mask_painting_context, paintable, CSS::ImageRendering::Auto, resolved_mask, {});
            layers.append({ MaskLayerOrigin::CssMaskLayers, mask_layer.area.is_empty(), DisplayListResource { *mask_display_list, move(visual_context_tree) } });
            break;
        }
        case MaskLayerOrigin::SvgMask:
        case MaskLayerOrigin::SvgClip: {
            bool mask_layer_area_is_empty = mask_layer.area.is_empty();
            any_svg_mask_layer_area_is_empty |= mask_layer_area_is_empty;
            bool stacking_context_site_consumes_the_layer = mask_layer_set == MaskLayerSet::CssAndSvg && paintable.layout_node().establishes_stacking_context();
            if (mask_layer_area_is_empty && !stacking_context_site_consumes_the_layer) {
                layers.append({ mask_layer.origin, true, {} });
                continue;
            }
            auto mask_display_list = mask_layer.origin == MaskLayerOrigin::SvgMask
                ? paintable.calculate_mask(context, mask_layer.area)
                : paintable.calculate_clip(context, mask_layer.area);
            layers.append({ mask_layer.origin, mask_layer_area_is_empty, move(mask_display_list) });
            break;
        }
        }
    }
    prerecorded.mask_entries.set(&paintable, move(layers));
    return any_svg_mask_layer_area_is_empty;
}

static void record_pattern_paint_styles(DisplayListRecordingContext& context, PrerecordedNestedDisplayLists& prerecorded, Paintable const& paintable)
{
    auto const* path_paintable = as_if<SVGPathPaintable>(paintable);
    if (!path_paintable || !path_paintable->fill_and_stroke_paint_styles_are_resolved(context))
        return;
    auto const& graphics_element = path_paintable->dom_node();
    auto fill_pattern = graphics_element.fill_pattern();
    auto stroke_pattern = graphics_element.stroke_pattern();
    if (!fill_pattern && !stroke_pattern)
        return;

    auto paint_context = path_paintable->svg_paint_context(context);
    auto content_scale = context.display_list_recorder().visual_context_tree().accumulated_2d_scale(
        context.accumulated_visual_context_index_of(*path_paintable),
        ScrollStateSnapshot {},
        AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);

    for (auto const* pattern : { fill_pattern.ptr(), stroke_pattern.ptr() }) {
        if (!pattern)
            continue;
        auto pattern_paintable = pattern->resolve_pattern_paintable(path_paintable->layout_node());
        if (!pattern_paintable)
            continue;
        prerecorded.pattern_paint_styles.ensure(pattern_paintable.ptr(), [&] {
            return pattern->record_pattern_paint_style(paint_context, context, *pattern_paintable, content_scale);
        });
    }
}

static void prerecord_nested_display_lists_for_svg_subtree(DisplayListRecordingContext& context, Paintable const& root)
{
    auto* prerecorded = context.prerecorded_nested_display_lists();
    VERIFY(prerecorded);
    auto const& nested_assignments = context.nested_visual_context_assignments();
    VERIFY(nested_assignments.has_value());
    root.for_each_in_inclusive_subtree([&](Paintable const& paintable) {
        bool any_svg_mask_layer_area_is_empty = nested_assignments->mask_node_indices.contains(&paintable)
            && record_mask_entry(context, *prerecorded, paintable, MaskLayerSet::SvgOnly);
        if (any_svg_mask_layer_area_is_empty)
            return TraversalDecision::SkipChildrenAndContinue;
        record_pattern_paint_styles(context, *prerecorded, paintable);
        return TraversalDecision::Continue;
    });
}

DisplayListResource record_nested_svg_display_list(DisplayListRecordingContext& context, Paintable const& paintable, TransformData root_transform, IncludeRootElementTransform include_root_element_transform, bool draw_svg_geometry_for_clip_path)
{
    NestedVisualContextAssignments nested_visual_context_assignments;
    auto visual_context_tree = build_nested_svg_visual_context_tree(const_cast<Paintable&>(paintable), move(root_transform), nested_visual_context_assignments, include_root_element_transform);
    auto display_list = DisplayList::create(visual_context_tree);
    DisplayListRecorder display_list_recorder(*display_list, visual_context_tree, context.display_list_recorder().resource_storage());
    auto paint_context = context.clone(display_list_recorder);
    paint_context.set_nested_visual_context_assignments(move(nested_visual_context_assignments));
    paint_context.set_draw_svg_geometry_for_clip_path(draw_svg_geometry_for_clip_path);
    prerecord_nested_display_lists_for_svg_subtree(paint_context, paintable);
    StackingContext::paint_svg(paint_context, paintable, PaintPhase::Foreground);
    return DisplayListResource { *display_list, move(visual_context_tree) };
}

static bool recorded_mask_entry_has_empty_svg_mask_layer_area(PrerecordedNestedDisplayLists const& prerecorded, Paintable const& paintable)
{
    auto mask_layers = prerecorded.mask_entries.get(&paintable);
    if (!mask_layers.has_value())
        return false;
    for (auto const& mask_layer : *mask_layers) {
        if (mask_layer.origin != MaskLayerOrigin::CssMaskLayers && mask_layer.mask_layer_area_is_empty)
            return true;
    }
    return false;
}

static bool layout_node_is_inside_svg_resource_box(Layout::Node const& layout_node)
{
    for (auto const* ancestor = layout_node.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor->is_svg_pattern_box() || ancestor->is_svg_mask_box() || ancestor->is_svg_clip_box())
            return true;
    }
    return false;
}

void prerecord_nested_display_lists(DisplayListRecordingContext& context, ViewportPaintable& viewport_paintable)
{
    auto* prerecorded = context.prerecorded_nested_display_lists();
    VERIFY(prerecorded);

    for (auto const& weak_paintable : viewport_paintable.paintables_with_mask_nodes()) {
        auto paintable = weak_paintable.strong_ref();
        if (!paintable)
            continue;
        record_mask_entry(context, *prerecorded, *paintable, MaskLayerSet::CssAndSvg);
    }

    auto& document = viewport_paintable.document();
    auto& pattern_referencing_elements = document.svg_pattern_referencing_elements();
    pattern_referencing_elements.remove_all_matching([](auto const& weak_element) {
        return !weak_element;
    });
    for (auto const& weak_element : pattern_referencing_elements) {
        auto element = weak_element.ptr();
        if (!element || &element->document() != &document)
            continue;
        auto paintable = element->paintable();
        if (!paintable)
            continue;
        if (layout_node_is_inside_svg_resource_box(paintable->layout_node()))
            continue;
        if (recorded_mask_entry_has_empty_svg_mask_layer_area(*prerecorded, *paintable))
            continue;
        record_pattern_paint_styles(context, *prerecorded, *paintable);
    }
}

Optional<PaintStyle> prerecorded_pattern_paint_style(DisplayListRecordingContext& context, SVG::SVGPatternElement const& pattern, Layout::Node const& target_layout_node)
{
    auto pattern_paintable = pattern.resolve_pattern_paintable(target_layout_node);
    if (!pattern_paintable)
        return {};
    auto const* prerecorded = context.prerecorded_nested_display_lists();
    VERIFY(prerecorded);
    auto pattern_paint_style = prerecorded->pattern_paint_styles.get(pattern_paintable.ptr());
    VERIFY(pattern_paint_style.has_value());
    return *pattern_paint_style;
}

}
