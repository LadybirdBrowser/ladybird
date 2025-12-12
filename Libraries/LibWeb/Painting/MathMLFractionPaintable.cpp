/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLFractionPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLFractionPaintable);

GC::Ref<MathMLFractionPaintable> MathMLFractionPaintable::create(Layout::MathMLFractionBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLFractionPaintable>(layout_box);
}

MathMLFractionPaintable::MathMLFractionPaintable(Layout::MathMLFractionBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLFractionBox const& MathMLFractionPaintable::layout_box() const
{
    return static_cast<Layout::MathMLFractionBox const&>(layout_node());
}

void MathMLFractionPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // First paint the background and borders like a normal box
    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    // Draw the fraction bar
    // FIXME: This should be laid out between the numerator and denominator. For now it's centered vertically.

    auto const& font = layout_box().first_available_font();
    CSSPixels line_thickness { max(1.0f, font.pixel_size() * 0.066f) }; // Approx 1/15 of font size

    auto content_rect = absolute_rect();
    auto bar_y = content_rect.center().y();
    auto bar_x_start = content_rect.left() + 1;
    auto bar_x_end = content_rect.right() - 1;

    context.display_list_recorder().draw_line(
        context.rounded_device_point({ bar_x_start, bar_y }).to_type<int>(),
        context.rounded_device_point({ bar_x_end, bar_y }).to_type<int>(),
        computed_values().color(),
        static_cast<int>(context.rounded_device_pixels(line_thickness)),
        Gfx::LineStyle::Solid);
}

}
