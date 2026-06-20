/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Variant.h>
#include <LibGC/Cell.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Rect.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

void Length::ResolutionContext::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(subject_element);
    visitor.visit(cached_width_query_container);
    visitor.visit(cached_height_query_container);
}

enum class ContainerRelativeAxis {
    Width,
    Height,
};

static bool inline_axis_is_horizontal(WritingMode writing_mode)
{
    return writing_mode == WritingMode::HorizontalTb;
}

static ContainerRelativeAxis logical_axis_to_physical_axis(bool inline_axis_is_horizontal, ContainerRelativeAxis logical_axis)
{
    if (inline_axis_is_horizontal)
        return logical_axis;
    return logical_axis == ContainerRelativeAxis::Width ? ContainerRelativeAxis::Height : ContainerRelativeAxis::Width;
}

static bool query_container_is_eligible_for_axis(DOM::Element const& container, ContainerRelativeAxis axis)
{
    auto style = container.computed_properties();
    if (!style)
        return false;

    auto container_type = style->container_type();
    auto container_inline_axis_is_horizontal = inline_axis_is_horizontal(style->writing_mode());

    if (axis == ContainerRelativeAxis::Width) {
        if (container_inline_axis_is_horizontal)
            return container_type.is_size_container || container_type.is_inline_size_container;
        return container_type.is_size_container;
    }

    if (container_inline_axis_is_horizontal)
        return container_type.is_size_container;
    return container_type.is_size_container || container_type.is_inline_size_container;
}

static DOM::Element const* nearest_query_container_for_axis(DOM::Element const& subject, ContainerRelativeAxis axis)
{
    for (auto const* container = subject.flat_tree_parent_element(); container; container = container->flat_tree_parent_element()) {
        if (query_container_is_eligible_for_axis(*container, axis))
            return container;
    }
    return nullptr;
}

static Optional<GC::Ptr<DOM::Element const>>& cached_query_container_for_axis(Length::ResolutionContext const& context, ContainerRelativeAxis axis)
{
    if (axis == ContainerRelativeAxis::Width)
        return context.cached_width_query_container;
    return context.cached_height_query_container;
}

static GC::Ptr<DOM::Element const> get_or_compute_query_container_for_axis(Length::ResolutionContext const& context, ContainerRelativeAxis axis)
{
    auto& cached_query_container = cached_query_container_for_axis(context, axis);
    if (!cached_query_container.has_value()) {
        cached_query_container = nullptr;
        if (context.subject_element)
            cached_query_container = nearest_query_container_for_axis(*context.subject_element, axis);
    }
    return cached_query_container.value();
}

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

Length Length::percentage_of(Percentage const& percentage) const
{
    return Length { percentage.as_fraction() * raw_value(), m_unit };
}

CSSPixels Length::font_relative_length_to_px(Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    return CSSPixels::nearest_value_for(font_relative_length_to_px_without_rounding(font_metrics, root_font_metrics));
}

double Length::font_relative_length_to_px_without_rounding(Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    switch (m_unit) {
    case LengthUnit::Em:
        return m_value * font_metrics.font_size.to_double();
    case LengthUnit::Rem:
        return m_value * root_font_metrics.font_size.to_double();
    case LengthUnit::Ex:
        return m_value * font_metrics.x_height.to_double();
    case LengthUnit::Rex:
        return m_value * root_font_metrics.x_height.to_double();
    case LengthUnit::Cap:
        return m_value * font_metrics.cap_height.to_double();
    case LengthUnit::Rcap:
        return m_value * root_font_metrics.cap_height.to_double();
    case LengthUnit::Ch:
        return m_value * font_metrics.zero_advance.to_double();
    case LengthUnit::Rch:
        return m_value * root_font_metrics.zero_advance.to_double();
    case LengthUnit::Ic:
        // FIXME: Use the "advance measure of the “水” (CJK water ideograph, U+6C34) glyph"
        return m_value * font_metrics.font_size.to_double();
    case LengthUnit::Ric:
        // FIXME: Use the "advance measure of the “水” (CJK water ideograph, U+6C34) glyph"
        return m_value * root_font_metrics.font_size.to_double();
    case LengthUnit::Lh:
        return m_value * font_metrics.line_height.to_double();
    case LengthUnit::Rlh:
        return m_value * root_font_metrics.line_height.to_double();
    default:
        VERIFY_NOT_REACHED();
    }
}

CSSPixels Length::viewport_relative_length_to_px(CSSPixelRect const& viewport_rect) const
{
    return CSSPixels::nearest_value_for(viewport_relative_length_to_px_without_rounding(viewport_rect));
}

double Length::viewport_relative_length_to_px_without_rounding(CSSPixelRect const& viewport_rect) const
{
    switch (m_unit) {
    case LengthUnit::Vw:
    case LengthUnit::Svw:
    case LengthUnit::Lvw:
    case LengthUnit::Dvw:
        return viewport_rect.width() * m_value / 100;
    case LengthUnit::Vh:
    case LengthUnit::Svh:
    case LengthUnit::Lvh:
    case LengthUnit::Dvh:
        return viewport_rect.height() * m_value / 100;
    case LengthUnit::Vi:
    case LengthUnit::Svi:
    case LengthUnit::Lvi:
    case LengthUnit::Dvi:
        // FIXME: Select the width or height based on which is the inline axis.
        return viewport_rect.width() * m_value / 100;
    case LengthUnit::Vb:
    case LengthUnit::Svb:
    case LengthUnit::Lvb:
    case LengthUnit::Dvb:
        // FIXME: Select the width or height based on which is the block axis.
        return viewport_rect.height() * m_value / 100;
    case LengthUnit::Vmin:
    case LengthUnit::Svmin:
    case LengthUnit::Lvmin:
    case LengthUnit::Dvmin:
        return min(viewport_rect.width(), viewport_rect.height()) * m_value / 100;
    case LengthUnit::Vmax:
    case LengthUnit::Svmax:
    case LengthUnit::Lvmax:
    case LengthUnit::Dvmax:
        return max(viewport_rect.width(), viewport_rect.height()) * m_value / 100;
    default:
        VERIFY_NOT_REACHED();
    }
}

double Length::container_relative_length_to_px_without_rounding(ResolutionContext const& context) const
{
    // https://drafts.csswg.org/css-conditional-5/#container-lengths
    auto resolve_physical_axis_basis_length = [&](ContainerRelativeAxis physical_axis) {
        // The query container for each axis is the nearest ancestor container that accepts container size queries on
        // that axis. If no eligible query container is available, then use the small viewport size for that axis.
        if (!context.subject_element) {
            context.record_viewport_relative_length_resolution();
            auto viewport_length = physical_axis == ContainerRelativeAxis::Width ? context.viewport_rect.width() : context.viewport_rect.height();
            return viewport_length.to_double();
        }

        auto& subject = *context.subject_element;
        const_cast<DOM::Element&>(subject).set_style_depends_on_size_container_query();

        auto query_container = get_or_compute_query_container_for_axis(context, physical_axis);
        if (!query_container) {
            context.record_viewport_relative_length_resolution();
            auto viewport_length = physical_axis == ContainerRelativeAxis::Width ? context.viewport_rect.width() : context.viewport_rect.height();
            return viewport_length.to_double();
        }

        auto paintable_box = query_container->unsafe_paintable_box();
        if (!paintable_box) {
            if (!subject.document().layout_is_up_to_date())
                const_cast<DOM::Document&>(subject.document()).set_needs_container_query_evaluation_after_layout(*query_container);
            return 0.0;
        }

        auto container_length = physical_axis == ContainerRelativeAxis::Width ? paintable_box->content_width() : paintable_box->content_height();
        return container_length.to_double();
    };

    auto resolve_physical_axis_length = [&](ContainerRelativeAxis physical_axis) {
        return resolve_physical_axis_basis_length(physical_axis) * m_value / 100;
    };

    auto resolve_logical_axis_basis_length = [&](ContainerRelativeAxis logical_axis) {
        return resolve_physical_axis_basis_length(logical_axis_to_physical_axis(context.subject_inline_axis_is_horizontal, logical_axis));
    };

    auto resolve_logical_axis_length = [&](ContainerRelativeAxis logical_axis) {
        return resolve_logical_axis_basis_length(logical_axis) * m_value / 100;
    };

    switch (m_unit) {
    case LengthUnit::Cqw:
        return resolve_physical_axis_length(ContainerRelativeAxis::Width);
    case LengthUnit::Cqh:
        return resolve_physical_axis_length(ContainerRelativeAxis::Height);
    case LengthUnit::Cqi:
        return resolve_logical_axis_length(ContainerRelativeAxis::Width);
    case LengthUnit::Cqb:
        return resolve_logical_axis_length(ContainerRelativeAxis::Height);
    case LengthUnit::Cqmin:
        return min(resolve_logical_axis_basis_length(ContainerRelativeAxis::Width), resolve_logical_axis_basis_length(ContainerRelativeAxis::Height)) * m_value / 100;
    case LengthUnit::Cqmax:
        return max(resolve_logical_axis_basis_length(ContainerRelativeAxis::Width), resolve_logical_axis_basis_length(ContainerRelativeAxis::Height)) * m_value / 100;
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
        .font_metrics = { element.computed_properties()->font_size(), element.computed_properties()->first_available_computed_font(element.document().font_computer())->pixel_metrics(), element.computed_properties()->line_height() },
        .root_font_metrics = { root_element->computed_properties()->font_size(), root_element->computed_properties()->first_available_computed_font(element.document().font_computer())->pixel_metrics(), element.computed_properties()->line_height() },
        .font_metrics_depend_on_viewport_metrics = element.computed_properties()->font_metrics_depend_on_viewport_metrics(),
        .root_font_metrics_depend_on_viewport_metrics = root_element->computed_properties()->font_metrics_depend_on_viewport_metrics(),
        .subject_inline_axis_is_horizontal = inline_axis_is_horizontal(element.computed_properties()->writing_mode()),
        .subject_element = &element.element(),
    };
}

Length::ResolutionContext Length::ResolutionContext::for_document(DOM::Document const& document)
{
    auto const& initial_font = document.font_computer().initial_font();
    Gfx::FontPixelMetrics const& initial_font_metrics = initial_font.pixel_metrics();
    Length::FontMetrics font_metrics { CSSPixels { initial_font.pixel_size() }, initial_font_metrics, InitialValues::line_height() };
    CSSPixelRect viewport_rect;

    if (document.navigable())
        viewport_rect = document.navigable()->viewport_rect();

    return Length::ResolutionContext {
        .viewport_rect = viewport_rect,
        .font_metrics = font_metrics,
        .root_font_metrics = font_metrics,
        .subject_inline_axis_is_horizontal = true,
    };
}

Length::ResolutionContext Length::ResolutionContext::for_layout_node(Layout::Node const& node)
{
    Layout::Node const* root_layout_node;
    DOM::Element const* subject_element = nullptr;

    if (is<DOM::Document>(node.dom_node())) {
        root_layout_node = &node;
    } else {
        auto const* root_element = node.document().document_element();
        VERIFY(root_element);
        // NB: Called during CSS length resolution, which may happen during style recalculation.
        VERIFY(root_element->unsafe_layout_node());
        root_layout_node = root_element->unsafe_layout_node();
    }

    if (auto const* dom_node = node.dom_node(); dom_node && is<DOM::Element>(*dom_node))
        subject_element = &as<DOM::Element>(*dom_node);

    return Length::ResolutionContext {
        .viewport_rect = node.navigable()->viewport_rect(),
        .font_metrics = { node.computed_values().font_size(), node.first_available_font().pixel_metrics(), node.computed_values().line_height() },
        .root_font_metrics = { root_layout_node->computed_values().font_size(), root_layout_node->first_available_font().pixel_metrics(), node.computed_values().line_height() },
        .subject_inline_axis_is_horizontal = inline_axis_is_horizontal(node.computed_values().writing_mode()),
        .subject_element = subject_element,
    };
}

CSSPixels Length::to_px(ResolutionContext const& context) const
{
    return CSSPixels::nearest_value_for(to_px_without_rounding(context));
}

CSSPixels Length::to_px_slow_case(Layout::Node const& layout_node) const
{
    if (!layout_node.document().browsing_context())
        return 0;
    return to_px(ResolutionContext::for_layout_node(layout_node));
}

void Length::serialize(StringBuilder& builder, SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <length>
    // The <number> component serialized as per <number> followed by the unit in its canonical form as defined in its
    // respective specification.

    // FIXME: Manually skip this for px so we avoid rounding errors in absolute_length_to_px.
    //        Maybe provide alternative functions that don't produce CSSPixels?
    if (serialization_mode == SerializationMode::ResolvedValue && is_absolute() && m_unit != LengthUnit::Px) {
        serialize_a_number(builder, absolute_length_to_px().to_double());
        builder.append("px"sv);
        return;
    }
    serialize_a_number(builder, m_value);
    builder.append(unit_name());
}

String Length::to_string(SerializationMode serialization_mode) const
{
    StringBuilder builder;
    serialize(builder, serialization_mode);
    return builder.to_string_without_validation();
}

Optional<Length> Length::absolutize(ResolutionContext const& context) const
{
    if (is_px())
        return {};

    return CSS::Length::make_px(to_px_without_rounding(context));
}

Length Length::from_style_value(NonnullRefPtr<StyleValue const> const& style_value, Optional<Length> percentage_basis)
{
    if (style_value->is_length())
        return style_value->as_length().length();

    if (style_value->is_calculated()) {
        CalculationResolutionContext::PercentageBasis resolved_percentage_basis;

        if (percentage_basis.has_value()) {
            resolved_percentage_basis = percentage_basis.value();
        }

        return style_value->as_calculated().resolve_length({ .percentage_basis = resolved_percentage_basis }).value();
    }

    if (style_value->is_percentage()) {
        VERIFY(percentage_basis.has_value());

        return percentage_basis.value().percentage_of(style_value->as_percentage().percentage());
    }

    VERIFY_NOT_REACHED();
}

LengthOrAuto LengthOrAuto::from_style_value(NonnullRefPtr<StyleValue const> const& style_value, Optional<Length> percentage_basis)
{
    if (style_value->has_auto())
        return make_auto();
    return LengthOrAuto { Length::from_style_value(style_value, percentage_basis) };
}

}
