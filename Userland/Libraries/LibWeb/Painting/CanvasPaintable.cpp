/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/CanvasPaintable.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(CanvasPaintable);

JS::NonnullGCPtr<CanvasPaintable> CanvasPaintable::create(Layout::CanvasBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<CanvasPaintable>(layout_box);
}

CanvasPaintable::CanvasPaintable(Layout::CanvasBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::CanvasBox const& CanvasPaintable::layout_box() const
{
    return static_cast<Layout::CanvasBox const&>(layout_node());
}

void CanvasPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto canvas_rect = context.rounded_device_rect(absolute_rect());
        ScopedCornerRadiusClip corner_clip { context, canvas_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

        if (layout_box().dom_node().bitmap()) {
            // FIXME: Remove this const_cast.
            const_cast<HTML::HTMLCanvasElement&>(layout_box().dom_node()).present();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), layout_box().dom_node().bitmap()->rect(), canvas_rect.to_type<int>());
            context.display_list_recorder().draw_scaled_bitmap(canvas_rect.to_type<int>(), *layout_box().dom_node().bitmap(), layout_box().dom_node().bitmap()->rect(), scaling_mode);
        }
    }
}

}
