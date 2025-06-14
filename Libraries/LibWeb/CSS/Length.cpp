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

Length::FontMetrics::FontMetrics(CSSPixels font_size, Gfx::FontPixelMetrics const& pixel_metrics)
    : font_size(font_size)
    , x_height(pixel_metrics.x_height)
    // FIXME: This is only approximately the cap height. The spec suggests measuring the "O" glyph:
    //        https://www.w3.org/TR/css-values-4/#cap
    , cap_height(pixel_metrics.ascent)
    , zero_advance(pixel_metrics.advance_of_ascii_zero)
    , line_height(round(pixel_metrics.line_spacing()))
{
}

Length::Length(double value, Type type)
    : m_type(type)
    , m_value(value)
{
}
Length::~Length() = default;

Length Length::make_auto()
{
    return Length(0, Type::Auto);
}

Length Length::make_px(double value)
{
    return Length(value, Type::Px);
}

Length Length::make_px(CSSPixels value)
{
    return make_px(value.to_double());
}

Length Length::percentage_of(Percentage const& percentage) const
{
    if (is_auto()) {
        dbgln("Attempting to get percentage of an auto length, this seems wrong? But for now we just return the original length.");
        return *this;
    }

    return Length { percentage.as_fraction() * raw_value(), m_type };
}

CSSPixels Length::font_relative_length_to_px(Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    switch (m_type) {
    case Type::Em:
        return CSSPixels::nearest_value_for(m_value * font_metrics.font_size.to_double());
    case Type::Rem:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.font_size.to_double());
    case Type::Ex:
        return CSSPixels::nearest_value_for(m_value * font_metrics.x_height.to_double());
    case Type::Rex:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.x_height.to_double());
    case Type::Cap:
        return CSSPixels::nearest_value_for(m_value * font_metrics.cap_height.to_double());
    case Type::Rcap:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.cap_height.to_double());
    case Type::Ch:
        return CSSPixels::nearest_value_for(m_value * font_metrics.zero_advance.to_double());
    case Type::Rch:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.zero_advance.to_double());
    case Type::Ic:
        // FIXME: Use the "advance measure of the “水” (CJK water ideograph, U+6C34) glyph"
        return CSSPixels::nearest_value_for(m_value * font_metrics.font_size.to_double());
    case Type::Ric:
        // FIXME: Use the "advance measure of the “水” (CJK water ideograph, U+6C34) glyph"
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.font_size.to_double());
    case Type::Lh:
        return CSSPixels::nearest_value_for(m_value * font_metrics.line_height.to_double());
    case Type::Rlh:
        return CSSPixels::nearest_value_for(m_value * root_font_metrics.line_height.to_double());
    default:
        VERIFY_NOT_REACHED();
    }
}

CSSPixels Length::viewport_relative_length_to_px(CSSPixelRect const& viewport_rect) const
{
    switch (m_type) {
    case Type::Vw:
    case Type::Svw:
    case Type::Lvw:
    case Type::Dvw:
        return viewport_rect.width() * (CSSPixels::nearest_value_for(m_value) / 100);
    case Type::Vh:
    case Type::Svh:
    case Type::Lvh:
    case Type::Dvh:
        return viewport_rect.height() * (CSSPixels::nearest_value_for(m_value) / 100);
    case Type::Vi:
    case Type::Svi:
    case Type::Lvi:
    case Type::Dvi:
        // FIXME: Select the width or height based on which is the inline axis.
        return viewport_rect.width() * (CSSPixels::nearest_value_for(m_value) / 100);
    case Type::Vb:
    case Type::Svb:
    case Type::Lvb:
    case Type::Dvb:
        // FIXME: Select the width or height based on which is the block axis.
        return viewport_rect.height() * (CSSPixels::nearest_value_for(m_value) / 100);
    case Type::Vmin:
    case Type::Svmin:
    case Type::Lvmin:
    case Type::Dvmin:
        return min(viewport_rect.width(), viewport_rect.height()) * (CSSPixels::nearest_value_for(m_value) / 100);
    case Type::Vmax:
    case Type::Svmax:
    case Type::Lvmax:
    case Type::Dvmax:
        return max(viewport_rect.width(), viewport_rect.height()) * (CSSPixels::nearest_value_for(m_value) / 100);
    default:
        VERIFY_NOT_REACHED();
    }
}

Length::ResolutionContext Length::ResolutionContext::for_window(HTML::Window const& window)
{
    auto const& initial_font = window.associated_document().style_computer().initial_font();
    Gfx::FontPixelMetrics const& initial_font_metrics = initial_font.pixel_metrics();
    Length::FontMetrics font_metrics { CSSPixels { initial_font.pixel_size() }, initial_font_metrics };
    return Length::ResolutionContext {
        .viewport_rect = window.page().web_exposed_screen_area(),
        .font_metrics = font_metrics,
        .root_font_metrics = font_metrics,
    };
}

Length::ResolutionContext Length::ResolutionContext::for_layout_node(Layout::Node const& node)
{
    auto const* root_element = node.document().document_element();
    VERIFY(root_element);
    VERIFY(root_element->layout_node());
    return Length::ResolutionContext {
        .viewport_rect = node.navigable()->viewport_rect(),
        .font_metrics = { node.computed_values().font_size(), node.first_available_font().pixel_metrics() },
        .root_font_metrics = { root_element->layout_node()->computed_values().font_size(), root_element->layout_node()->first_available_font().pixel_metrics() },
    };
}

CSSPixels Length::to_px(ResolutionContext const& context) const
{
    return to_px(context.viewport_rect, context.font_metrics, context.root_font_metrics);
}

CSSPixels Length::to_px_slow_case(Layout::Node const& layout_node) const
{
    if (is_auto()) {
        // FIXME: We really, really shouldn't end up here, but we do, and so frequently that
        //        adding a dbgln() here outputs a couple hundred lines loading `welcome.html`.
        return 0;
    }
    if (!layout_node.document().browsing_context())
        return 0;

    if (is_font_relative()) {
        auto* root_element = layout_node.document().document_element();
        if (!root_element || !root_element->layout_node())
            return 0;

        FontMetrics font_metrics {
            layout_node.computed_values().font_size(),
            layout_node.first_available_font().pixel_metrics(),
        };
        FontMetrics root_font_metrics {
            root_element->layout_node()->computed_values().font_size(),
            root_element->layout_node()->first_available_font().pixel_metrics(),
        };

        return font_relative_length_to_px(font_metrics, root_font_metrics);
    }

    VERIFY(is_viewport_relative());
    auto const& viewport_rect = layout_node.document().viewport_rect();
    return viewport_relative_length_to_px(viewport_rect);
}

String Length::to_string(SerializationMode serialization_mode) const
{
    if (is_auto())
        return "auto"_string;
    // FIXME: Manually skip this for px so we avoid rounding errors in absolute_length_to_px.
    //        Maybe provide alternative functions that don't produce CSSPixels?
    if (serialization_mode == SerializationMode::ResolvedValue && is_absolute() && m_type != Type::Px)
        return MUST(String::formatted("{:.5}px", absolute_length_to_px()));
    return MUST(String::formatted("{:.5}{}", m_value, unit_name()));
}

StringView Length::unit_name() const
{
    switch (m_type) {
    case Type::Em:
        return "em"_sv;
    case Type::Rem:
        return "rem"_sv;
    case Type::Ex:
        return "ex"_sv;
    case Type::Rex:
        return "rex"_sv;
    case Type::Cap:
        return "cap"_sv;
    case Type::Rcap:
        return "rcap"_sv;
    case Type::Ch:
        return "ch"_sv;
    case Type::Rch:
        return "rch"_sv;
    case Type::Ic:
        return "ic"_sv;
    case Type::Ric:
        return "ric"_sv;
    case Type::Lh:
        return "lh"_sv;
    case Type::Rlh:
        return "rlh"_sv;
    case Type::Vw:
        return "vw"_sv;
    case Type::Svw:
        return "_svw"_sv;
    case Type::Lvw:
        return "lvw"_sv;
    case Type::Dvw:
        return "dvw"_sv;
    case Type::Vh:
        return "vh"_sv;
    case Type::Svh:
        return "_svh"_sv;
    case Type::Lvh:
        return "lvh"_sv;
    case Type::Dvh:
        return "dvh"_sv;
    case Type::Vi:
        return "vi"_sv;
    case Type::Svi:
        return "_svi"_sv;
    case Type::Lvi:
        return "lvi"_sv;
    case Type::Dvi:
        return "dvi"_sv;
    case Type::Vb:
        return "vb"_sv;
    case Type::Svb:
        return "_svb"_sv;
    case Type::Lvb:
        return "lvb"_sv;
    case Type::Dvb:
        return "dvb"_sv;
    case Type::Vmin:
        return "vmin"_sv;
    case Type::Svmin:
        return "_svmin"_sv;
    case Type::Lvmin:
        return "lvmin"_sv;
    case Type::Dvmin:
        return "dvmin"_sv;
    case Type::Vmax:
        return "vmax"_sv;
    case Type::Svmax:
        return "_svmax"_sv;
    case Type::Lvmax:
        return "lvmax"_sv;
    case Type::Dvmax:
        return "dvmax"_sv;
    case Type::Cm:
        return "cm"_sv;
    case Type::Mm:
        return "mm"_sv;
    case Type::Q:
        return "Q"_sv;
    case Type::In:
        return "in"_sv;
    case Type::Pt:
        return "pt"_sv;
    case Type::Pc:
        return "pc"_sv;
    case Type::Px:
        return "px"_sv;
    case Type::Auto:
        return "auto"_sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Length::Type> Length::unit_from_name(StringView name)
{
    if (name.equals_ignoring_ascii_case("em"_sv)) {
        return Length::Type::Em;
    } else if (name.equals_ignoring_ascii_case("rem"_sv)) {
        return Length::Type::Rem;
    } else if (name.equals_ignoring_ascii_case("ex"_sv)) {
        return Length::Type::Ex;
    } else if (name.equals_ignoring_ascii_case("rex"_sv)) {
        return Length::Type::Rex;
    } else if (name.equals_ignoring_ascii_case("cap"_sv)) {
        return Length::Type::Cap;
    } else if (name.equals_ignoring_ascii_case("rcap"_sv)) {
        return Length::Type::Rcap;
    } else if (name.equals_ignoring_ascii_case("ch"_sv)) {
        return Length::Type::Ch;
    } else if (name.equals_ignoring_ascii_case("rch"_sv)) {
        return Length::Type::Rch;
    } else if (name.equals_ignoring_ascii_case("ic"_sv)) {
        return Length::Type::Ic;
    } else if (name.equals_ignoring_ascii_case("ric"_sv)) {
        return Length::Type::Ric;
    } else if (name.equals_ignoring_ascii_case("lh"_sv)) {
        return Length::Type::Lh;
    } else if (name.equals_ignoring_ascii_case("rlh"_sv)) {
        return Length::Type::Rlh;
    } else if (name.equals_ignoring_ascii_case("vw"_sv)) {
        return Length::Type::Vw;
    } else if (name.equals_ignoring_ascii_case("_svw"_sv)) {
        return Length::Type::Svw;
    } else if (name.equals_ignoring_ascii_case("lvw"_sv)) {
        return Length::Type::Lvw;
    } else if (name.equals_ignoring_ascii_case("dvw"_sv)) {
        return Length::Type::Dvw;
    } else if (name.equals_ignoring_ascii_case("vh"_sv)) {
        return Length::Type::Vh;
    } else if (name.equals_ignoring_ascii_case("_svh"_sv)) {
        return Length::Type::Svh;
    } else if (name.equals_ignoring_ascii_case("lvh"_sv)) {
        return Length::Type::Lvh;
    } else if (name.equals_ignoring_ascii_case("dvh"_sv)) {
        return Length::Type::Dvh;
    } else if (name.equals_ignoring_ascii_case("vi"_sv)) {
        return Length::Type::Vi;
    } else if (name.equals_ignoring_ascii_case("_svi"_sv)) {
        return Length::Type::Svi;
    } else if (name.equals_ignoring_ascii_case("lvi"_sv)) {
        return Length::Type::Lvi;
    } else if (name.equals_ignoring_ascii_case("dvi"_sv)) {
        return Length::Type::Dvi;
    } else if (name.equals_ignoring_ascii_case("vb"_sv)) {
        return Length::Type::Vb;
    } else if (name.equals_ignoring_ascii_case("_svb"_sv)) {
        return Length::Type::Svb;
    } else if (name.equals_ignoring_ascii_case("lvb"_sv)) {
        return Length::Type::Lvb;
    } else if (name.equals_ignoring_ascii_case("dvb"_sv)) {
        return Length::Type::Dvb;
    } else if (name.equals_ignoring_ascii_case("vmin"_sv)) {
        return Length::Type::Vmin;
    } else if (name.equals_ignoring_ascii_case("_svmin"_sv)) {
        return Length::Type::Svmin;
    } else if (name.equals_ignoring_ascii_case("lvmin"_sv)) {
        return Length::Type::Lvmin;
    } else if (name.equals_ignoring_ascii_case("dvmin"_sv)) {
        return Length::Type::Dvmin;
    } else if (name.equals_ignoring_ascii_case("vmax"_sv)) {
        return Length::Type::Vmax;
    } else if (name.equals_ignoring_ascii_case("_svmax"_sv)) {
        return Length::Type::Svmax;
    } else if (name.equals_ignoring_ascii_case("lvmax"_sv)) {
        return Length::Type::Lvmax;
    } else if (name.equals_ignoring_ascii_case("dvmax"_sv)) {
        return Length::Type::Dvmax;
    } else if (name.equals_ignoring_ascii_case("cm"_sv)) {
        return Length::Type::Cm;
    } else if (name.equals_ignoring_ascii_case("mm"_sv)) {
        return Length::Type::Mm;
    } else if (name.equals_ignoring_ascii_case("Q"_sv)) {
        return Length::Type::Q;
    } else if (name.equals_ignoring_ascii_case("in"_sv)) {
        return Length::Type::In;
    } else if (name.equals_ignoring_ascii_case("pt"_sv)) {
        return Length::Type::Pt;
    } else if (name.equals_ignoring_ascii_case("pc"_sv)) {
        return Length::Type::Pc;
    } else if (name.equals_ignoring_ascii_case("px"_sv)) {
        return Length::Type::Px;
    }

    return {};
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
    return calculated->resolve_length(
                         {
                             .percentage_basis = reference_value,
                             .length_resolution_context = ResolutionContext::for_layout_node(layout_node),
                         })
        .value();
}

Length Length::resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const& calculated, Layout::Node const& layout_node, CSSPixels reference_value)
{
    return calculated->resolve_length(
                         {
                             .percentage_basis = make_px(reference_value),
                             .length_resolution_context = ResolutionContext::for_layout_node(layout_node),
                         })
        .value();
}

}
