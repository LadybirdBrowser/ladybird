/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
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

    auto const& font = layout_box().first_available_font();
    CSSPixels line_thickness { max(1.5f, font.pixel_size() * 0.05f) };

    auto content_rect = absolute_rect();
    auto color = computed_values().color();

    CSSPixels symbol_width { font.pixel_size() / 2 };
    CSSPixels hook_size { font.pixel_size() / 7 };

    auto left = content_rect.left() + 2;
    auto y_bottom = content_rect.bottom() - line_thickness;
    auto y_top = content_rect.y() + 4;

    Gfx::Path path;
    auto device_point = [&context](CSSPixels x, CSSPixels y) {
        auto position = context.rounded_device_point({ x, y });
        return position.to_type<DevicePixels::Type>().to_type<float>();
    };

    // 1. Small hook at bottom left
    path.move_to(device_point(left, y_bottom - hook_size));
    path.line_to(device_point(left + hook_size, y_bottom));

    // 2. Diagonal line going up from hook
    path.line_to(device_point(left + symbol_width, y_top));

    // 3. Horizontal line (vinculum) over the content
    path.line_to(device_point(content_rect.right() - 2, y_top));

    context.display_list_recorder().stroke_path({
        .cap_style = Gfx::Path::CapStyle::Square,
        .join_style = Gfx::Path::JoinStyle::Miter,
        .miter_limit = 4,
        .dash_array = {},
        .dash_offset = 0,
        .path = path,
        .paint_style_or_color = color,
        .thickness = static_cast<float>(line_thickness * context.device_pixels_per_css_pixel()),
    });
}

}
