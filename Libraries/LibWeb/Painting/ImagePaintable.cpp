/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(ImagePaintable);

GC::Ref<ImagePaintable> ImagePaintable::create(Layout::SVGImageBox const& layout_box)
{
    return layout_box.heap().allocate<ImagePaintable>(layout_box, layout_box.dom_node(), false, String {}, true);
}

GC::Ref<ImagePaintable> ImagePaintable::create(Layout::ImageBox const& layout_box)
{
    String alt;
    if (auto element = layout_box.dom_node())
        alt = element->get_attribute_value(HTML::AttributeNames::alt);
    return layout_box.heap().allocate<ImagePaintable>(layout_box, layout_box.image_provider(), layout_box.renders_as_alt_text(), move(alt), false);
}

ImagePaintable::ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider, bool renders_as_alt_text, String alt_text, bool is_svg_image)
    : PaintableBox(layout_box)
    , m_renders_as_alt_text(renders_as_alt_text)
    , m_alt_text(move(alt_text))
    , m_image_provider(image_provider)
    , m_is_svg_image(is_svg_image)
{
}

void ImagePaintable::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_image_provider.image_provider_visit_edges(visitor);
}

void ImagePaintable::reset_for_relayout()
{
    PaintableBox::reset_for_relayout();

    if (!m_is_svg_image) {
        m_renders_as_alt_text = !m_image_provider.is_image_available();
        if (auto const* image_box = as_if<Layout::ImageBox>(layout_node())) {
            if (auto element = image_box->dom_node())
                m_alt_text = element->get_attribute_value(HTML::AttributeNames::alt);
        }
    }
}

void ImagePaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto image_rect = absolute_rect();
        auto image_rect_device_pixels = context.rounded_device_rect(image_rect);
        if (m_renders_as_alt_text) {
            if (!m_alt_text.is_empty()) {
                auto enclosing_rect = context.enclosing_device_rect(image_rect).to_type<int>();
                context.display_list_recorder().draw_text(enclosing_rect, Utf16String::from_utf8(m_alt_text), layout_node().font(context), Gfx::TextAlignment::Center, computed_values().color());
            }
        } else if (auto decoded_image_data = m_image_provider.decoded_image_data()) {
            ScopedCornerRadiusClip corner_clip { context, image_rect_device_pixels, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };
            auto image_int_rect_device_pixels = image_rect_device_pixels.to_type<int>();
            auto bitmap_rect = decoded_image_data->frame_rect(m_image_provider.current_frame_index()).value_or(image_int_rect_device_pixels);
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), bitmap_rect.size(), image_int_rect_device_pixels.size());

            // https://drafts.csswg.org/css-images/#the-object-fit
            auto object_fit = m_is_svg_image ? CSS::ObjectFit::Contain : computed_values().object_fit();
            auto draw_rect = get_replaced_box_painting_area(*this, context, object_fit, bitmap_rect.size());
            if (!draw_rect.is_empty())
                decoded_image_data->paint(context, m_image_provider.current_frame_index(), draw_rect, image_int_rect_device_pixels, scaling_mode);
        }

        if (selection_state() != SelectionState::None) {
            auto selection_background_color = selection_style().background_color;
            if (selection_background_color.alpha() > 0)
                context.display_list_recorder().fill_rect(image_rect_device_pixels.to_type<int>(), selection_background_color);
        }
    }
}

}
