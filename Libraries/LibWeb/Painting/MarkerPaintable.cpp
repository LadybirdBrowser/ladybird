/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
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

void MarkerPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (phase == PaintPhase::Overlay)
        PaintableBox::paint(context, phase);
    if (phase != PaintPhase::Foreground)
        return;

    CSSPixelRect enclosing = absolute_rect().to_rounded<CSSPixels>();
    auto device_enclosing = context.enclosing_device_rect(enclosing);

    CSSPixels marker_width = enclosing.height() / 2;

    if (auto const* list_style_image = layout_box().list_style_image()) {
        CSSPixelRect image_rect {
            0, 0,
            list_style_image->natural_width().value_or(marker_width),
            list_style_image->natural_height().value_or(marker_width)
        };
        image_rect.center_within(enclosing);

        auto device_image_rect = context.enclosing_device_rect(image_rect);
        list_style_image->resolve_for_size(layout_box(), image_rect.size());
        list_style_image->paint(context, device_image_rect, computed_values().image_rendering());
        return;
    }

    CSSPixelRect marker_rect { 0, 0, marker_width, marker_width };
    marker_rect.center_within(enclosing);
    auto device_marker_rect = context.enclosing_device_rect(marker_rect);

    float left = device_marker_rect.x().value();
    float right = left + device_marker_rect.width().value();
    float top = device_marker_rect.y().value();
    float bottom = top + device_marker_rect.height().value();

    auto color = computed_values().color();

    if (auto text = layout_box().text(); text.has_value()) {
        // FIXME: This should use proper text layout logic!
        // This does not line up with the text in the <li> element which looks very sad :(
        context.display_list_recorder().draw_text(device_enclosing.to_type<int>(), *text, layout_box().scaled_font(context), Gfx::TextAlignment::Center, color);
    } else if (auto const* counter_style = layout_box().list_style_type().get_pointer<CSS::CounterStyleNameKeyword>()) {
        switch (*counter_style) {
        case CSS::CounterStyleNameKeyword::Square:
            context.display_list_recorder().fill_rect(device_marker_rect.to_type<int>(), color);
            break;
        case CSS::CounterStyleNameKeyword::Circle:
            context.display_list_recorder().draw_ellipse(device_marker_rect.to_type<int>(), color, 1);
            break;
        case CSS::CounterStyleNameKeyword::Disc:
            context.display_list_recorder().fill_ellipse(device_marker_rect.to_type<int>(), color);
            break;
        case CSS::CounterStyleNameKeyword::DisclosureClosed: {
            // https://drafts.csswg.org/css-counter-styles-3/#disclosure-closed
            // For the disclosure-open and disclosure-closed counter styles, the marker must be an image or character suitable for indicating the open and closed states of a disclosure widget, such as HTML’s details element.
            // FIXME: If the image is directional, it must respond to the writing mode of the element, similar to the bidi-sensitive images feature of the Images 4 module.

            // Draw an equilateral triangle pointing right.
            auto path = Gfx::Path();
            path.move_to({ left, top });
            path.line_to({ left + sin_60_deg * (right - left), (top + bottom) / 2 });
            path.line_to({ left, bottom });
            path.close();
            context.display_list_recorder().fill_path({ .path = path, .color = color, .winding_rule = Gfx::WindingRule::EvenOdd });
            break;
        }
        case CSS::CounterStyleNameKeyword::DisclosureOpen: {
            // https://drafts.csswg.org/css-counter-styles-3/#disclosure-open
            // For the disclosure-open and disclosure-closed counter styles, the marker must be an image or character suitable for indicating the open and closed states of a disclosure widget, such as HTML’s details element.
            // FIXME: If the image is directional, it must respond to the writing mode of the element, similar to the bidi-sensitive images feature of the Images 4 module.

            // Draw an equilateral triangle pointing down.
            auto path = Gfx::Path();
            path.move_to({ left, top });
            path.line_to({ right, top });
            path.line_to({ (left + right) / 2, top + sin_60_deg * (bottom - top) });
            path.close();
            context.display_list_recorder().fill_path({ .path = path, .color = color, .winding_rule = Gfx::WindingRule::EvenOdd });
            break;
        }
        case CSS::CounterStyleNameKeyword::None:
            return;
        case CSS::CounterStyleNameKeyword::Decimal:
        case CSS::CounterStyleNameKeyword::DecimalLeadingZero:
        case CSS::CounterStyleNameKeyword::LowerAlpha:
        case CSS::CounterStyleNameKeyword::LowerLatin:
        case CSS::CounterStyleNameKeyword::LowerRoman:
        case CSS::CounterStyleNameKeyword::UpperAlpha:
        case CSS::CounterStyleNameKeyword::UpperLatin:
        case CSS::CounterStyleNameKeyword::UpperRoman:
            // These are handled by text() already.
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

}
