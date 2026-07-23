/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Painting {

NonnullRefPtr<SVGSVGPaintable> SVGSVGPaintable::create(Layout::SVGSVGBox const& layout_box)
{
    return adopt_ref(*new SVGSVGPaintable(layout_box));
}

SVGSVGPaintable::SVGSVGPaintable(Layout::SVGSVGBox const& layout_box)
    : Paintable(layout_box)
{
}

static void record_foreign_object_descendant_hit_test_items(DisplayListRecordingContext& context, Paintable const& paintable)
{
    paintable.for_each_child_of_type<Paintable>([&](Paintable& child) {
        child.record_hit_test_items(context, PaintPhase::Background);
        record_foreign_object_descendant_hit_test_items(context, child);
        child.record_hit_test_items(context, PaintPhase::Foreground);
        child.record_hit_test_items(context, PaintPhase::Overlay);
        return IterationDecision::Continue;
    });
}

void SVGSVGPaintable::paint_svg_box(DisplayListRecordingContext& context, Paintable const& svg_box, PaintPhase phase)
{
    context.display_list_recorder().set_accumulated_visual_context(svg_box.accumulated_visual_context_index());

    // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
    // This ensures content-generating filters (feFlood, feImage) work even with empty source.
    if (auto const& bounds = svg_box.filter().svg_filter_bounds; bounds.has_value()) {
        auto device_rect = context.enclosing_device_rect(*bounds).to_type<int>();
        context.display_list_recorder().fill_rect_transparent(device_rect);
    }

    // Collect masks (SVG <mask>, SVG <clipPath>).
    Vector<MaskLayerDisplayList> masks;

    bool skip_painting = false;

    for (auto const& mask_layer : svg_box.mask_layer_presence(MaskLayerSet::SvgOnly)) {
        if (mask_layer.area.is_empty()) {
            skip_painting = true;
            continue;
        }
        auto mask_display_list = mask_layer.origin == MaskLayerOrigin::SvgMask
            ? svg_box.calculate_mask(context, mask_layer.area)
            : svg_box.calculate_clip(context, mask_layer.area);
        if (mask_display_list.has_value())
            masks.append({ mask_layer.origin, mask_display_list.release_value() });
    }

    register_mask_display_lists(context, svg_box, masks);

    if (!skip_painting) {
        svg_box.record_hit_test_items(context, phase);
        if (svg_box.layout_node().is_svg_foreign_object_box())
            record_foreign_object_descendant_hit_test_items(context, svg_box);
        if (!svg_box.is_svg_paintable()
            && !svg_box.is_svg_svg_paintable()
            && is<Layout::ReplacedBox>(svg_box.layout_node()))
            svg_box.paint(context, PaintPhase::Background);
        svg_box.paint(context, PaintPhase::Foreground);
        paint_descendants(context, svg_box, phase);
    }
}

void SVGSVGPaintable::paint_descendants(DisplayListRecordingContext& context, Paintable const& paintable, PaintPhase phase)
{
    if (phase != PaintPhase::Foreground)
        return;

    paintable.for_each_child_of_type<Paintable>([&](Paintable& child) {
        paint_svg_box(context, child, phase);
        return IterationDecision::Continue;
    });
}

}
