/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibGfx/DisjointRectSet.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Painter.h>
#include <LibWeb/Layout/LineBoxFragment.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BorderPainting.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintContext.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

struct OuterBoxShadowMetrics {
    Gfx::IntRect shadow_bitmap_rect;
    Gfx::IntRect non_blurred_shadow_rect;
    Gfx::IntRect inner_bounding_rect;
    int blurred_edge_thickness;
    int double_radius;
    int blur_radius;

    Gfx::IntRect top_left_corner_rect;
    Gfx::IntRect top_right_corner_rect;
    Gfx::IntRect bottom_right_corner_rect;
    Gfx::IntRect bottom_left_corner_rect;

    Gfx::IntPoint top_left_corner_blit_pos;
    Gfx::IntPoint top_right_corner_blit_pos;
    Gfx::IntPoint bottom_right_corner_blit_pos;
    Gfx::IntPoint bottom_left_corner_blit_pos;

    Gfx::IntSize top_left_corner_size;
    Gfx::IntSize top_right_corner_size;
    Gfx::IntSize bottom_right_corner_size;
    Gfx::IntSize bottom_left_corner_size;

    int left_start;
    int top_start;
    int right_start;
    int bottom_start;

    Gfx::IntRect left_edge_rect;
    Gfx::IntRect right_edge_rect;
    Gfx::IntRect top_edge_rect;
    Gfx::IntRect bottom_edge_rect;

    CornerRadius top_left_shadow_corner;
    CornerRadius top_right_shadow_corner;
    CornerRadius bottom_right_shadow_corner;
    CornerRadius bottom_left_shadow_corner;
};

static OuterBoxShadowMetrics get_outer_box_shadow_configuration(PaintBoxShadowParams params)
{
    auto device_content_rect = params.device_content_rect;

    auto top_left_corner = params.corner_radii.top_left;
    auto top_right_corner = params.corner_radii.top_right;
    auto bottom_right_corner = params.corner_radii.bottom_right;
    auto bottom_left_corner = params.corner_radii.bottom_left;

    auto offset_x = params.offset_x;
    auto offset_y = params.offset_y;
    auto blur_radius = params.blur_radius;
    auto spread_distance = params.spread_distance;

    // Our blur cannot handle radii over 255 so there's no point trying (255 is silly big anyway)
    blur_radius = clamp(blur_radius, 0, 255);

    auto top_left_shadow_corner = top_left_corner;
    auto top_right_shadow_corner = top_right_corner;
    auto bottom_right_shadow_corner = bottom_right_corner;
    auto bottom_left_shadow_corner = bottom_left_corner;

    auto spread_corner = [&](auto& corner) {
        if (corner) {
            corner.horizontal_radius += spread_distance;
            corner.vertical_radius += spread_distance;
        }
    };

    spread_corner(top_left_shadow_corner);
    spread_corner(top_right_shadow_corner);
    spread_corner(bottom_right_shadow_corner);
    spread_corner(bottom_left_shadow_corner);

    auto expansion = spread_distance - (blur_radius * 2);
    Gfx::IntRect inner_bounding_rect = {
        device_content_rect.x() + offset_x - expansion,
        device_content_rect.y() + offset_y - expansion,
        device_content_rect.width() + 2 * expansion,
        device_content_rect.height() + 2 * expansion
    };

    // Calculating and blurring the box-shadow full size is expensive, and wasteful - aside from the corners,
    // all vertical strips of the shadow are identical, and the same goes for horizontal ones.
    // So instead, we generate a shadow bitmap that is just large enough to include the corners and 1px of
    // non-corner, and then we repeatedly blit sections of it. This is similar to a NinePatch on Android.
    auto double_radius = blur_radius * 2;
    auto blurred_edge_thickness = blur_radius * 4;

    auto default_corner_size = Gfx::IntSize { double_radius, double_radius };
    auto top_left_corner_size = top_left_shadow_corner ? top_left_shadow_corner.as_rect().size() : default_corner_size;
    auto top_right_corner_size = top_right_shadow_corner ? top_right_shadow_corner.as_rect().size() : default_corner_size;
    auto bottom_left_corner_size = bottom_left_shadow_corner ? bottom_left_shadow_corner.as_rect().size() : default_corner_size;
    auto bottom_right_corner_size = bottom_right_shadow_corner ? bottom_right_shadow_corner.as_rect().size() : default_corner_size;

    auto non_blurred_shadow_rect = device_content_rect.inflated(spread_distance, spread_distance, spread_distance, spread_distance);

    auto max_edge_width = non_blurred_shadow_rect.width() / 2;
    auto max_edge_height = non_blurred_shadow_rect.height() / 2;
    auto extra_edge_width = non_blurred_shadow_rect.width() % 2;
    auto extra_edge_height = non_blurred_shadow_rect.height() % 2;

    auto clip_corner_size = [&](auto& size, auto const& corner, int x_bonus = 0, int y_bonus = 0) {
        auto max_x = max_edge_width + x_bonus;
        auto max_y = max_edge_height + y_bonus;
        auto min_x = max(corner.horizontal_radius, min(double_radius, max_x));
        auto min_y = max(corner.vertical_radius, min(double_radius, max_y));
        if (min_x <= max_x)
            size.set_width(clamp(size.width(), min_x, max_x));
        if (min_y <= max_y)
            size.set_height(clamp(size.height(), min_y, max_y));
    };

    clip_corner_size(top_left_corner_size, top_left_corner, extra_edge_width, extra_edge_height);
    clip_corner_size(top_right_corner_size, top_right_corner, 0, extra_edge_height);
    clip_corner_size(bottom_left_corner_size, bottom_left_corner, extra_edge_width);
    clip_corner_size(bottom_right_corner_size, bottom_right_corner);

    auto shadow_bitmap_rect = Gfx::IntRect {
        0, 0,
        max(max(
                top_left_corner_size.width() + top_right_corner_size.width(),
                bottom_left_corner_size.width() + bottom_right_corner_size.width()),
            max(top_left_corner_size.width() + bottom_right_corner_size.width(),
                bottom_left_corner_size.width() + top_right_corner_size.width()))
            + 1 + blurred_edge_thickness,
        max(max(
                top_left_corner_size.height() + bottom_left_corner_size.height(),
                top_right_corner_size.height() + bottom_right_corner_size.height()),
            max(top_left_corner_size.height() + bottom_right_corner_size.height(),
                bottom_left_corner_size.height() + top_right_corner_size.height()))
            + 1 + blurred_edge_thickness
    };

    auto top_left_corner_rect = Gfx::IntRect {
        0, 0,
        top_left_corner_size.width() + double_radius,
        top_left_corner_size.height() + double_radius
    };
    auto top_right_corner_rect = Gfx::IntRect {
        shadow_bitmap_rect.width() - (top_right_corner_size.width() + double_radius), 0,
        top_right_corner_size.width() + double_radius,
        top_right_corner_size.height() + double_radius
    };
    auto bottom_right_corner_rect = Gfx::IntRect {
        shadow_bitmap_rect.width() - (bottom_right_corner_size.width() + double_radius),
        shadow_bitmap_rect.height() - (bottom_right_corner_size.height() + double_radius),
        bottom_right_corner_size.width() + double_radius,
        bottom_right_corner_size.height() + double_radius
    };
    auto bottom_left_corner_rect = Gfx::IntRect {
        0, shadow_bitmap_rect.height() - (bottom_left_corner_size.height() + double_radius),
        bottom_left_corner_size.width() + double_radius,
        bottom_left_corner_size.height() + double_radius
    };

    auto horizontal_edge_width = min(max_edge_height, double_radius) + double_radius;
    auto vertical_edge_width = min(max_edge_width, double_radius) + double_radius;
    auto horizontal_top_edge_width = min(max_edge_height + extra_edge_height, double_radius) + double_radius;
    auto vertical_left_edge_width = min(max_edge_width + extra_edge_width, double_radius) + double_radius;

    Gfx::IntRect left_edge_rect { 0, top_left_corner_rect.height(), vertical_left_edge_width, 1 };
    Gfx::IntRect right_edge_rect { shadow_bitmap_rect.width() - vertical_edge_width, top_right_corner_rect.height(), vertical_edge_width, 1 };
    Gfx::IntRect top_edge_rect { top_left_corner_rect.width(), 0, 1, horizontal_top_edge_width };
    Gfx::IntRect bottom_edge_rect { bottom_left_corner_rect.width(), shadow_bitmap_rect.height() - horizontal_edge_width, 1, horizontal_edge_width };

    auto left_start = inner_bounding_rect.left() - blurred_edge_thickness;
    auto right_start = inner_bounding_rect.left() + inner_bounding_rect.width() + (blurred_edge_thickness - vertical_edge_width);
    auto top_start = inner_bounding_rect.top() - blurred_edge_thickness;
    auto bottom_start = inner_bounding_rect.top() + inner_bounding_rect.height() + (blurred_edge_thickness - horizontal_edge_width);

    auto top_left_corner_blit_pos = inner_bounding_rect.top_left().translated(-blurred_edge_thickness, -blurred_edge_thickness);
    auto top_right_corner_blit_pos = inner_bounding_rect.top_right().translated(-top_right_corner_size.width() + double_radius, -blurred_edge_thickness);
    auto bottom_left_corner_blit_pos = inner_bounding_rect.bottom_left().translated(-blurred_edge_thickness, -bottom_left_corner_size.height() + double_radius);
    auto bottom_right_corner_blit_pos = inner_bounding_rect.bottom_right().translated(-bottom_right_corner_size.width() + double_radius, -bottom_right_corner_size.height() + double_radius);

    return OuterBoxShadowMetrics {
        .shadow_bitmap_rect = shadow_bitmap_rect,
        .non_blurred_shadow_rect = non_blurred_shadow_rect,
        .inner_bounding_rect = inner_bounding_rect,
        .blurred_edge_thickness = blurred_edge_thickness,
        .double_radius = double_radius,
        .blur_radius = blur_radius,

        .top_left_corner_rect = top_left_corner_rect,
        .top_right_corner_rect = top_right_corner_rect,
        .bottom_right_corner_rect = bottom_right_corner_rect,
        .bottom_left_corner_rect = bottom_left_corner_rect,

        .top_left_corner_blit_pos = top_left_corner_blit_pos,
        .top_right_corner_blit_pos = top_right_corner_blit_pos,
        .bottom_right_corner_blit_pos = bottom_right_corner_blit_pos,
        .bottom_left_corner_blit_pos = bottom_left_corner_blit_pos,

        .top_left_corner_size = top_left_corner_size,
        .top_right_corner_size = top_right_corner_size,
        .bottom_right_corner_size = bottom_right_corner_size,
        .bottom_left_corner_size = bottom_left_corner_size,

        .left_start = left_start,
        .top_start = top_start,
        .right_start = right_start,
        .bottom_start = bottom_start,

        .left_edge_rect = left_edge_rect,
        .right_edge_rect = right_edge_rect,
        .top_edge_rect = top_edge_rect,
        .bottom_edge_rect = bottom_edge_rect,

        .top_left_shadow_corner = top_left_shadow_corner,
        .top_right_shadow_corner = top_right_shadow_corner,
        .bottom_right_shadow_corner = bottom_right_shadow_corner,
        .bottom_left_shadow_corner = bottom_left_shadow_corner,
    };
}

Gfx::IntRect get_outer_box_shadow_bounding_rect(PaintBoxShadowParams params)
{
    auto shadow_config = get_outer_box_shadow_configuration(params);

    auto const& top_left_corner_blit_pos = shadow_config.top_left_corner_blit_pos;
    auto const& top_right_corner_blit_pos = shadow_config.top_right_corner_blit_pos;
    auto const& bottom_left_corner_blit_pos = shadow_config.bottom_left_corner_blit_pos;
    auto const& top_right_corner_rect = shadow_config.top_right_corner_rect;
    auto const& bottom_left_corner_rect = shadow_config.bottom_left_corner_rect;

    return Gfx::IntRect {
        top_left_corner_blit_pos,
        { top_right_corner_blit_pos.x() - top_left_corner_blit_pos.x() + top_right_corner_rect.width(),
            bottom_left_corner_blit_pos.y() - top_left_corner_blit_pos.y() + bottom_left_corner_rect.height() }
    };
}

void paint_box_shadow(PaintContext& context,
    CSSPixelRect const& bordered_content_rect,
    CSSPixelRect const& borderless_content_rect,
    BordersData const& borders_data,
    BorderRadiiData const& border_radii,
    Vector<ShadowData> const& box_shadow_layers)
{
    // Note: Box-shadow layers are ordered front-to-back, so we paint them in reverse
    for (auto& box_shadow_data : box_shadow_layers.in_reverse()) {
        auto offset_x = context.rounded_device_pixels(box_shadow_data.offset_x);
        auto offset_y = context.rounded_device_pixels(box_shadow_data.offset_y);
        auto blur_radius = context.rounded_device_pixels(box_shadow_data.blur_radius);
        auto spread_distance = context.rounded_device_pixels(box_shadow_data.spread_distance);

        DevicePixelRect device_content_rect;
        if (box_shadow_data.placement == ShadowPlacement::Inner) {
            device_content_rect = context.rounded_device_rect(borderless_content_rect);
        } else {
            device_content_rect = context.rounded_device_rect(bordered_content_rect);
        }

        auto params = PaintBoxShadowParams {
            .color = box_shadow_data.color,
            .placement = box_shadow_data.placement,
            .corner_radii = CornerRadii {
                .top_left = border_radii.top_left.as_corner(context),
                .top_right = border_radii.top_right.as_corner(context),
                .bottom_right = border_radii.bottom_right.as_corner(context),
                .bottom_left = border_radii.bottom_left.as_corner(context) },
            .offset_x = offset_x.value(),
            .offset_y = offset_y.value(),
            .blur_radius = blur_radius.value(),
            .spread_distance = spread_distance.value(),
            .device_content_rect = device_content_rect.to_type<int>(),
        };

        if (box_shadow_data.placement == ShadowPlacement::Inner) {
            auto shrinked_border_radii = border_radii;
            shrinked_border_radii.shrink(borders_data.top.width, borders_data.right.width, borders_data.bottom.width, borders_data.left.width);
            ScopedCornerRadiusClip corner_clipper { context, device_content_rect, shrinked_border_radii, CornerClip::Outside };
            context.display_list_recorder().paint_inner_box_shadow_params(params);
        } else {
            ScopedCornerRadiusClip corner_clipper { context, device_content_rect, border_radii, CornerClip::Inside };
            context.display_list_recorder().paint_outer_box_shadow_params(params);
        }
    }
}

void paint_text_shadow(PaintContext& context, PaintableFragment const& fragment, Vector<ShadowData> const& shadow_layers)
{
    if (shadow_layers.is_empty())
        return;

    auto glyph_run = fragment.glyph_run();
    if (!glyph_run || glyph_run->glyphs().is_empty())
        return;

    auto fragment_width = context.enclosing_device_pixels(fragment.width()).value();
    auto fragment_height = context.enclosing_device_pixels(fragment.height()).value();
    auto draw_rect = context.enclosing_device_rect(fragment.absolute_rect()).to_type<int>();
    auto fragment_baseline = context.rounded_device_pixels(fragment.baseline()).value();

    // Note: Box-shadow layers are ordered front-to-back, so we paint them in reverse
    for (auto& layer : shadow_layers.in_reverse()) {
        int offset_x = context.rounded_device_pixels(layer.offset_x).value();
        int offset_y = context.rounded_device_pixels(layer.offset_y).value();
        int blur_radius = context.rounded_device_pixels(layer.blur_radius).value();

        // Space around the painted text to allow it to blur.
        // FIXME: Include spread in this once we use that.
        int margin = blur_radius * 2;
        Gfx::IntRect text_rect {
            margin, margin,
            fragment_width, fragment_height
        };
        Gfx::IntRect bounding_rect {
            0, 0,
            text_rect.width() + margin + margin,
            text_rect.height() + margin + margin
        };
        Gfx::IntPoint draw_location {
            draw_rect.x() + offset_x - margin,
            draw_rect.y() + offset_y - margin
        };

        context.display_list_recorder().paint_text_shadow(blur_radius, bounding_rect, text_rect.translated(0, fragment_baseline), *glyph_run, context.device_pixels_per_css_pixel(), layer.color, draw_location);
    }
}

}
