/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ImagePaintable.h>
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
    const_cast<DOM::Document&>(layout_box.document()).register_viewport_client(*this);
}

void ImagePaintable::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_image_provider.image_provider_visit_edges(visitor);
}

void ImagePaintable::finalize()
{
    Base::finalize();

    // NOTE: We unregister from the document in finalize() to avoid trouble
    //       in the scenario where our Document has already been swept by GC.
    document().unregister_viewport_client(*this);
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
                context.display_list_recorder().draw_rect(enclosing_rect, Gfx::Color::Black);
                context.display_list_recorder().draw_text(enclosing_rect, Utf16String::from_utf8(m_alt_text), *Platform::FontPlugin::the().default_font(12), Gfx::TextAlignment::Center, computed_values().color());
            }
        } else if (auto decoded_image_data = m_image_provider.decoded_image_data()) {
            ScopedCornerRadiusClip corner_clip { context, image_rect_device_pixels, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };
            auto image_int_rect_device_pixels = image_rect_device_pixels.to_type<int>();
            auto bitmap_rect = decoded_image_data->frame_rect(m_image_provider.current_frame_index()).value_or(image_int_rect_device_pixels);
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), bitmap_rect.size(), image_int_rect_device_pixels.size());
            auto bitmap_aspect_ratio = (float)bitmap_rect.height() / bitmap_rect.width();
            auto image_aspect_ratio = (float)image_rect.height() / (float)image_rect.width();

            auto scale_x = 0.0f;
            auto scale_y = 0.0f;

            // https://drafts.csswg.org/css-images/#the-object-fit
            auto object_fit = m_is_svg_image ? CSS::ObjectFit::Contain : computed_values().object_fit();
            if (object_fit == CSS::ObjectFit::ScaleDown) {
                if (bitmap_rect.width() > image_rect.width() || bitmap_rect.height() > image_rect.height()) {
                    object_fit = CSS::ObjectFit::Contain;
                } else {
                    object_fit = CSS::ObjectFit::None;
                }
            }

            switch (object_fit) {
            case CSS::ObjectFit::Fill:
                scale_x = (float)image_rect.width() / bitmap_rect.width();
                scale_y = (float)image_rect.height() / bitmap_rect.height();
                break;
            case CSS::ObjectFit::Contain:
                if (bitmap_aspect_ratio >= image_aspect_ratio) {
                    scale_x = (float)image_rect.height() / bitmap_rect.height();
                    scale_y = scale_x;
                } else {
                    scale_x = (float)image_rect.width() / bitmap_rect.width();
                    scale_y = scale_x;
                }
                break;
            case CSS::ObjectFit::Cover:
                if (bitmap_aspect_ratio >= image_aspect_ratio) {
                    scale_x = (float)image_rect.width() / bitmap_rect.width();
                    scale_y = scale_x;
                } else {
                    scale_x = (float)image_rect.height() / bitmap_rect.height();
                    scale_y = scale_x;
                }
                break;
            case CSS::ObjectFit::ScaleDown:
                VERIFY_NOT_REACHED(); // handled outside the switch-case
            case CSS::ObjectFit::None:
                scale_x = 1;
                scale_y = 1;
            }

            auto scaled_bitmap_width = CSSPixels::nearest_value_for(bitmap_rect.width() * scale_x);
            auto scaled_bitmap_height = CSSPixels::nearest_value_for(bitmap_rect.height() * scale_y);

            auto residual_horizontal = image_rect.width() - scaled_bitmap_width;
            auto residual_vertical = image_rect.height() - scaled_bitmap_height;

            // https://drafts.csswg.org/css-images/#the-object-position
            auto const& object_position = computed_values().object_position();

            auto offset_x = CSSPixels::from_raw(0);
            if (object_position.edge_x == CSS::PositionEdge::Left) {
                offset_x = object_position.offset_x.to_px(layout_node(), residual_horizontal);
            } else if (object_position.edge_x == CSS::PositionEdge::Right) {
                offset_x = residual_horizontal - object_position.offset_x.to_px(layout_node(), residual_horizontal);
            }

            auto offset_y = CSSPixels::from_raw(0);
            if (object_position.edge_y == CSS::PositionEdge::Top) {
                offset_y = object_position.offset_y.to_px(layout_node(), residual_vertical);
            } else if (object_position.edge_y == CSS::PositionEdge::Bottom) {
                offset_y = residual_vertical - object_position.offset_y.to_px(layout_node(), residual_vertical);
            }

            Gfx::IntRect draw_rect = {
                image_int_rect_device_pixels.x() + context.rounded_device_pixels(offset_x).value(),
                image_int_rect_device_pixels.y() + context.rounded_device_pixels(offset_y).value(),
                context.rounded_device_pixels(scaled_bitmap_width).value(),
                context.rounded_device_pixels(scaled_bitmap_height).value()
            };

            decoded_image_data->paint(context, m_image_provider.current_frame_index(), draw_rect, image_rect_device_pixels.to_type<int>(), scaling_mode);
        }

        if (selection_state() != SelectionState::None) {
            auto selection_background_color = selection_style().background_color;
            if (selection_background_color.alpha() > 0)
                context.display_list_recorder().fill_rect(image_rect_device_pixels.to_type<int>(), selection_background_color);
        }
    }
}

void ImagePaintable::did_set_viewport_rect(CSSPixelRect const& viewport_rect)
{
    const_cast<Layout::ImageProvider&>(m_image_provider).set_visible_in_viewport(viewport_rect.intersects(absolute_rect()));
}

}
