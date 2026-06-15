/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/CanvasPaintable.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::Painting {

NonnullRefPtr<CanvasPaintable> CanvasPaintable::create(Layout::CanvasBox const& layout_box)
{
    return adopt_ref(*new CanvasPaintable(layout_box));
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
        if (auto content_size = canvas_element.canvas_surface_content_size(); content_size.has_value()) {
            auto canvas_id = canvas_element.canvas_id();
            VERIFY(canvas_id.has_value());
            auto canvas_int_rect = canvas_rect.to_type<int>();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(),
                *content_size, canvas_int_rect.size());
            context.display_list_recorder().draw_canvas(canvas_int_rect,
                *canvas_id, scaling_mode);
        }
    }
}

}
