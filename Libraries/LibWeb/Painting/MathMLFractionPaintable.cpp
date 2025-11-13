/*
 * Copyright (c) 2025, the Ladybird Browser contributors
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
    // The bar should be drawn between the numerator and denominator
    // with appropriate spacing based on font metrics

    auto const& font = layout_box().first_available_font();
    auto line_thickness = max(1.0f, font.pixel_size() * 0.066f); // Approx 1/15 of font size

    // Get the content rectangle
    auto content_rect = absolute_rect();

    // Calculate the position for the fraction bar
    // It should be centered vertically within the content area
    auto bar_y = content_rect.y() + content_rect.height() / 2;
    auto bar_x_start = content_rect.x() + 1; // Small padding
    auto bar_x_end = content_rect.x() + content_rect.width() - 1;

    // Draw the fraction bar
    context.display_list_recorder().draw_line(
        Gfx::IntPoint(bar_x_start.to_int(), bar_y.to_int()),
        Gfx::IntPoint(bar_x_end.to_int(), bar_y.to_int()),
        computed_values().color(),
        line_thickness,
        Gfx::LineStyle::Solid);
}

}
