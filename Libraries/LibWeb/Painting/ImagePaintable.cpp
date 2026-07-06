/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ImagePaintable.h>
#include <LibWeb/Painting/ReplacedElementCommon.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Painting {

static void paint_alt_text(DisplayListRecordingContext& context, Layout::Node const& layout_node, Gfx::IntRect const& content_rect, String const& alt_text, Color color)
{
    auto const& font = layout_node.font(context);
    auto const metrics = font.pixel_metrics();
    auto line_height = metrics.ascent + metrics.descent;
    if (line_height <= 0)
        return;

    float baseline_y = content_rect.y() + metrics.ascent;
    Utf16String line;
    auto draw_line = [&] {
        if (line.is_empty())
            return;
        auto glyph_run = Gfx::shape_text({}, 0, line.utf16_view(), font, Gfx::GlyphRun::TextType::Ltr);
        context.display_list_recorder().draw_glyph_run({ static_cast<float>(content_rect.x()), baseline_y }, *glyph_run, color, content_rect, 1.0, Gfx::Orientation::Horizontal);
        baseline_y += line_height;
        line = {};
    };

    Utf16String::from_utf8(alt_text).for_each_split_view(' ', SplitBehavior::Nothing, [&](Utf16View const& word) {
        Utf16StringBuilder builder;
        builder.append(line);
        if (!line.is_empty())
            builder.append_ascii(' ');
        builder.append(word);

        auto candidate_line = builder.to_string();
        if (line.is_empty() || font.width(candidate_line) <= content_rect.width()) {
            line = move(candidate_line);
            return IterationDecision::Continue;
        }

        draw_line();
        builder.clear();
        builder.append(word);
        line = builder.to_string();
        return IterationDecision::Continue;
    });

    draw_line();
}

NonnullRefPtr<ImagePaintable> ImagePaintable::create(Layout::SVGImageBox const& layout_box)
{
    return adopt_ref(*new ImagePaintable(layout_box, layout_box.dom_node(), false, String {}, true));
}

NonnullRefPtr<ImagePaintable> ImagePaintable::create(Layout::ImageBox const& layout_box)
{
    String alt;
    if (auto element = layout_box.dom_node())
        alt = element->get_attribute_value(HTML::AttributeNames::alt);
    return adopt_ref(*new ImagePaintable(layout_box, layout_box.image_provider(), layout_box.renders_as_alt_text(), move(alt), false));
}

ImagePaintable::ImagePaintable(Layout::Box const& layout_box, Layout::ImageProvider const& image_provider, bool renders_as_alt_text, String alt_text, bool is_svg_image)
    : Paintable(layout_box)
    , m_renders_as_alt_text(renders_as_alt_text)
    , m_alt_text(move(alt_text))
    , m_image_provider(image_provider)
    , m_is_svg_image(is_svg_image)
{
}

void ImagePaintable::reset_for_relayout()
{
    Paintable::reset_for_relayout();

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

    Paintable::paint(context, phase);

    if (phase == PaintPhase::Foreground) {
        auto image_rect = absolute_rect();
        auto image_rect_device_pixels = context.rounded_device_rect(image_rect);
        auto renders_as_alt_text = m_is_svg_image ? m_renders_as_alt_text : !m_image_provider.is_image_available();
        if (renders_as_alt_text) {
            if (!m_alt_text.is_empty()) {
                auto enclosing_rect = context.enclosing_device_rect(image_rect).to_type<int>();
                context.display_list_recorder().save();
                context.display_list_recorder().add_clip_rect(enclosing_rect);
                paint_alt_text(context, layout_node(), enclosing_rect, m_alt_text, computed_values().color());
                context.display_list_recorder().restore();
            }
        } else if (auto decoded_image_data = m_image_provider.decoded_image_data()) {
            ScopedCornerRadiusClip corner_clip { context, image_rect_device_pixels, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };
            auto image_int_rect_device_pixels = image_rect_device_pixels.to_type<int>();

            // https://drafts.csswg.org/css-images/#the-object-fit
            auto object_fit = m_is_svg_image ? CSS::ObjectFit::Contain : computed_values().object_fit();

            auto intrinsic_size = m_image_provider.intrinsic_size()
                                      .map([](auto size) { return size.template to_type<int>(); })
                                      .value_or(image_int_rect_device_pixels.size());

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
}

}
