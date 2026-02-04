/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ShadowPainting.h>

namespace Web::Painting {

void paint_box_shadow(DisplayListRecordingContext& context,
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
                .top_left = border_radii.top_left.as_corner(context.device_pixel_converter()),
                .top_right = border_radii.top_right.as_corner(context.device_pixel_converter()),
                .bottom_right = border_radii.bottom_right.as_corner(context.device_pixel_converter()),
                .bottom_left = border_radii.bottom_left.as_corner(context.device_pixel_converter()) },
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
            context.display_list_recorder().paint_inner_box_shadow(params);
        } else {
            ScopedCornerRadiusClip corner_clipper { context, device_content_rect, border_radii, CornerClip::Inside };
            context.display_list_recorder().paint_outer_box_shadow(params);
        }
    }
}

void paint_text_shadow(DisplayListRecordingContext& context, PaintableFragment::FragmentSpan const& span)
{
    auto const& fragment = span.fragment;
    auto const& shadow_layers = span.shadow_layers.has_value() ? *span.shadow_layers : fragment.shadows();

    if (shadow_layers.is_empty())
        return;

    auto glyph_run = fragment.glyph_run();
    if (!glyph_run || glyph_run->glyphs().is_empty())
        return;

    // If this is a partial span, slice the glyph run to only include the relevant glyphs.
    auto const& glyphs = glyph_run->glyphs();
    if (span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units()) {
        size_t start_glyph = 0;
        size_t glyph_count = 0;
        size_t code_unit_offset = 0;
        for (size_t i = 0; i < glyphs.size(); ++i) {
            if (code_unit_offset == span.start_code_unit)
                start_glyph = i;
            code_unit_offset += glyphs[i].length_in_code_units;
            if (code_unit_offset == span.end_code_unit) {
                glyph_count = i - start_glyph + 1;
                break;
            }
        }
        if (glyph_count > 0)
            glyph_run = glyph_run->slice(start_glyph, glyph_count);
    }

    auto fragment_width = context.enclosing_device_pixels(fragment.width()).value();
    auto fragment_height = context.enclosing_device_pixels(fragment.height()).value();
    auto fragment_baseline = context.rounded_device_pixels(fragment.baseline()).value();
    auto fragment_absolute_rect = fragment.absolute_rect();

    // Note: Box-shadow layers are ordered front-to-back, so we paint them in reverse
    for (auto const& layer : shadow_layers.in_reverse()) {
        int blur_radius = context.rounded_device_pixels(layer.blur_radius).value();

        // Space around the painted text to allow it to blur.
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

        // FIXME: this is close but not quite perfect. non integer scale values can be offset by tiny amounts.
        auto css_margin = layer.blur_radius * 2;
        auto scale = context.device_pixels_per_css_pixel();
        auto draw_location = Gfx::FloatPoint {
            fragment_absolute_rect.x() + layer.offset_x - css_margin,
            fragment_absolute_rect.y() + layer.offset_y - css_margin,
        } * (float)scale;

        context.display_list_recorder().paint_text_shadow(blur_radius, bounding_rect, text_rect.translated(0, fragment_baseline), *glyph_run, scale, layer.color, draw_location);
    }
}

}
