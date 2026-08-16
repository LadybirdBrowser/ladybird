/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>

namespace Web::Painting {

NonnullRefPtr<ImagePaintable> ImagePaintable::create(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider)
{
    return adopt_ref(*new ImagePaintable(layout_box, image_provider));
}

ImagePaintable::ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider)
    : Paintable(layout_box)
    , m_image_provider(image_provider)
{
}

void ImagePaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    Paintable::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto image_rect = absolute_rect();
        auto image_rect_device_pixels = context.rounded_device_rect(image_rect);
        if (auto decoded_image_data = m_image_provider.decoded_image_data()) {
            ScopedCornerRadiusClip corner_clip { context, image_rect_device_pixels, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };
            auto image_int_rect_device_pixels = image_rect_device_pixels.to_type<int>();

            // https://drafts.csswg.org/css-images/#the-object-fit
            auto object_fit = layout_node().object_fit();

            CSS::SizeWithAspectRatio natural_size { m_image_provider.intrinsic_width(), m_image_provider.intrinsic_height(), m_image_provider.intrinsic_aspect_ratio() };
            auto concrete_object_size = CSS::run_default_sizing_algorithm({}, {}, natural_size, image_rect.size());

            auto draw_rect = get_replaced_box_painting_area(*this, context, object_fit, concrete_object_size);
            if (!draw_rect.is_empty()) {
                auto draw_rect_needs_clip = !image_int_rect_device_pixels.contains(draw_rect);
                if (draw_rect_needs_clip) {
                    context.display_list_recorder().save();
                    context.display_list_recorder().add_clip_rect(image_int_rect_device_pixels);
                }
                decoded_image_data->paint(context, draw_rect.to_type<float>(), layout_node().image_rendering(), layout_node().color_scheme());
                if (draw_rect_needs_clip)
                    context.display_list_recorder().restore();
            }
        }

        if (selection_state() != SelectionState::None) {
            auto selection_background_color = selection_style().background_color;
            if (selection_background_color.alpha() > 0)
                context.display_list_recorder().fill_rect(image_rect_device_pixels.to_type<int>(), selection_background_color);
        }
    }
}

}
