/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Variant.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Rect.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

Length::FontMetrics::FontMetrics(CSSPixels font_size, Gfx::FontPixelMetrics const& pixel_metrics, CSSPixels line_height)
    : font_size(font_size)
    , x_height(pixel_metrics.x_height)
    // FIXME: This is only approximately the cap height. The spec suggests measuring the "O" glyph:
    //        https://www.w3.org/TR/css-values-4/#cap
    , cap_height(pixel_metrics.ascent)
    , zero_advance(pixel_metrics.advance_of_ascii_zero)
    , line_height(line_height)
{
}

Length::Length(double value, LengthUnit unit)
    : m_unit(unit)
    , m_value(value)
{
}
Length::~Length() = default;

Length Length::make_px(double value)
{
    return Length(value, LengthUnit::Px);
}

Length Length::make_px(CSSPixels value)
{
    return make_px(value.to_double());
}

Length Length::percentage_of(Percentage const& percentage) const
{
    return Length { percentage.as_fraction() * raw_value(), m_unit };
}

CSSPixels Length::font_relative_length_to_px(Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    switch (m_unit) {
    case LengthUnit::Em:
        return CSSPixels::nearest_value_for(m_value * font_metrics.font_size.to_double());
    case LengthUnit::Rem:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.font_size.to_double());
    case LengthUnit::Ex:
        return CSSPixels::nearest_value_for(m_value * font_metrics.x_height.to_double());
    case LengthUnit::Rex:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.x_height.to_double());
    case LengthUnit::Cap:
        return CSSPixels::nearest_value_for(m_value * font_metrics.cap_height.to_double());
    case LengthUnit::Rcap:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.cap_height.to_double());
    case LengthUnit::Ch:
        return CSSPixels::nearest_value_for(m_value * font_metrics.zero_advance.to_double());
    case LengthUnit::Rch:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.zero_advance.to_double());
    case LengthUnit::Ic:
        // FIXME: Use the "advance measure of the “水” (CJK water ideograph, U+6C34) glyph"
        return CSSPixels::nearest_value_for(m_value * font_metrics.font_size.to_double());
    case LengthUnit::Ric:
        // FIXME: Use the "advance measure of the “水” (CJK water ideograph, U+6C34) glyph"
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.font_size.to_double());
    case LengthUnit::Lh:
        return CSSPixels::nearest_value_for(m_value * font_metrics.line_height.to_double());
    case LengthUnit::Rlh:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.line_height.to_double());
    default:
        VERIFY_NOT_REACHED();
    }
}

CSSPixels Length::viewport_relative_length_to_px(CSSPixelRect const& viewport_rect) const
{
    switch (m_unit) {
    case LengthUnit::Vw:
    case LengthUnit::Svw:
    case LengthUnit::Lvw:
    case LengthUnit::Dvw:
        return viewport_rect.width() * (CSSPixels::nearest_value_for(m_value) / 100);
    case LengthUnit::Vh:
    case LengthUnit::Svh:
    case LengthUnit::Lvh:
    case LengthUnit::Dvh:
        return viewport_rect.height() * (CSSPixels::nearest_value_for(m_value) / 100);
    case LengthUnit::Vi:
    case LengthUnit::Svi:
    case LengthUnit::Lvi:
    case LengthUnit::Dvi:
        // FIXME: Select the width or height based on which is the inline axis.
        return viewport_rect.width() * (CSSPixels::nearest_value_for(m_value) / 100);
    case LengthUnit::Vb:
    case LengthUnit::Svb:
    case LengthUnit::Lvb:
    case LengthUnit::Dvb:
        // FIXME: Select the width or height based on which is the block axis.
        return viewport_rect.height() * (CSSPixels::nearest_value_for(m_value) / 100);
    case LengthUnit::Vmin:
    case LengthUnit::Svmin:
    case LengthUnit::Lvmin:
    case LengthUnit::Dvmin:
        return min(viewport_rect.width(), viewport_rect.height()) * (CSSPixels::nearest_value_for(m_value) / 100);
    case LengthUnit::Vmax:
    case LengthUnit::Svmax:
    case LengthUnit::Lvmax:
    case LengthUnit::Dvmax:
        return max(viewport_rect.width(), viewport_rect.height()) * (CSSPixels::nearest_value_for(m_value) / 100);
    default:
        VERIFY_NOT_REACHED();
    }
}

Length::ResolutionContext Length::ResolutionContext::for_element(DOM::AbstractElement const& element)
{
    auto const* root_element = element.element().document().document_element();

    VERIFY(element.computed_properties());
    VERIFY(root_element);
    VERIFY(root_element->computed_properties());

    return Length::ResolutionContext {
        .viewport_rect = element.element().navigable()->viewport_rect(),
        .font_metrics = { element.computed_properties()->font_size(), element.computed_properties()->first_available_computed_font().pixel_metrics(), element.computed_properties()->line_height() },
        .root_font_metrics = { root_element->computed_properties()->font_size(), root_element->computed_properties()->first_available_computed_font().pixel_metrics(), element.computed_properties()->line_height() }
    };
}

Length::ResolutionContext Length::ResolutionContext::for_window(HTML::Window const& window)
{
    auto const& initial_font = window.associated_document().style_computer().initial_font();
    Gfx::FontPixelMetrics const& initial_font_metrics = initial_font.pixel_metrics();
    Length::FontMetrics font_metrics { CSSPixels { initial_font.pixel_size() }, initial_font_metrics, InitialValues::line_height() };
    return Length::ResolutionContext {
        .viewport_rect = window.page().web_exposed_screen_area(),
        .font_metrics = font_metrics,
        .root_font_metrics = font_metrics,
    };
}

Length::ResolutionContext Length::ResolutionContext::for_layout_node(Layout::Node const& node)
{
    Layout::Node const* root_layout_node;

    if (is<DOM::Document>(node.dom_node())) {
        root_layout_node = &node;
    } else {
        auto const* root_element = node.document().document_element();
        VERIFY(root_element);
        VERIFY(root_element->layout_node());
        root_layout_node = root_element->layout_node();
    }

    return Length::ResolutionContext {
        .viewport_rect = node.navigable()->viewport_rect(),
        .font_metrics = { node.computed_values().font_size(), node.first_available_font().pixel_metrics(), node.computed_values().line_height() },
        .root_font_metrics = { root_layout_node->computed_values().font_size(), root_layout_node->first_available_font().pixel_metrics(), node.computed_values().line_height() },
    };
}

CSSPixels Length::to_px(ResolutionContext const& context) const
{
    return to_px(context.viewport_rect, context.font_metrics, context.root_font_metrics);
}

CSSPixels Length::to_px_slow_case(Layout::Node const& layout_node) const
{
    if (!layout_node.document().browsing_context())
        return 0;

    if (is_font_relative()) {
        auto* root_element = layout_node.document().document_element();
        if (!root_element || !root_element->layout_node())
            return 0;

        FontMetrics font_metrics {
            layout_node.computed_values().font_size(),
            layout_node.first_available_font().pixel_metrics(),
            layout_node.computed_values().line_height()
        };
        FontMetrics root_font_metrics {
            root_element->layout_node()->computed_values().font_size(),
            root_element->layout_node()->first_available_font().pixel_metrics(),
            layout_node.computed_values().line_height()
        };

        return font_relative_length_to_px(font_metrics, root_font_metrics);
    }

    VERIFY(is_viewport_relative());
    auto const& viewport_rect = layout_node.document().viewport_rect();
    return viewport_relative_length_to_px(viewport_rect);
}

String Length::to_string(SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <length>
    // The <number> component serialized as per <number> followed by the unit in its canonical form as defined in its
    // respective specification.

    // FIXME: Manually skip this for px so we avoid rounding errors in absolute_length_to_px.
    //        Maybe provide alternative functions that don't produce CSSPixels?
    if (serialization_mode == SerializationMode::ResolvedValue && is_absolute() && m_unit != LengthUnit::Px) {
        StringBuilder builder;
        serialize_a_number(builder, absolute_length_to_px().to_double());
        builder.append("px"sv);
        return builder.to_string_without_validation();
    }
    StringBuilder builder;
    serialize_a_number(builder, m_value);
    builder.append(unit_name());
    return builder.to_string_without_validation();
}

Optional<Length> Length::absolutize(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const
{
    if (is_px())
        return {};
    if (is_absolute() || is_relative()) {
        auto px = to_px(viewport_rect, font_metrics, root_font_metrics);
        return CSS::Length::make_px(px);
    }
    return {};
}

Length Length::absolutized(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const
{
    return absolutize(viewport_rect, font_metrics, root_font_metrics).value_or(*this);
}

Length Length::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, Length const& reference_value)
{
    CalculationResolutionContext context {
        .percentage_basis = reference_value,
        .length_resolution_context = ResolutionContext::for_layout_node(layout_node),
    };
    return calculated->resolve_length(context).value();
}

Length Length::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, CSSPixels reference_value)
{
    return resolve_calculated(calculated, layout_node, make_px(reference_value));
}

}
