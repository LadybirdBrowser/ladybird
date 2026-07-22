/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>

namespace Web::Painting {

NonnullRefPtr<ImagePaintable> ImagePaintable::create(Layout::ImageBox const& layout_box)
{
    return adopt_ref(*new ImagePaintable(layout_box, layout_box.image_provider()));
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
            auto object_fit = computed_values().object_fit();

            auto intrinsic_size = m_image_provider.intrinsic_size().value_or(image_rect.size());

            auto draw_rect = get_replaced_box_painting_area(*this, context, object_fit, intrinsic_size);
            if (!draw_rect.is_empty()) {
                auto draw_rect_needs_clip = !image_int_rect_device_pixels.contains(draw_rect);
                if (draw_rect_needs_clip) {
                    context.display_list_recorder().save();
                    context.display_list_recorder().add_clip_rect(image_int_rect_device_pixels);
                }
                decoded_image_data->paint(context, draw_rect, computed_values().image_rendering());
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

    if (phase == PaintPhase::Outline)
        paint_focused_area_outline(context);
}

void ImagePaintable::paint_focused_area_outline(DisplayListRecordingContext& context) const
{
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // The shapes of area elements in an image map associated with an img element that is being rendered and is not
    // inert.
    // NB: Focused area elements have no paintable of their own, so the image whose rendering makes the area's shape a
    //     focusable area paints the focus outline along that shape.
    auto const* area_element = as_if<HTML::HTMLAreaElement>(document().focused_area().ptr());
    if (!area_element)
        return;

    auto const* map_element = area_element->first_ancestor_of_type<HTML::HTMLMapElement>();
    if (!map_element)
        return;

    auto const* image_element = as_if<HTML::HTMLImageElement>(dom_node().ptr());
    if (!image_element || map_element->first_painted_image_with_focusable_shapes().ptr() != image_element)
        return;

    auto area_computed_values = area_element->computed_values();
    if (!area_computed_values)
        return;

    // AD-HOC: Only the user agent focus ring is painted. Other engines do not let author outline values style the
    //         focus indicator of an image map area.
    if (area_computed_values->outline_style() != CSS::OutlineStyle::Auto)
        return;

    auto outline_data = this->outline_data(*area_computed_values);
    if (!outline_data.has_value())
        return;

    auto image_rect = absolute_rect();
    auto path = area_element->shape_path(image_rect.size());
    if (!path.has_value())
        return;

    auto scale = static_cast<float>(context.device_pixels_per_css_pixel());
    auto device_origin = context.rounded_device_point(image_rect.location());
    Gfx::AffineTransform transform;
    transform.translate(device_origin.to_type<int>().to_type<float>());
    transform.scale(scale, scale);

    context.display_list_recorder().save();
    context.display_list_recorder().add_clip_rect(context.enclosing_device_rect(image_rect).to_type<int>());
    context.display_list_recorder().stroke_path({
        .cap_style = Gfx::Path::CapStyle::Round,
        .join_style = Gfx::Path::JoinStyle::Round,
        .miter_limit = 4,
        .dash_array = {},
        .dash_offset = 0,
        .path = path->copy_transformed(transform),
        .paint_style_or_color = outline_data->top.color,
        .thickness = static_cast<float>(outline_data->top.width.to_double()) * scale,
    });
    context.display_list_recorder().restore();
}

}
