/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/SupportedImageTypes.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<ImageSetStyleValue const> ImageSetStyleValue::create(Vector<Option> options)
{
    return adopt_ref(*new (nothrow) ImageSetStyleValue(move(options)));
}

ImageSetStyleValue::ImageSetStyleValue(Vector<Option> options)
    : AbstractImageStyleValue(Type::ImageSet)
    , m_options(move(options))
{
}

AbstractImageStyleValue const* ImageSetStyleValue::select_image(double device_pixels_per_css_pixel) const
{
    ImageSetStyleValue::Option const* best_below_or_equal = nullptr;
    Optional<double> best_below_or_equal_resolution;
    ImageSetStyleValue::Option const* best_above = nullptr;
    Optional<double> best_above_resolution;

    for (auto const& option : m_options) {
        if (option.type.has_value() && !HTML::is_supported_image_type(*option.type))
            continue;

        auto resolution = Resolution::from_style_value(option.resolution).to_dots_per_pixel();

        if (resolution >= device_pixels_per_css_pixel) {
            if (!best_above_resolution.has_value() || resolution < *best_above_resolution) {
                best_above = &option;
                best_above_resolution = resolution;
            }
            continue;
        }

        if (!best_below_or_equal_resolution.has_value() || resolution > *best_below_or_equal_resolution) {
            best_below_or_equal = &option;
            best_below_or_equal_resolution = resolution;
        }
    }

    if (best_above)
        return best_above->image.ptr();
    if (best_below_or_equal)
        return best_below_or_equal->image.ptr();
    return nullptr;
}

void ImageSetStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("image-set("sv);
    for (size_t i = 0; i < m_options.size(); ++i) {
        if (i > 0)
            builder.append(", "sv);
        auto const& option = m_options[i];
        option.image->serialize(builder, mode);
        builder.append(' ');
        option.resolution->serialize(builder, mode);
        if (option.type.has_value()) {
            builder.append(" type(\""sv);
            builder.append_escaped_for_json(*option.type);
            builder.append("\")"sv);
        }
    }
    builder.append(')');
}

bool ImageSetStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_image_set = other.as_image_set();
    if (m_options.size() != other_image_set.m_options.size())
        return false;

    for (size_t i = 0; i < m_options.size(); ++i) {
        auto const& option = m_options[i];
        auto const& other_option = other_image_set.m_options[i];
        if (!option.image->equals(*other_option.image))
            return false;
        if (!option.resolution->equals(*other_option.resolution))
            return false;
        if (option.type != other_option.type)
            return false;
    }
    return true;
}

bool ImageSetStyleValue::is_computationally_independent() const
{
    for (auto const& option : m_options) {
        if (!option.image->is_computationally_independent())
            return false;
        if (!option.resolution->is_computationally_independent())
            return false;
    }
    return true;
}

void ImageSetStyleValue::load_any_resources(DOM::Document& document)
{
    auto dpr = document.page().client().device_pixels_per_css_pixel();
    if (auto const* image = select_image(dpr); image && image != m_selected_image)
        m_selected_image = image;
    if (m_selected_image)
        const_cast<AbstractImageStyleValue&>(*m_selected_image).load_any_resources(document);
}

Optional<CSSPixels> ImageSetStyleValue::natural_width(DOM::Document const& document) const
{
    if (m_selected_image)
        return m_selected_image->natural_width(document);
    return {};
}

Optional<CSSPixels> ImageSetStyleValue::natural_height(DOM::Document const& document) const
{
    if (m_selected_image)
        return m_selected_image->natural_height(document);
    return {};
}

Optional<CSSPixelFraction> ImageSetStyleValue::natural_aspect_ratio(DOM::Document const& document) const
{
    if (m_selected_image)
        return m_selected_image->natural_aspect_ratio(document);
    return {};
}

void ImageSetStyleValue::resolve_for_size(Layout::NodeWithStyle const& layout_node, CSSPixelSize size) const
{
    if (m_selected_image)
        m_selected_image->resolve_for_size(layout_node, size);
}

bool ImageSetStyleValue::is_paintable(DOM::Document const& document) const
{
    if (m_selected_image)
        return m_selected_image->is_paintable(document);
    return false;
}

void ImageSetStyleValue::paint(DisplayListRecordingContext& context, DOM::Document const& document, DevicePixelRect const& dest_rect, ImageRendering image_rendering) const
{
    if (m_selected_image)
        m_selected_image->paint(context, document, dest_rect, image_rendering);
}

Optional<Gfx::Color> ImageSetStyleValue::color_if_single_pixel_bitmap(DOM::Document const& document) const
{
    if (m_selected_image)
        return m_selected_image->color_if_single_pixel_bitmap(document);
    return {};
}

void ImageSetStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);

    // Propagate the style sheet to candidate images whose type() filter does not exclude them. This ensures the
    // candidate images register themselves as pending image resources on the style sheet, so their fetches start when
    // the style sheet is associated with the document, properly delaying the document's load event.
    for (auto const& option : m_options) {
        if (option.type.has_value() && !HTML::is_supported_image_type(*option.type))
            continue;
        const_cast<AbstractImageStyleValue&>(*option.image).set_style_sheet(style_sheet);
    }
}

ValueComparingNonnullRefPtr<StyleValue const> ImageSetStyleValue::absolutized(ComputationContext const& context) const
{
    Vector<Option> options;
    options.ensure_capacity(m_options.size());
    for (auto const& option : m_options) {
        auto image = option.image->absolutized(context);
        VERIFY(image->is_abstract_image());
        options.unchecked_append({
            .image = image->as_abstract_image(),
            .resolution = option.resolution->absolutized(context),
            .type = option.type,
        });
    }
    return ImageSetStyleValue::create(move(options));
}

}
