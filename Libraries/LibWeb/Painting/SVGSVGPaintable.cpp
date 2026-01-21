/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(SVGSVGPaintable);

GC::Ref<SVGSVGPaintable> SVGSVGPaintable::create(Layout::SVGSVGBox const& layout_box)
{
    return layout_box.heap().allocate<SVGSVGPaintable>(layout_box);
}

SVGSVGPaintable::SVGSVGPaintable(Layout::SVGSVGBox const& layout_box)
    : PaintableBox(layout_box)
{
}

void SVGSVGPaintable::paint_svg_box(DisplayListRecordingContext& context, PaintableBox const& svg_box, PaintPhase phase)
{
    context.display_list_recorder().set_accumulated_visual_context(svg_box.accumulated_visual_context());

    // For elements with SVG filters, emit a transparent FillRect to trigger filter application.
    // This ensures content-generating filters (feFlood, feImage) work even with empty source.
    if (auto const& bounds = svg_box.filter().svg_filter_bounds; bounds.has_value()) {
        auto device_rect = context.enclosing_device_rect(*bounds).to_type<int>();
        context.display_list_recorder().fill_rect_transparent(device_rect);
    }

    auto masking_area = svg_box.get_masking_area();
    if (masking_area.has_value()) {
        context.display_list_recorder().save();

        bool skip_painting = false;
        if (masking_area->is_empty()) {
            skip_painting = true;
        } else {
            auto mask_bitmap = svg_box.calculate_mask(context, *masking_area);
            if (mask_bitmap) {
                auto source_paintable_rect = context.enclosing_device_rect(*masking_area).template to_type<int>();
                context.display_list_recorder().apply_mask_bitmap(source_paintable_rect.location(), mask_bitmap.release_nonnull(), *svg_box.get_mask_type());
            }
        }

        if (!skip_painting) {
            svg_box.paint(context, PaintPhase::Foreground);
            paint_descendants(context, svg_box, phase);
        }

        context.display_list_recorder().restore();
    } else {
        svg_box.paint(context, PaintPhase::Foreground);
        paint_descendants(context, svg_box, phase);
    }
}

void SVGSVGPaintable::paint_descendants(DisplayListRecordingContext& context, PaintableBox const& paintable, PaintPhase phase)
{
    if (phase != PaintPhase::Foreground)
        return;

    paintable.for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
        paint_svg_box(context, child, phase);
        return IterationDecision::Continue;
    });
}

}
