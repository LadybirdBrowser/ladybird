/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

static RefPtr<DisplayList> compute_text_clip_paths(PaintContext& context, Paintable const& paintable, CSSPixelPoint containing_block_location)
{
    auto text_clip_paths = DisplayList::create();
    DisplayListRecorder display_list_recorder(*text_clip_paths);
    // Remove containing block offset, so executing the display list will produce mask at (0, 0)
    display_list_recorder.translate(-context.floored_device_point(containing_block_location).to_type<int>());
    auto add_text_clip_path = [&](PaintableFragment const& fragment) {
        auto glyph_run = fragment.glyph_run();
        if (!glyph_run || glyph_run->glyphs().is_empty())
            return;

        auto fragment_absolute_rect = fragment.absolute_rect();
        auto fragment_absolute_device_rect = context.enclosing_device_rect(fragment_absolute_rect);

        auto scale = context.device_pixels_per_css_pixel();
        auto baseline_start = Gfx::FloatPoint {
            fragment_absolute_rect.x().to_float(),
            fragment_absolute_rect.y().to_float() + fragment.baseline().to_float(),
        } * scale;
        display_list_recorder.draw_text_run(baseline_start, *glyph_run, Gfx::Color::Black, fragment_absolute_device_rect.to_type<int>(), scale, fragment.orientation());
    };

    paintable.for_each_in_inclusive_subtree([&](auto& paintable) {
        if (is<PaintableWithLines>(paintable)) {
            auto const& paintable_lines = static_cast<PaintableWithLines const&>(paintable);
            for (auto const& fragment : paintable_lines.fragments()) {
                if (is<Layout::TextNode>(fragment.layout_node()))
                    add_text_clip_path(fragment);
            }
        }
        return TraversalDecision::Continue;
    });

    return text_clip_paths;
}

static BackgroundBox get_box(CSS::BackgroundBox box_clip, BackgroundBox border_box, auto const& paintable_box)
{
    auto box = border_box;
    switch (box_clip) {
    case CSS::BackgroundBox::ContentBox: {
        auto& padding = paintable_box.box_model().padding;
        box.shrink(padding.top, padding.right, padding.bottom, padding.left);
        [[fallthrough]];
    }
    case CSS::BackgroundBox::PaddingBox: {
        auto& border = paintable_box.box_model().border;
        box.shrink(border.top, border.right, border.bottom, border.left);
        [[fallthrough]];
    }
    case CSS::BackgroundBox::BorderBox:
    default:
        return box;
    }
}

// https://www.w3.org/TR/css-backgrounds-3/#backgrounds
void paint_background(PaintContext& context, PaintableBox const& paintable_box, CSS::ImageRendering image_rendering, ResolvedBackground resolved_background, BorderRadiiData const& border_radii)
{
    auto& display_list_recorder = context.display_list_recorder();

    // https://drafts.fxtf.org/compositing/#background-blend-mode
    // Background layers must not blend with the content that is behind the element,
    // instead they must act as if they are rendered into an isolated group.
    // OPTIMIZATION: It is only required to render the element into an isolated group,
    //               if a background blend mode other than normal are used.
    auto paint_into_isolated_group = any_of(resolved_background.layers, [](auto const& layer) {
        return layer.blend_mode != CSS::MixBlendMode::Normal;
    });
    if (paint_into_isolated_group) {
        display_list_recorder.save_layer();
    }

    DisplayListRecorderStateSaver state { display_list_recorder };
    if (resolved_background.needs_text_clip) {
        auto display_list = compute_text_clip_paths(context, paintable_box, resolved_background.background_rect.location());
        auto rect = context.rounded_device_rect(resolved_background.background_rect);
        display_list_recorder.add_mask(move(display_list), rect.to_type<int>());
    }

    BackgroundBox border_box {
        resolved_background.background_rect,
        border_radii
    };

    auto const& color_box = resolved_background.color_box;

    display_list_recorder.fill_rect_with_rounded_corners(
        context.rounded_device_rect(color_box.rect).to_type<int>(),
        resolved_background.color,
        color_box.radii.top_left.as_corner(context),
        color_box.radii.top_right.as_corner(context),
        color_box.radii.bottom_right.as_corner(context),
        color_box.radii.bottom_left.as_corner(context));

    struct {
        DevicePixels top { 0 };
        DevicePixels bottom { 0 };
        DevicePixels left { 0 };
        DevicePixels right { 0 };
    } clip_shrink;

    auto border_top = paintable_box.computed_values().border_top();
    auto border_bottom = paintable_box.computed_values().border_bottom();
    auto border_left = paintable_box.computed_values().border_left();
    auto border_right = paintable_box.computed_values().border_right();

    if (border_top.color.alpha() == 255 && border_bottom.color.alpha() == 255
        && border_left.color.alpha() == 255 && border_right.color.alpha() == 255) {
        clip_shrink.top = context.rounded_device_pixels(border_top.width);
        clip_shrink.bottom = context.rounded_device_pixels(border_bottom.width);
        clip_shrink.left = context.rounded_device_pixels(border_left.width);
        clip_shrink.right = context.rounded_device_pixels(border_right.width);
    }

    // Note: Background layers are ordered front-to-back, so we paint them in reverse
    for (auto& layer : resolved_background.layers.in_reverse()) {
        DisplayListRecorderStateSaver state { display_list_recorder };

        // Clip
        auto clip_box = get_box(layer.clip, border_box, paintable_box);

        CSSPixelRect const& css_clip_rect = clip_box.rect;
        auto clip_rect = context.rounded_device_rect(css_clip_rect);
        display_list_recorder.add_clip_rect(clip_rect.to_type<int>());
        ScopedCornerRadiusClip corner_clip { context, context.rounded_device_rect(css_clip_rect), clip_box.radii };

        if (layer.clip == CSS::BackgroundBox::BorderBox) {
            // Shrink the effective clip rect if to account for the bits the borders will definitely paint over
            // (if they all have alpha == 255).
            clip_rect.shrink(clip_shrink.top, clip_shrink.right, clip_shrink.bottom, clip_shrink.left);
        }

        auto const& image = *layer.background_image;
        auto image_rect = layer.image_rect;
        auto background_positioning_area = layer.background_positioning_area;

        switch (layer.attachment) {
        case CSS::BackgroundAttachment::Fixed:
            background_positioning_area.set_location(paintable_box.layout_node().root().navigable()->viewport_scroll_offset());
            break;
        case CSS::BackgroundAttachment::Local:
            if (!paintable_box.is_viewport()) {
                auto scroll_offset = paintable_box.scroll_offset();
                background_positioning_area.translate_by(-scroll_offset.x(), -scroll_offset.y());
            }
            break;
        case CSS::BackgroundAttachment::Scroll:
            break;
        }

        if (background_positioning_area.is_empty())
            continue;

        if (layer.position_edge_x == CSS::PositionEdge::Right) {
            image_rect.set_right_without_resize(background_positioning_area.right() - layer.offset_x);
        } else {
            image_rect.set_left(background_positioning_area.left() + layer.offset_x);
        }

        if (layer.position_edge_y == CSS::PositionEdge::Bottom) {
            image_rect.set_bottom_without_resize(background_positioning_area.bottom() - layer.offset_y);
        } else {
            image_rect.set_top(background_positioning_area.top() + layer.offset_y);
        }

        // Repetition
        bool repeat_x = false;
        bool repeat_y = false;
        bool repeat_x_has_gap = false;
        bool repeat_y_has_gap = false;
        CSSPixels x_step = 0;
        CSSPixels y_step = 0;

        switch (layer.repeat_x) {
        case CSS::Repeat::Round:
            x_step = image_rect.width();
            repeat_x = true;
            break;
        case CSS::Repeat::Space: {
            int whole_images = (background_positioning_area.width() / image_rect.width()).to_int();
            if (whole_images <= 1) {
                x_step = image_rect.width();
                repeat_x = false;
            } else {
                auto space = fmod(background_positioning_area.width().to_double(), image_rect.width().to_double());
                x_step = image_rect.width() + CSSPixels::nearest_value_for(space / static_cast<double>(whole_images - 1));
                repeat_x = true;
                repeat_x_has_gap = true;
            }
            break;
        }
        case CSS::Repeat::Repeat:
            x_step = image_rect.width();
            repeat_x = true;
            break;
        case CSS::Repeat::NoRepeat:
            repeat_x = false;
            break;
        }
        // Move image_rect to the left-most tile position that is still visible
        if (repeat_x && image_rect.x() > css_clip_rect.x()) {
            auto x_delta = floor(x_step * ceil((image_rect.x() - css_clip_rect.x()) / x_step));
            image_rect.set_x(image_rect.x() - x_delta);
        }

        switch (layer.repeat_y) {
        case CSS::Repeat::Round:
            y_step = image_rect.height();
            repeat_y = true;
            break;
        case CSS::Repeat::Space: {
            int whole_images = (background_positioning_area.height() / image_rect.height()).to_int();
            if (whole_images <= 1) {
                y_step = image_rect.height();
                repeat_y = false;
            } else {
                auto space = fmod(background_positioning_area.height().to_float(), image_rect.height().to_float());
                y_step = image_rect.height() + CSSPixels::nearest_value_for(static_cast<double>(space) / static_cast<double>(whole_images - 1));
                repeat_y = true;
                repeat_y_has_gap = true;
            }
            break;
        }
        case CSS::Repeat::Repeat:
            y_step = image_rect.height();
            repeat_y = true;
            break;
        case CSS::Repeat::NoRepeat:
            repeat_y = false;
            break;
        }
        // Move image_rect to the top-most tile position that is still visible
        if (repeat_y && image_rect.y() > css_clip_rect.y()) {
            auto y_delta = floor(y_step * ceil((image_rect.y() - css_clip_rect.y()) / y_step));
            image_rect.set_y(image_rect.y() - y_delta);
        }

        CSSPixels initial_image_x = image_rect.x();
        CSSPixels image_y = image_rect.y();

        image.resolve_for_size(paintable_box.layout_node_with_style_and_box_metrics(), image_rect.size());

        auto for_each_image_device_rect = [&](auto callback) {
            while (image_y < css_clip_rect.bottom()) {
                image_rect.set_y(image_y);

                auto image_x = initial_image_x;
                while (image_x < css_clip_rect.right()) {
                    image_rect.set_x(image_x);
                    auto image_device_rect = context.rounded_device_rect(image_rect);
                    callback(image_device_rect);
                    if (!repeat_x)
                        break;
                    image_x += x_step;
                }

                if (!repeat_y)
                    break;
                image_y += y_step;
            }
        };

        Gfx::CompositingAndBlendingOperator compositing_and_blending_operator = mix_blend_mode_to_compositing_and_blending_operator(layer.blend_mode);
        if (compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal) {
            display_list_recorder.apply_compositing_and_blending_operator(compositing_and_blending_operator);
        }

        if (auto color = image.color_if_single_pixel_bitmap(); color.has_value()) {
            // OPTIMIZATION: If the image is a single pixel, we can just fill the whole area with it.
            //               However, we must first figure out the real coverage area, taking repeat etc into account.

            // FIXME: This could be written in a far more efficient way.
            DevicePixelRect fill_rect;
            for_each_image_device_rect([&](auto const& image_device_rect) {
                fill_rect.unite(image_device_rect);
            });
            display_list_recorder.fill_rect(fill_rect.to_type<int>(), color.value());
        } else if (is<CSS::ImageStyleValue>(image) && repeat_x && repeat_y && !repeat_x_has_gap && !repeat_y_has_gap) {
            // Use a dedicated painting command for repeated images instead of recording a separate command for each instance
            // of a repeated background, so the painter has the opportunity to optimize the painting of repeated images.
            auto dest_rect = context.rounded_device_rect(image_rect);
            auto const* bitmap = static_cast<CSS::ImageStyleValue const&>(image).current_frame_bitmap(dest_rect);
            auto scaling_mode = to_gfx_scaling_mode(image_rendering, bitmap->rect(), dest_rect.to_type<int>());
            context.display_list_recorder().draw_repeated_immutable_bitmap(dest_rect.to_type<int>(), clip_rect.to_type<int>(), *bitmap, scaling_mode, { .x = repeat_x, .y = repeat_y });
        } else {
            for_each_image_device_rect([&](auto const& image_device_rect) {
                image.paint(context, image_device_rect, image_rendering);
            });
        }

        if (compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal) {
            display_list_recorder.restore();
        }
    }

    if (paint_into_isolated_group) {
        display_list_recorder.restore();
    }
}

ResolvedBackground resolve_background_layers(Vector<CSS::BackgroundLayerData> const& layers, PaintableBox const& paintable_box, Color background_color, CSSPixelRect const& border_rect, BorderRadiiData const& border_radii)
{
    auto layer_is_paintable = [&](auto& layer) {
        return layer.background_image && layer.background_image->is_paintable();
    };

    BackgroundBox border_box {
        border_rect,
        border_radii
    };

    auto color_box = border_box;
    if (!layers.is_empty())
        color_box = get_box(layers.last().clip, border_box, paintable_box);

    Vector<ResolvedBackgroundLayerData> resolved_layers;
    for (auto const& layer : layers) {
        if (!layer_is_paintable(layer))
            continue;

        auto background_positioning_area = get_box(layer.origin, border_box, paintable_box).rect;
        auto const& image = *layer.background_image;

        Optional<CSSPixels> specified_width {};
        Optional<CSSPixels> specified_height {};
        if (layer.size_type == CSS::BackgroundSize::LengthPercentage) {
            if (!layer.size_x.is_auto())
                specified_width = layer.size_x.to_px(paintable_box.layout_node(), background_positioning_area.width());
            if (!layer.size_y.is_auto())
                specified_height = layer.size_y.to_px(paintable_box.layout_node(), background_positioning_area.height());
        }
        auto concrete_image_size = CSS::run_default_sizing_algorithm(
            specified_width, specified_height,
            image.natural_width(), image.natural_height(), image.natural_aspect_ratio(),
            background_positioning_area.size());

        // If any of these are zero, the NaNs will pop up in the painting code.
        if (background_positioning_area.is_empty() || concrete_image_size.is_empty()) {
            continue;
        }

        // Size
        CSSPixelRect image_rect;
        switch (layer.size_type) {
        case CSS::BackgroundSize::Contain: {
            double max_width_ratio = background_positioning_area.width().to_double() / concrete_image_size.width().to_double();
            double max_height_ratio = background_positioning_area.height().to_double() / concrete_image_size.height().to_double();
            double ratio = min(max_width_ratio, max_height_ratio);
            image_rect.set_size(concrete_image_size.width().scaled(ratio), concrete_image_size.height().scaled(ratio));
            break;
        }
        case CSS::BackgroundSize::Cover: {
            double max_width_ratio = background_positioning_area.width().to_double() / concrete_image_size.width().to_double();
            double max_height_ratio = background_positioning_area.height().to_double() / concrete_image_size.height().to_double();
            double ratio = max(max_width_ratio, max_height_ratio);
            image_rect.set_size(concrete_image_size.width().scaled(ratio), concrete_image_size.height().scaled(ratio));
            break;
        }
        case CSS::BackgroundSize::LengthPercentage:
            image_rect.set_size(concrete_image_size);
            break;
        }

        // If after sizing we have a 0px image, we're done. Attempting to paint this would be an infinite loop.
        if (image_rect.is_empty()) {
            continue;
        }

        // If background-repeat is round for one (or both) dimensions, there is a second step.
        // The UA must scale the image in that dimension (or both dimensions) so that it fits a
        // whole number of times in the background positioning area.
        if (layer.repeat_x == CSS::Repeat::Round || layer.repeat_y == CSS::Repeat::Round) {
            // If X ≠ 0 is the width of the image after step one and W is the width of the
            // background positioning area, then the rounded width X' = W / round(W / X)
            // where round() is a function that returns the nearest natural number
            // (integer greater than zero).
            if (layer.repeat_x == CSS::Repeat::Round) {
                image_rect.set_width(background_positioning_area.width() / round(background_positioning_area.width() / image_rect.width()));
            }
            if (layer.repeat_y == CSS::Repeat::Round) {
                image_rect.set_height(background_positioning_area.height() / round(background_positioning_area.height() / image_rect.height()));
            }

            // If background-repeat is round for one dimension only and if background-size is auto
            // for the other dimension, then there is a third step: that other dimension is scaled
            // so that the original aspect ratio is restored.
            if (layer.repeat_x != layer.repeat_y) {
                if (layer.size_x.is_auto()) {
                    image_rect.set_width(image_rect.height() * (concrete_image_size.width() / concrete_image_size.height()));
                }
                if (layer.size_y.is_auto()) {
                    image_rect.set_height(image_rect.width() * (concrete_image_size.height() / concrete_image_size.width()));
                }
            }
        }

        CSSPixels space_x = background_positioning_area.width() - image_rect.width();
        CSSPixels space_y = background_positioning_area.height() - image_rect.height();

        CSSPixels offset_x = layer.position_offset_x.to_px(paintable_box.layout_node(), space_x);
        CSSPixels offset_y = layer.position_offset_y.to_px(paintable_box.layout_node(), space_y);

        resolved_layers.append({ .background_image = layer.background_image,
            .attachment = layer.attachment,
            .clip = layer.clip,
            .position_edge_x = layer.position_edge_x,
            .position_edge_y = layer.position_edge_y,
            .offset_x = offset_x,
            .offset_y = offset_y,
            .background_positioning_area = background_positioning_area,
            .image_rect = image_rect,
            .repeat_x = layer.repeat_x,
            .repeat_y = layer.repeat_y,
            .blend_mode = layer.blend_mode });
    }

    return ResolvedBackground {
        .color_box = color_box,
        .layers = move(resolved_layers),
        .needs_text_clip = !layers.is_empty() && layers.last().clip == CSS::BackgroundBox::Text,
        .background_rect = border_rect,
        .color = background_color
    };
}

}
