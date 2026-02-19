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
        if (canvas_element.surface()) {
            // present() snapshots the surface and publishes to ExternalContentSource.
            // FIXME: Remove this const_cast.
            auto& mutable_canvas_element = const_cast<HTML::HTMLCanvasElement&>(canvas_element);
            mutable_canvas_element.present();
            auto canvas_int_rect = canvas_rect.to_type<int>();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(),
                canvas_element.surface()->size(), canvas_int_rect.size());
            context.display_list_recorder().draw_external_content(canvas_int_rect,
                mutable_canvas_element.ensure_external_content_source(), scaling_mode);
        }
    }
}

}
