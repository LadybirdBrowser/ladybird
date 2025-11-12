/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/CanvasPaintable.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(CanvasPaintable);

GC::Ref<CanvasPaintable> CanvasPaintable::create(Layout::CanvasBox const& layout_box)
{
    return layout_box.heap().allocate<CanvasPaintable>(layout_box);
}

CanvasPaintable::CanvasPaintable(Layout::CanvasBox const& layout_box)
    : PaintableBox(layout_box)
{
}

void CanvasPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto canvas_rect = context.rounded_device_rect(absolute_rect());
        ScopedCornerRadiusClip corner_clip { context, canvas_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

        auto& canvas_element = as<HTML::HTMLCanvasElement>(*dom_node());
        if (auto surface = canvas_element.surface()) {
            // FIXME: Remove this const_cast.
            const_cast<HTML::HTMLCanvasElement&>(canvas_element).present();
            auto canvas_int_rect = canvas_rect.to_type<int>();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), surface->size(), canvas_int_rect.size());
            context.display_list_recorder().draw_painting_surface(canvas_int_rect, *surface, surface->rect(), scaling_mode);
        }
    }
}

}
