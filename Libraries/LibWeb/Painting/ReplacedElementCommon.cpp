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

static CSSPixelRect resolve_replaced_content_rect(PaintableBox const& paintable, CSSPixelSize concrete_size);

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
    CSSPixelRect destination_rect = resolve_replaced_content_rect(paintable, { scaled_bitmap_width, scaled_bitmap_height });

    return Gfx::IntRect(
        paintable_rect_device_pixels.x().value() + context.rounded_device_pixels(destination_rect.x() - paintable_rect.x()).value(),
        paintable_rect_device_pixels.y().value() + context.rounded_device_pixels(destination_rect.y() - paintable_rect.y()).value(),
        context.rounded_device_pixels(destination_rect.width()).value(),
        context.rounded_device_pixels(destination_rect.height()).value());
}

CSSPixelRect get_replaced_content_rect(PaintableBox const& paintable, CSS::SizeWithAspectRatio const& natural_size)
{
    CSSPixelRect paintable_rect = paintable.absolute_rect();
    CSSPixelSize concrete_size = CSS::replaced_object_fit_size(paintable.computed_values().object_fit(), natural_size, paintable_rect.size());
    return resolve_replaced_content_rect(paintable, concrete_size);
}

static CSSPixelRect resolve_replaced_content_rect(PaintableBox const& paintable, CSSPixelSize concrete_size)
{
    CSSPixelRect paintable_rect = paintable.absolute_rect();
    CSSPixels residual_horizontal = paintable_rect.width() - concrete_size.width();
    CSSPixels residual_vertical = paintable_rect.height() - concrete_size.height();

    // https://drafts.csswg.org/css-images-3/#the-object-position
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

    return CSSPixelRect({ paintable_rect.x() + offset_x, paintable_rect.y() + offset_y }, concrete_size);
}

}
