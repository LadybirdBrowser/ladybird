/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/Painting/SVGImagePaintable.h>
#include <LibWeb/SVG/SVGImageElement.h>

namespace Web::Painting {

NonnullRefPtr<SVGImagePaintable> SVGImagePaintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new SVGImagePaintable(layout_box));
}

SVGImagePaintable::SVGImagePaintable(Layout::Box const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

void SVGImagePaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    // NB: An image has no geometry, so it contributes nothing to a clipping path.
    if (context.draw_svg_geometry_for_clip_path())
        return;

    if (!is_visible())
        return;

    SVGGraphicsPaintable::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    auto const& image_element = as<SVG::SVGImageElement>(*layout_box().dom_node());
    auto decoded_image_data = image_element.decoded_image_data();
    if (!decoded_image_data)
        return;

    // The image rect is in the viewport's user units; a fraction of a unit can span many device
    // pixels, so the positioning math stays in float and only the overflow clip quantizes.
    auto device_scale = static_cast<float>(context.device_pixels_per_css_pixel());
    auto image_rect = absolute_rect().to_type<float>().scaled(device_scale, device_scale);
    auto natural_size = image_element.intrinsic_size().map([](auto size) { return size.template to_type<float>(); }).value_or(image_rect.size());
    // FIXME: Respect the preserveAspectRatio attribute instead of assuming its default value.
    auto draw_rect = image_rect;
    if (natural_size.width() > 0 && natural_size.height() > 0) {
        auto contain_scale = min(image_rect.width() / natural_size.width(), image_rect.height() / natural_size.height());
        draw_rect = Gfx::FloatRect { {}, natural_size.scaled(contain_scale, contain_scale) }.centered_within(image_rect);
    }
    if (draw_rect.is_empty())
        return;

    // https://svgwg.org/svg2-draft/embedded.html#ImageElement
    // The user agent stylesheet sets the value of the overflow property on ‘image’ element to hidden. Unless
    // over-ridden by the author, images will therefore be clipped to the positioning rectangle defined by the
    // geometry properties.
    bool overflow_is_visible = layout_box().overflow_x() == CSS::Overflow::Visible
        && layout_box().overflow_y() == CSS::Overflow::Visible;
    bool draw_rect_needs_clip = !overflow_is_visible && !image_rect.contains(draw_rect);
    if (draw_rect_needs_clip) {
        context.display_list_recorder().save();
        context.display_list_recorder().add_clip_rect(image_rect);
    }

    decoded_image_data->paint(context, draw_rect, layout_box().image_rendering(), layout_box().color_scheme());

    if (draw_rect_needs_clip)
        context.display_list_recorder().restore();
}

}
