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

StyleValueFFI::StyleValueData* ImageSetStyleValue::make_image_set_data(Vector<Option> const& options)
{
    // The Rust allocation takes ownership of one strong reference to each value and one leaked
    // reference to each type string.
    Vector<StyleValueFFI::RetainedImageSetOption> ffi_options;
    ffi_options.ensure_capacity(options.size());
    for (auto const& option : options) {
        option.image->ref();
        option.resolution->ref();
        ffi_options.unchecked_append({
            { option.image.ptr() },
            { option.resolution.ptr() },
            option.type.has_value(),
            { option.type.has_value() ? option.type->to_raw_leaked() : 0 },
        });
    }
    return StyleValueFFI::rust_style_value_create_image_set(ffi_options.data(), ffi_options.size());
}

ValueComparingNonnullRefPtr<ImageSetStyleValue const> ImageSetStyleValue::create(Vector<Option> options)
{
    return adopt_ref(*new (nothrow) ImageSetStyleValue(move(options)));
}

ImageSetStyleValue::ImageSetStyleValue(Vector<Option> options)
    : AbstractImageStyleValue(Type::ImageSet, make_image_set_data(options))
{
}

Optional<ImageSetStyleValue::Option> ImageSetStyleValue::select_option(double device_pixels_per_css_pixel) const
{
    Optional<Option> best_below_or_equal;
    Optional<double> best_below_or_equal_resolution;
    Optional<Option> best_above;
    Optional<double> best_above_resolution;

    for (auto const& option : options()) {
        if (option.type.has_value() && !HTML::is_supported_image_type(*option.type))
            continue;

        auto resolution = Resolution::from_style_value(option.resolution).to_dots_per_pixel();

        if (resolution >= device_pixels_per_css_pixel) {
            if (!best_above_resolution.has_value() || resolution < *best_above_resolution) {
                best_above = option;
                best_above_resolution = resolution;
            }
            continue;
        }

        if (!best_below_or_equal_resolution.has_value() || resolution > *best_below_or_equal_resolution) {
            best_below_or_equal = option;
            best_below_or_equal_resolution = resolution;
        }
    }

    if (best_above.has_value())
        return best_above;
    return best_below_or_equal;
}

void ImageSetStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("image-set("sv);
    auto options = this->options();
    for (size_t i = 0; i < options.size(); ++i) {
        if (i > 0)
            builder.append(", "sv);
        auto const& option = options[i];
        option.image->serialize(builder, mode);
        builder.append(' ');
        option.resolution->serialize(builder, mode);
        if (option.type.has_value()) {
            builder.append(" type(\""sv);
            builder.append_escaped_for_json(MUST(option.type->utf16_view().to_utf8()));
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
    auto options = this->options();
    auto other_options = other_image_set.options();
    if (options.size() != other_options.size())
        return false;

    for (size_t i = 0; i < options.size(); ++i) {
        auto const& option = options[i];
        auto const& other_option = other_options[i];
        if (!option.image->equals(*other_option.image))
            return false;
        if (!option.resolution->equals(*other_option.resolution))
            return false;
        if (option.type != other_option.type)
            return false;
    }
    return true;
}

void ImageSetStyleValue::load_any_resources(DOM::Document& document)
{
    auto dpr = document.page().client().device_pixels_per_css_pixel();
    if (auto option = select_option(dpr); option.has_value()) {
        m_selected_image = option->image.ptr();
        m_selected_resolution = Resolution::from_style_value(option->resolution).to_dots_per_pixel();
    }
    if (m_selected_image)
        const_cast<AbstractImageStyleValue&>(*m_selected_image).load_any_resources(document);
}

Optional<CSSPixels> ImageSetStyleValue::natural_width(DOM::Document const& document) const
{
    if (!m_selected_image)
        return {};
    auto natural_width = m_selected_image->natural_width(document);
    if (!natural_width.has_value())
        return {};
    return CSSPixels { natural_width->to_double() / m_selected_resolution };
}

Optional<CSSPixels> ImageSetStyleValue::natural_height(DOM::Document const& document) const
{
    if (!m_selected_image)
        return {};
    auto natural_height = m_selected_image->natural_height(document);
    if (!natural_height.has_value())
        return {};
    return CSSPixels { natural_height->to_double() / m_selected_resolution };
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

    // Propagate the style sheet to candidate images whose type() filter does not exclude them. This ensures the
    // candidate images register themselves as pending image resources on the style sheet, so their fetches start when
    // the style sheet is associated with the document, properly delaying the document's load event.
    for (auto const& option : options()) {
        if (option.type.has_value() && !HTML::is_supported_image_type(*option.type))
            continue;
        const_cast<AbstractImageStyleValue&>(*option.image).set_style_sheet(style_sheet);
    }
}

ValueComparingNonnullRefPtr<StyleValue const> ImageSetStyleValue::absolutized(ComputationContext const& context) const
{
    auto existing_options = this->options();
    Vector<Option> options;
    options.ensure_capacity(existing_options.size());
    for (auto const& option : existing_options) {
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
