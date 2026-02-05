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

    auto mask_area = svg_box.get_mask_area();
    auto clip_area = svg_box.get_clip_area();
    auto needs_to_save_state = mask_area.has_value() || clip_area.has_value();

    if (needs_to_save_state) {
        context.display_list_recorder().save();
    }

    bool skip_painting = false;

    // Apply <mask> if present
    if (mask_area.has_value()) {
        if (mask_area->is_empty()) {
            skip_painting = true;
        } else if (auto mask_display_list = svg_box.calculate_mask(context, *mask_area)) {
            auto rect = context.enclosing_device_rect(*mask_area).to_type<int>();
            auto kind = svg_box.get_mask_type().value_or(Gfx::MaskKind::Alpha);
            context.display_list_recorder().add_mask(mask_display_list, rect, kind);
        }
    }

    // Apply <clipPath> if present
    if (clip_area.has_value()) {
        if (clip_area->is_empty()) {
            skip_painting = true;
        } else if (auto clip_display_list = svg_box.calculate_clip(context, *clip_area)) {
            auto rect = context.enclosing_device_rect(*clip_area).to_type<int>();
            context.display_list_recorder().add_mask(clip_display_list, rect, Gfx::MaskKind::Alpha);
        }
    }

    if (!skip_painting) {
        svg_box.paint(context, PaintPhase::Foreground);
        paint_descendants(context, svg_box, phase);
    }

    if (needs_to_save_state) {
        context.display_list_recorder().restore();
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
