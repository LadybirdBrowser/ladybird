/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/CheckBox.h>
#include <LibWeb/Layout/Label.h>
#include <LibWeb/Painting/CheckBoxPaintable.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/InputColors.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(CheckBoxPaintable);

static Gfx::Path check_mark_path(Gfx::IntRect checkbox_rect)
{
    Gfx::Path path;
    path.move_to({ 72, 14 });
    path.line_to({ 37, 64 });
    path.line_to({ 19, 47 });
    path.line_to({ 8, 58 });
    path.line_to({ 40, 89 });
    path.line_to({ 85, 24 });
    path.close();

    float const checkmark_width = 100;
    float const checkmark_height = 100;
    Gfx::AffineTransform scale_checkmark_to_fit;
    scale_checkmark_to_fit.scale(checkbox_rect.width() / checkmark_width, checkbox_rect.height() / checkmark_height);
    return path.copy_transformed(scale_checkmark_to_fit);
}

GC::Ref<CheckBoxPaintable>
CheckBoxPaintable::create(Layout::CheckBox const& layout_box)
{
    return layout_box.heap().allocate<CheckBoxPaintable>(layout_box);
}

CheckBoxPaintable::CheckBoxPaintable(Layout::CheckBox const& layout_box)
    : LabelablePaintable(layout_box)
{
}

Layout::CheckBox const& CheckBoxPaintable::layout_box() const
{
    return static_cast<Layout::CheckBox const&>(layout_node());
}

Layout::CheckBox& CheckBoxPaintable::layout_box()
{
    return static_cast<Layout::CheckBox&>(layout_node());
}

void CheckBoxPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto const& checkbox = static_cast<HTML::HTMLInputElement const&>(layout_box().dom_node());
    bool enabled = layout_box().dom_node().enabled();
    auto checkbox_rect = context.enclosing_device_rect(absolute_rect()).to_type<int>();
    auto checkbox_radius = checkbox_rect.width() / 5;

    auto shade = [&](Color color, float amount) {
        return InputColors::get_shade(color, amount, computed_values().color_scheme());
    };

    auto modify_color = [&](Color color) {
        if (being_pressed() && enabled)
            return shade(color, 0.3f);
        return color;
    };

    auto input_colors = compute_input_colors(computed_values().color_scheme(), computed_values().accent_color());

    auto increase_contrast = [&](Color color, Color background) {
        auto constexpr min_contrast = 2;
        if (color.contrast_ratio(background) < min_contrast) {
            color = color.inverted();
            if (color.contrast_ratio(background) > min_contrast)
                return color;
        }
        return color;
    };

    // Little heuristic that smaller things look better with more smoothness.
    if (checkbox.checked() && !checkbox.indeterminate()) {
        auto background_color = enabled ? input_colors.accent : input_colors.mid_gray;
        context.display_list_recorder().fill_rect_with_rounded_corners(checkbox_rect, modify_color(background_color), checkbox_radius);
        auto tick_color = increase_contrast(input_colors.base, background_color);
        if (!enabled)
            tick_color = shade(tick_color, 0.5f);
        context.display_list_recorder().fill_path({
            .path = check_mark_path(checkbox_rect),
            .color = tick_color,
            .translation = checkbox_rect.location().to_type<float>(),
        });
    } else {
        auto background_color = input_colors.background_color(enabled);
        auto border_thickness = max(1, checkbox_rect.width() / 10);
        context.display_list_recorder().fill_rect_with_rounded_corners(checkbox_rect, modify_color(input_colors.border_color(enabled)), checkbox_radius);
        context.display_list_recorder().fill_rect_with_rounded_corners(checkbox_rect.shrunken(border_thickness, border_thickness, border_thickness, border_thickness),
            background_color, max(0, checkbox_radius - border_thickness));
        if (checkbox.indeterminate()) {
            int radius = 0.05 * checkbox_rect.width();
            auto dash_color = increase_contrast(input_colors.dark_gray, background_color);
            auto dash_rect = checkbox_rect.inflated(-0.4 * checkbox_rect.width(), -0.8 * checkbox_rect.height());
            context.display_list_recorder().fill_rect_with_rounded_corners(dash_rect, dash_color, radius, radius, radius, radius);
        }
    }
}

}
