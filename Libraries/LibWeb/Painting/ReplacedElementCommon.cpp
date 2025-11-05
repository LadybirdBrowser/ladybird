/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

Gfx::IntRect get_replaced_box_painting_area(PaintableBox const& paintable, DisplayListRecordingContext const& context, CSS::ObjectFit object_fit, Gfx::IntSize content_size)
{
    if (content_size.is_empty())
        return {};

    auto paintable_rect = paintable.absolute_rect();
    if (paintable_rect.is_empty())
        return {};

    auto paintable_rect_device_pixels = context.rounded_device_rect(paintable_rect);

    auto bitmap_aspect_ratio = CSSPixels(content_size.height()) / content_size.width();
    auto image_aspect_ratio = paintable_rect.height() / paintable_rect.width();

    auto scale_x = CSSPixelFraction(1);
    auto scale_y = CSSPixelFraction(1);

    if (object_fit == CSS::ObjectFit::ScaleDown) {
        if (content_size.width() > paintable_rect.width() || content_size.height() > paintable_rect.height()) {
            object_fit = CSS::ObjectFit::Contain;
        } else {
            object_fit = CSS::ObjectFit::None;
        }
    }

    switch (object_fit) {
    case CSS::ObjectFit::Fill:
        scale_x = paintable_rect.width() / content_size.width();
        scale_y = paintable_rect.height() / content_size.height();
        break;
    case CSS::ObjectFit::Contain:
        if (bitmap_aspect_ratio >= image_aspect_ratio) {
            scale_x = paintable_rect.height() / content_size.height();
            scale_y = scale_x;
        } else {
            scale_x = paintable_rect.width() / content_size.width();
            scale_y = scale_x;
        }
        break;
    case CSS::ObjectFit::Cover:
        if (bitmap_aspect_ratio >= image_aspect_ratio) {
            scale_x = paintable_rect.width() / content_size.width();
            scale_y = scale_x;
        } else {
            scale_x = paintable_rect.height() / content_size.height();
            scale_y = scale_x;
        }
        break;
    case CSS::ObjectFit::ScaleDown:
        VERIFY_NOT_REACHED(); // handled outside the switch-case
    case CSS::ObjectFit::None:
        break;
    }

    auto scaled_bitmap_width = CSSPixels(content_size.width()) * scale_x;
    auto scaled_bitmap_height = CSSPixels(content_size.height()) * scale_y;

    auto residual_horizontal = paintable_rect.width() - scaled_bitmap_width;
    auto residual_vertical = paintable_rect.height() - scaled_bitmap_height;

    // https://drafts.csswg.org/css-images/#the-object-position
    auto const& object_position = paintable.computed_values().object_position();

    auto offset_x = CSSPixels::from_raw(0);
    if (object_position.edge_x == CSS::PositionEdge::Left) {
        offset_x = object_position.offset_x.to_px(paintable.layout_node(), residual_horizontal);
    } else if (object_position.edge_x == CSS::PositionEdge::Right) {
        offset_x = residual_horizontal - object_position.offset_x.to_px(paintable.layout_node(), residual_horizontal);
    }

    auto offset_y = CSSPixels::from_raw(0);
    if (object_position.edge_y == CSS::PositionEdge::Top) {
        offset_y = object_position.offset_y.to_px(paintable.layout_node(), residual_vertical);
    } else if (object_position.edge_y == CSS::PositionEdge::Bottom) {
        offset_y = residual_vertical - object_position.offset_y.to_px(paintable.layout_node(), residual_vertical);
    }

    return Gfx::IntRect(
        paintable_rect_device_pixels.x().value() + context.rounded_device_pixels(offset_x).value(),
        paintable_rect_device_pixels.y().value() + context.rounded_device_pixels(offset_y).value(),
        context.rounded_device_pixels(scaled_bitmap_width).value(),
        context.rounded_device_pixels(scaled_bitmap_height).value());
}

}
