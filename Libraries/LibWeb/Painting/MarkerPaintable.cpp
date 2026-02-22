/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/MarkerPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(MarkerPaintable);

GC::Ref<MarkerPaintable> MarkerPaintable::create(Layout::ListItemMarkerBox const& layout_box)
{
    return layout_box.heap().allocate<MarkerPaintable>(layout_box);
}

MarkerPaintable::MarkerPaintable(Layout::ListItemMarkerBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::ListItemMarkerBox const& MarkerPaintable::layout_box() const
{
    return static_cast<Layout::ListItemMarkerBox const&>(layout_node());
}

constexpr float sin_60_deg = 0.866025403f;

void MarkerPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    if (phase == PaintPhase::Overlay)
        PaintableBox::paint(context, phase);
    if (phase != PaintPhase::Foreground)
        return;

    auto marker_rect = absolute_rect().to_rounded<CSSPixels>();
    auto device_rect = context.enclosing_device_rect(marker_rect);

    if (auto const* list_style_image = layout_box().list_style_image()) {
        list_style_image->resolve_for_size(layout_box(), marker_rect.size());
        list_style_image->paint(context, device_rect, computed_values().image_rendering());
        return;
    }

    auto const& list_style_type = layout_box().list_style_type();

    VERIFY(!list_style_type.has<Empty>());

    auto color = computed_values().color();

    if (auto text = layout_box().text(); text.has_value()) {
        // FIXME: This should use proper text layout logic!
        // This does not line up with the text in the <li> element which looks very sad :(
        context.display_list_recorder().draw_text(device_rect.to_type<int>(), Utf16String::from_utf8(*text), layout_box().font(context), Gfx::TextAlignment::Center, color);
        return;
    }

    auto const& counter_style = list_style_type.get<Optional<CSS::CounterStyle const&>>();
    VERIFY(Layout::ListItemMarkerBox::counter_style_is_rendered_with_custom_image(counter_style));

    if (counter_style->name() == "square"_fly_string) {
        context.display_list_recorder().fill_rect(device_rect.to_type<int>(), color);
        return;
    }

    if (counter_style->name() == "circle"_fly_string) {
        context.display_list_recorder().draw_ellipse(device_rect.to_type<int>(), color, 1);
        return;
    }

    if (counter_style->name() == "disc"_fly_string) {
        context.display_list_recorder().fill_ellipse(device_rect.to_type<int>(), color);
        return;
    }

    float left = device_rect.x().value();
    float right = left + device_rect.width().value();
    float top = device_rect.y().value();
    float bottom = top + device_rect.height().value();

    if (counter_style->name() == "disclosure-closed"_fly_string) {
        // https://drafts.csswg.org/css-counter-styles-3/#disclosure-closed
        // For the disclosure-open and disclosure-closed counter styles, the marker must be an image or character
        // suitable for indicating the open and closed states of a disclosure widget, such as HTML’s details element.
        // FIXME: If the image is directional, it must respond to the writing mode of the element, similar to the
        //        bidi-sensitive images feature of the Images 4 module.

        // Draw an equilateral triangle pointing right.
        auto path = Gfx::Path();
        path.move_to({ left, top });
        path.line_to({ left + sin_60_deg * (right - left), (top + bottom) / 2 });
        path.line_to({ left, bottom });
        path.close();
        context.display_list_recorder().fill_path({ .path = path, .paint_style_or_color = color, .winding_rule = Gfx::WindingRule::EvenOdd });
        return;
    }

    if (counter_style->name() == "disclosure-open"_fly_string) {
        // https://drafts.csswg.org/css-counter-styles-3/#disclosure-open
        // For the disclosure-open and disclosure-closed counter styles, the marker must be an image or character
        // suitable for indicating the open and closed states of a disclosure widget, such as HTML’s details element.
        // FIXME: If the image is directional, it must respond to the writing mode of the element, similar to the
        //        bidi-sensitive images feature of the Images 4 module.

        // Draw an equilateral triangle pointing down.
        auto path = Gfx::Path();
        path.move_to({ left, top });
        path.line_to({ right, top });
        path.line_to({ (left + right) / 2, top + sin_60_deg * (bottom - top) });
        path.close();
        context.display_list_recorder().fill_path({ .path = path, .paint_style_or_color = color, .winding_rule = Gfx::WindingRule::EvenOdd });
        return;
    }

    VERIFY_NOT_REACHED();
}

}
