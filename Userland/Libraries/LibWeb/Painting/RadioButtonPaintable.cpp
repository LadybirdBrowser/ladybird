/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/StylePainter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/Label.h>
#include <LibWeb/Layout/RadioButton.h>
#include <LibWeb/Painting/InputColors.h>
#include <LibWeb/Painting/RadioButtonPaintable.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(RadioButtonPaintable);

JS::NonnullGCPtr<RadioButtonPaintable> RadioButtonPaintable::create(Layout::RadioButton const& layout_box)
{
    return layout_box.heap().allocate_without_realm<RadioButtonPaintable>(layout_box);
}

RadioButtonPaintable::RadioButtonPaintable(Layout::RadioButton const& layout_box)
    : LabelablePaintable(layout_box)
{
}

void RadioButtonPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto draw_circle = [&](auto const& rect, Color color) {
        // Note: Doing this is a bit more forgiving than draw_circle() which will round to the nearset even radius.
        // This will fudge it (which works better here).
        context.recording_painter().fill_rect_with_rounded_corners(rect, color, rect.width() / 2);
    };

    auto shrink_all = [&](auto const& rect, int amount) {
        return rect.shrunken(amount, amount, amount, amount);
    };

    auto const& radio_button = static_cast<HTML::HTMLInputElement const&>(layout_box().dom_node());

    auto& palette = context.palette();
    bool enabled = layout_box().dom_node().enabled();
    auto input_colors = compute_input_colors(palette, computed_values().accent_color());

    auto background_color = input_colors.background_color(enabled);
    auto accent = input_colors.accent;

    auto radio_color = [&] {
        if (radio_button.checked()) {
            // Handle the awkward case where a light color has been used for the accent color.
            if (accent.contrast_ratio(background_color) < 2 && accent.contrast_ratio(input_colors.dark_gray) > 2)
                background_color = input_colors.dark_gray;
            return accent;
        }
        return input_colors.gray;
    };

    auto fill_color = [&] {
        if (!enabled)
            return input_colors.mid_gray;
        auto color = radio_color();
        if (being_pressed())
            color = InputColors::get_shade(color, 0.3f, palette.is_dark());
        return color;
    }();

    // This is based on a 1px outer border and 2px inner border when drawn at 13x13.
    auto radio_button_rect = context.enclosing_device_rect(absolute_rect()).to_type<int>();
    auto outer_border_width = max(1, static_cast<int>(ceilf(radio_button_rect.width() / 13.0f)));
    auto inner_border_width = max(2, static_cast<int>(ceilf(radio_button_rect.width() / 4.0f)));

    draw_circle(radio_button_rect, fill_color);
    draw_circle(shrink_all(radio_button_rect, outer_border_width), background_color);
    if (radio_button.checked())
        draw_circle(shrink_all(radio_button_rect, inner_border_width), fill_color);
}

}
