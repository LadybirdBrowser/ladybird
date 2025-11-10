/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MathMLRadicalPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MathMLRadicalPaintable);

GC::Ref<MathMLRadicalPaintable> MathMLRadicalPaintable::create(Layout::MathMLRadicalBox const& layout_box)
{
    return layout_box.heap().allocate<MathMLRadicalPaintable>(layout_box);
}

MathMLRadicalPaintable::MathMLRadicalPaintable(Layout::MathMLRadicalBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::MathMLRadicalBox const& MathMLRadicalPaintable::layout_box() const
{
    return static_cast<Layout::MathMLRadicalBox const&>(layout_node());
}

void MathMLRadicalPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    // First paint the background and borders like a normal box
    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    // Draw the radical symbol (√)
    auto const& font = layout_box().first_available_font();
    auto line_thickness = max(1.5f, font.pixel_size() * 0.05f);

    auto content_rect = absolute_rect();
    auto color = computed_values().color();

    // Calculate radical symbol dimensions
    auto symbol_width = static_cast<int>(font.pixel_size()) / 2; // Width of the radical symbol
    auto hook_size = static_cast<int>(font.pixel_size()) / 7;    // Size of the bottom hook (approx 0.14)

    // Left edge where symbol starts
    auto x = content_rect.x() + 2;
    auto y_bottom = content_rect.y() + content_rect.height() - 4;
    auto y_top = content_rect.y() + 4;

    // Draw the radical symbol in three parts:

    // 1. Small hook at bottom left
    context.display_list_recorder().draw_line(
        Gfx::IntPoint(x.to_int(), y_bottom.to_int()),
        Gfx::IntPoint((x + hook_size).to_int(), (y_bottom - hook_size * 3 / 5).to_int()),
        color,
        line_thickness,
        Gfx::LineStyle::Solid);

    // 2. Diagonal line going up from hook
    context.display_list_recorder().draw_line(
        Gfx::IntPoint((x + hook_size).to_int(), (y_bottom - hook_size * 3 / 5).to_int()),
        Gfx::IntPoint((x + symbol_width).to_int(), y_top.to_int()),
        color,
        line_thickness,
        Gfx::LineStyle::Solid);

    // 3. Horizontal line (vinculum) over the content
    auto vinculum_start_x = x + symbol_width;
    auto vinculum_end_x = content_rect.x() + content_rect.width() - 2;

    context.display_list_recorder().draw_line(
        Gfx::IntPoint(vinculum_start_x.to_int(), y_top.to_int()),
        Gfx::IntPoint(vinculum_end_x.to_int(), y_top.to_int()),
        color,
        line_thickness,
        Gfx::LineStyle::Solid);
}

}
