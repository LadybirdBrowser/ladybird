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

static Optional<double> option_resolution_in_dppx(ImageSetStyleValue::Option const& option, Optional<CalculationResolutionContext> const& calculation_resolution_context)
{
    if (option.resolution->is_resolution())
        return option.resolution->as_resolution().resolution().to_dots_per_pixel();
    if (option.resolution->is_calculated()) {
        auto resolution = option.resolution->as_calculated().resolve_resolution(calculation_resolution_context.value_or({}));
        if (resolution.has_value())
            return resolution->to_dots_per_pixel();
    }
    return {};
}

AbstractImageStyleValue const* ImageSetStyleValue::select_image(double device_pixels_per_css_pixel, Optional<CalculationResolutionContext> const& calculation_resolution_context) const
{
    ImageSetStyleValue::Option const* best_below_or_equal = nullptr;
    Optional<double> best_below_or_equal_resolution;
    ImageSetStyleValue::Option const* best_above = nullptr;
    Optional<double> best_above_resolution;

    for (auto const& option : m_options) {
        if (option.type.has_value() && !HTML::is_supported_image_type(*option.type))
            continue;

        auto resolution = option_resolution_in_dppx(option, calculation_resolution_context);
        if (!resolution.has_value())
            continue;

        if (*resolution >= device_pixels_per_css_pixel) {
            if (!best_above_resolution.has_value() || *resolution < *best_above_resolution) {
                best_above = &option;
                best_above_resolution = *resolution;
            }
            continue;
        }

        if (!best_below_or_equal_resolution.has_value() || *resolution > *best_below_or_equal_resolution) {
            best_below_or_equal = &option;
            best_below_or_equal_resolution = *resolution;
        }
    }

    if (best_above)
        return best_above->image.ptr();
    if (best_below_or_equal)
        return best_below_or_equal->image.ptr();
    return nullptr;
}

void ImageSetStyleValue::visit_edges(JS::Cell::Visitor& visitor) const
{
    Base::visit_edges(visitor);
    visitor.visit(m_style_sheet);
    for (auto const& option : m_options)
        option.image->visit_edges(visitor);
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
    if (auto const* image = select_image(dpr, {}); image && image != m_selected_image) {
        const_cast<AbstractImageStyleValue&>(*image).set_style_sheet(m_style_sheet);
        m_selected_image = image;
    }
    if (m_selected_image)
        const_cast<AbstractImageStyleValue&>(*m_selected_image).load_any_resources(document);
}

void ImageSetStyleValue::load_any_resources(Layout::NodeWithStyle const& layout_node)
{
    update_selected_image_for_layout_node(layout_node);
    if (m_selected_image)
        const_cast<AbstractImageStyleValue&>(*m_selected_image).load_any_resources(const_cast<DOM::Document&>(layout_node.document()));
}

void ImageSetStyleValue::update_selected_image_for_layout_node(Layout::NodeWithStyle const& layout_node) const
{
    Optional<DOM::AbstractElement> abstract_element;
    if (layout_node.is_generated_for_pseudo_element()) {
        if (auto const* pseudo_element_generator = layout_node.pseudo_element_generator())
            abstract_element = DOM::AbstractElement { *pseudo_element_generator, layout_node.generated_for_pseudo_element() };
    } else if (auto const* dom_node = layout_node.dom_node(); dom_node && dom_node->is_element()) {
        abstract_element = DOM::AbstractElement { static_cast<DOM::Element const&>(*dom_node) };
    }

    auto context = CalculationResolutionContext {
        .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node),
        .abstract_element = abstract_element,
    };
    auto dpr = layout_node.document().page().client().device_pixels_per_css_pixel();

    if (auto const* image = select_image(dpr, context); image && image != m_selected_image) {
        const_cast<AbstractImageStyleValue&>(*image).set_style_sheet(m_style_sheet);
        m_selected_image = image;
    }
}

Optional<CSSPixels> ImageSetStyleValue::natural_width() const
{
    if (m_selected_image)
        return m_selected_image->natural_width();
    return {};
}

Optional<CSSPixels> ImageSetStyleValue::natural_height() const
{
    if (m_selected_image)
        return m_selected_image->natural_height();
    return {};
}

Optional<CSSPixelFraction> ImageSetStyleValue::natural_aspect_ratio() const
{
    if (m_selected_image)
        return m_selected_image->natural_aspect_ratio();
    return {};
}

void ImageSetStyleValue::resolve_for_size(Layout::NodeWithStyle const& layout_node, CSSPixelSize size) const
{
    update_selected_image_for_layout_node(layout_node);
    if (m_selected_image)
        m_selected_image->resolve_for_size(layout_node, size);
}

bool ImageSetStyleValue::is_paintable() const
{
    if (m_selected_image)
        return m_selected_image->is_paintable();
    return false;
}

void ImageSetStyleValue::paint(DisplayListRecordingContext& context, DevicePixelRect const& dest_rect, ImageRendering image_rendering) const
{
    if (m_selected_image)
        m_selected_image->paint(context, dest_rect, image_rendering);
}

Optional<Gfx::Color> ImageSetStyleValue::color_if_single_pixel_bitmap() const
{
    if (m_selected_image)
        return m_selected_image->color_if_single_pixel_bitmap();
    return {};
}

void ImageSetStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);
    m_style_sheet = style_sheet;
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
