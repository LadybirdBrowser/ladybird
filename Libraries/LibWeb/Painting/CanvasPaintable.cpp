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

Layout::CanvasBox const& CanvasPaintable::layout_box() const
{
    return static_cast<Layout::CanvasBox const&>(layout_node());
}

void CanvasPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto canvas_rect = context.rounded_device_rect(absolute_rect());
        ScopedCornerRadiusClip corner_clip { context, canvas_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

        if (layout_box().dom_node().surface()) {
            auto surface = layout_box().dom_node().surface();

            // FIXME: Remove this const_cast.
            const_cast<HTML::HTMLCanvasElement&>(layout_box().dom_node()).present();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), surface->rect(), canvas_rect.to_type<int>());
            context.display_list_recorder().draw_painting_surface(canvas_rect.to_type<int>(), *layout_box().dom_node().surface(), surface->rect(), scaling_mode);
        }
    }
}

}
