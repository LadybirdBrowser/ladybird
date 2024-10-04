/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(ImagePaintable);

JS::NonnullGCPtr<ImagePaintable> ImagePaintable::create(Layout::SVGImageBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<ImagePaintable>(layout_box, layout_box.dom_node(), false, String {}, true);
}

JS::NonnullGCPtr<ImagePaintable> ImagePaintable::create(Layout::ImageBox const& layout_box)
{
    auto alt = layout_box.dom_node().get_attribute_value(HTML::AttributeNames::alt);
    return layout_box.heap().allocate_without_realm<ImagePaintable>(layout_box, layout_box.image_provider(), layout_box.renders_as_alt_text(), move(alt), false);
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
    visitor.visit(m_image_provider.to_html_element());
}

void ImagePaintable::finalize()
{
    Base::finalize();

    // NOTE: We unregister from the document in finalize() to avoid trouble
    //       in the scenario where our Document has already been swept by GC.
    document().unregister_viewport_client(*this);
}

void ImagePaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto image_rect = context.rounded_device_rect(absolute_rect());
        if (m_renders_as_alt_text) {
            auto enclosing_rect = context.enclosing_device_rect(absolute_rect()).to_type<int>();
            context.display_list_recorder().draw_rect(enclosing_rect, Gfx::Color::Black);
            context.display_list_recorder().draw_text(enclosing_rect, m_alt_text, Platform::FontPlugin::the().default_font(), Gfx::TextAlignment::Center, computed_values().color());
        } else if (auto bitmap = m_image_provider.current_image_bitmap(image_rect.size().to_type<int>())) {
            ScopedCornerRadiusClip corner_clip { context, image_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };
            auto image_int_rect = image_rect.to_type<int>();
            auto bitmap_rect = bitmap->rect();
            auto scaling_mode = to_gfx_scaling_mode(computed_values().image_rendering(), bitmap_rect, image_int_rect);
            auto bitmap_aspect_ratio = (float)bitmap_rect.height() / bitmap_rect.width();
            auto image_aspect_ratio = (float)image_rect.height().value() / image_rect.width().value();

            auto scale_x = 0.0f;
            auto scale_y = 0.0f;
            Gfx::IntRect bitmap_intersect = bitmap_rect;

            auto object_fit = m_is_svg_image ? CSS::ObjectFit::Contain : computed_values().object_fit();
            switch (object_fit) {
            case CSS::ObjectFit::Fill:
                scale_x = (float)image_int_rect.width() / bitmap_rect.width();
                scale_y = (float)image_int_rect.height() / bitmap_rect.height();
                break;
            case CSS::ObjectFit::Contain:
                if (bitmap_aspect_ratio >= image_aspect_ratio) {
                    scale_x = (float)image_int_rect.height() / bitmap_rect.height();
                    scale_y = scale_x;
                } else {
                    scale_x = (float)image_int_rect.width() / bitmap_rect.width();
                    scale_y = scale_x;
                }
                break;
            case CSS::ObjectFit::Cover:
                if (bitmap_aspect_ratio >= image_aspect_ratio) {
                    scale_x = (float)image_int_rect.width() / bitmap_rect.width();
                    scale_y = scale_x;
                    bitmap_intersect.set_height(bitmap_rect.width() * image_aspect_ratio);
                } else {
                    scale_x = (float)image_int_rect.height() / bitmap_rect.height();
                    scale_y = scale_x;
                    bitmap_intersect.set_width(bitmap_rect.height() / image_aspect_ratio);
                }
                break;
            case CSS::ObjectFit::ScaleDown:
                // FIXME: Implement
            case CSS::ObjectFit::None:
                scale_x = 1;
                scale_y = 1;
                bitmap_intersect.set_size(image_int_rect.size());
            }

            auto scaled_bitmap_width = bitmap_rect.width() * scale_x;
            auto scaled_bitmap_height = bitmap_rect.height() * scale_y;

            auto residual_horizontal = CSSPixels::nearest_value_for(image_int_rect.width() - scaled_bitmap_width);
            auto residual_vertical = CSSPixels::nearest_value_for(image_int_rect.height() - scaled_bitmap_height);

            bitmap_intersect.set_x((bitmap_rect.width() - bitmap_intersect.width()) / 2);
            bitmap_intersect.set_y((bitmap_rect.height() - bitmap_intersect.height()) / 2);

            auto const& object_position = computed_values().object_position();

            auto offset_x = 0;
            if (object_position.edge_x == CSS::PositionEdge::Left) {
                offset_x = object_position.offset_x.to_px(layout_node(), residual_horizontal).to_int();
                bitmap_intersect.set_x(0);
            } else if (object_position.edge_x == CSS::PositionEdge::Right) {
                offset_x = residual_horizontal.to_int() - object_position.offset_x.to_px(layout_node(), residual_horizontal).to_int();
            }
            if (image_int_rect.width() < scaled_bitmap_width)
                bitmap_intersect.set_x(-(offset_x / scale_x));

            auto offset_y = 0;
            if (object_position.edge_y == CSS::PositionEdge::Top) {
                offset_y = object_position.offset_y.to_px(layout_node(), residual_vertical).to_int();
                bitmap_intersect.set_y(0);
            } else if (object_position.edge_y == CSS::PositionEdge::Bottom) {
                offset_y = residual_vertical.to_int() - object_position.offset_y.to_px(layout_node(), residual_vertical).to_int();
            }
            if (image_int_rect.height() < scaled_bitmap_height)
                bitmap_intersect.set_y(-(offset_y / scale_y));

            Gfx::IntRect draw_rect = {
                image_int_rect.x() + offset_x,
                image_int_rect.y() + offset_y,
                (int)scaled_bitmap_width,
                (int)scaled_bitmap_height
            };

            context.display_list_recorder().draw_scaled_immutable_bitmap(draw_rect.intersected(image_int_rect), *bitmap, bitmap_rect.intersected(bitmap_intersect), scaling_mode);
        }
    }
}

void ImagePaintable::did_set_viewport_rect(CSSPixelRect const& viewport_rect)
{
    const_cast<Layout::ImageProvider&>(m_image_provider).set_visible_in_viewport(viewport_rect.intersects(absolute_rect()));
}

}
