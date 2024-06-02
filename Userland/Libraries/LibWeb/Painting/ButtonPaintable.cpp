/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Layout/ButtonBox.h>
#include <LibWeb/Layout/Label.h>
#include <LibWeb/Painting/ButtonPaintable.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(ButtonPaintable);

JS::NonnullGCPtr<ButtonPaintable> ButtonPaintable::create(Layout::ButtonBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<ButtonPaintable>(layout_box);
}

ButtonPaintable::ButtonPaintable(Layout::ButtonBox const& layout_box)
    : LabelablePaintable(layout_box)
{
}

Layout::ButtonBox const& ButtonPaintable::layout_box() const
{
    return static_cast<Layout::ButtonBox const&>(layout_node());
}

Layout::ButtonBox& ButtonPaintable::layout_box()
{
    return static_cast<Layout::ButtonBox&>(layout_node());
}

void ButtonPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    auto const& dom_node = layout_box().dom_node();
    if (is<HTML::HTMLInputElement>(dom_node) && phase == PaintPhase::Foreground) {
        auto button_rect = context.enclosing_device_rect(absolute_rect());
        auto text_rect = button_rect;

        // Apply CSS text-indent property to text rect
        // FIXME: The second parameter to to_px() needs to be the block container’s own inline-axis inner size:
        //        https://drafts.csswg.org/css-text-3/#propdef-text-indent
        auto text_indent = computed_values().text_indent().to_px(layout_box(), CSSPixels());
        text_rect.translate_by(context.rounded_device_pixels(text_indent), 0);

        // Apply button pressed state offset
        if (being_pressed()) {
            auto offset = context.rounded_device_pixels(1);
            text_rect.translate_by(offset, offset);
        }

        // Paint button text clipped to button rect
        auto& painter = context.recording_painter();
        painter.save();
        auto clip_rect = absolute_rect();
        clip_rect.translate_by(enclosing_scroll_frame_offset().value_or({}));
        painter.add_clip_rect(context.enclosing_device_rect(clip_rect).to_type<int>());
        painter.draw_text(
            text_rect.to_type<int>(),
            static_cast<HTML::HTMLInputElement const&>(dom_node).value(),
            layout_box().scaled_font(context.device_pixels_per_css_pixel()),
            Gfx::TextAlignment::Center,
            computed_values().color());
        painter.restore();
    }
}

}
