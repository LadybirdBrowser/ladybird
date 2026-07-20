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

NonnullRefPtr<SVGImagePaintable> SVGImagePaintable::create(Layout::SVGImageBox const& layout_box)
{
    return adopt_ref(*new SVGImagePaintable(layout_box));
}

SVGImagePaintable::SVGImagePaintable(Layout::SVGImageBox const& layout_box)
    : SVGGraphicsPaintable(layout_box)
{
}

Layout::SVGImageBox const& SVGImagePaintable::layout_box() const
{
    return static_cast<Layout::SVGImageBox const&>(layout_node());
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

    auto decoded_image_data = layout_box().dom_node().decoded_image_data();
    if (!decoded_image_data)
        return;

    auto image_rect = absolute_rect();
    auto intrinsic_size = layout_box().dom_node().intrinsic_size().value_or(image_rect.size());
    // FIXME: Respect the preserveAspectRatio attribute instead of assuming its default value.
    auto draw_rect = get_replaced_box_painting_area(*this, context, CSS::ObjectFit::Contain, intrinsic_size);
    if (draw_rect.is_empty())
        return;

    // https://svgwg.org/svg2-draft/embedded.html#ImageElement
    // The user agent stylesheet sets the value of the overflow property on ‘image’ element to hidden. Unless
    // over-ridden by the author, images will therefore be clipped to the positioning rectangle defined by the
    // geometry properties.
    auto positioning_rectangle = context.rounded_device_rect(image_rect).to_type<int>();
    bool overflow_is_visible = computed_values().overflow_x() == CSS::Overflow::Visible
        && computed_values().overflow_y() == CSS::Overflow::Visible;
    bool draw_rect_needs_clip = !overflow_is_visible && !positioning_rectangle.contains(draw_rect);
    if (draw_rect_needs_clip) {
        context.display_list_recorder().save();
        context.display_list_recorder().add_clip_rect(positioning_rectangle);
    }

    decoded_image_data->paint(context, draw_rect, computed_values().image_rendering());

    if (draw_rect_needs_clip)
        context.display_list_recorder().restore();
}

}
