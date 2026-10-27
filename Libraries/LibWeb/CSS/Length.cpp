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
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/StyleEngineRustFFI.h>

namespace Web::CSS {

void Length::ResolutionContext::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(subject_element);
}

static bool inline_axis_is_horizontal(WritingMode writing_mode)
{
    return writing_mode == WritingMode::HorizontalTb;
}

ComputedValuesFFI::FfiFontMetrics font_metrics_from(CSSPixels font_size, Gfx::FontPixelMetrics const& pixel_metrics, CSSPixels line_height)
{
    return {
        .font_size = font_size.to_double(),
        .x_height = CSSPixels(pixel_metrics.x_height).to_double(),
        // FIXME: This is only approximately the cap height. The spec suggests measuring the "O" glyph:
        //        https://www.w3.org/TR/css-values-4/#cap
        .cap_height = CSSPixels(pixel_metrics.ascent).to_double(),
        .zero_advance = CSSPixels(pixel_metrics.advance_of_ascii_zero).to_double(),
        .line_height = line_height.to_double(),
    };
}

Length Length::percentage_of(Percentage const& percentage) const
{
    return Length { percentage.as_fraction() * raw_value(), m_unit };
}

ComputedValuesFFI::FfiLengthResolutionContext to_ffi_length_resolution_context_with_container_bases(Length::ResolutionContext const& context, u8 container_relative_length_unit_mask)
{
    auto ffi_context = to_ffi_length_resolution_context(context);
    auto viewport_width = context.viewport_rect.width().to_double();
    auto viewport_height = context.viewport_rect.height().to_double();

    if (!context.subject_element) {
        ffi_context.has_container_width_basis = true;
        ffi_context.has_container_height_basis = true;
        ffi_context.container_width_basis = viewport_width;
        ffi_context.container_height_basis = viewport_height;
        ffi_context.container_width_basis_depends_on_viewport_metrics = true;
        ffi_context.container_height_basis_depends_on_viewport_metrics = true;
        return ffi_context;
    }

    auto& subject = const_cast<DOM::Element&>(*context.subject_element);
    auto& document = subject.document();
    auto& style_computer = document.style_computer();
    StyleEngineFFI::FfiContainerUnitBases bases;
    {
        Layout::ForcedReadScope read { document };
        bases = StyleEngineFFI::style_engine_container_unit_bases(style_computer.style_engine().host(), read, subject.style_node_id().value(), container_relative_length_unit_mask, context.subject_inline_axis_is_horizontal);
    }
    if (!bases.reads_width && !bases.reads_height)
        return ffi_context;

    subject.set_style_depends_on_size_container_query();
    auto apply_axis_basis = [&](u32 container_style_node, double container_basis, bool container_has_no_box, double viewport_basis, bool& has_basis, double& basis, bool& depends_on_viewport_metrics) {
        has_basis = true;
        if (!container_style_node) {
            basis = viewport_basis;
            depends_on_viewport_metrics = true;
            return;
        }
        basis = container_basis;
        auto container = style_computer.element_for_style_node(StyleNodeID { container_style_node });
        if (!container)
            return;
        container->set_is_size_query_container();
        if (container_has_no_box && (!document.layout_is_up_to_date() || document.is_running_update_layout()))
            document.set_needs_container_query_evaluation_after_layout(*container);
    };
    if (bases.reads_width)
        apply_axis_basis(bases.width_container, bases.width, bases.width_container_has_no_box, viewport_width, ffi_context.has_container_width_basis, ffi_context.container_width_basis, ffi_context.container_width_basis_depends_on_viewport_metrics);
    if (bases.reads_height)
        apply_axis_basis(bases.height_container, bases.height, bases.height_container_has_no_box, viewport_height, ffi_context.has_container_height_basis, ffi_context.container_height_basis, ffi_context.container_height_basis_depends_on_viewport_metrics);

    return ffi_context;
}

Length::ResolutionContext Length::ResolutionContext::for_element(DOM::AbstractElement const& element)
{
    auto computed_values = element.computed_style();
    VERIFY(computed_values);
    return for_element(element, *computed_values);
}

Length::ResolutionContext Length::ResolutionContext::for_element(DOM::AbstractElement const& element, ComputedValues const& computed_values)
{
    auto const* root_element = element.element().document().document_element();

    VERIFY(root_element);
    auto root_computed_values = root_element->computed_style();
    VERIFY(root_computed_values);

    return Length::ResolutionContext {
        .viewport_rect = element.element().navigable()->viewport_rect(),
        .font_metrics = font_metrics_from(computed_values.font_size(), computed_values.font_list().first_available_font().pixel_metrics(), computed_values.line_height()),
        .root_font_metrics = font_metrics_from(root_computed_values->font_size(), root_computed_values->font_list().first_available_font().pixel_metrics(), root_computed_values->line_height()),
        .font_metrics_depend_on_viewport_metrics = computed_values.font_metrics_depend_on_viewport_metrics(),
        .root_font_metrics_depend_on_viewport_metrics = root_computed_values->font_metrics_depend_on_viewport_metrics(),
        .subject_inline_axis_is_horizontal = inline_axis_is_horizontal(computed_values.writing_mode()),
        .subject_element = &element.element(),
    };
}

Length::ResolutionContext Length::ResolutionContext::for_document(DOM::Document const& document)
{
    auto const& initial_font = document.font_computer().initial_font();
    Gfx::FontPixelMetrics const& initial_font_metrics = initial_font.pixel_metrics();
    auto font_metrics = font_metrics_from(CSSPixels { initial_font.pixel_size() }, initial_font_metrics, InitialValues::line_height());
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

void Length::serialize(Utf16StringBuilder& builder, SerializationMode serialization_mode) const
{
    // https://drafts.csswg.org/cssom/#serialize-a-css-value
    // -> <length>
    // The <number> component serialized as per <number> followed by the unit in its canonical form as defined in its
    // respective specification.

    // FIXME: Manually skip this for px so we avoid rounding errors in absolute_length_to_px.
    //        Maybe provide alternative functions that don't produce CSSPixels?
    if (serialization_mode == SerializationMode::ResolvedValue && is_absolute() && m_unit != LengthUnit::Px) {
        serialize_a_number(builder, absolute_length_to_px().to_double());
        builder.append_ascii("px"sv);
        return;
    }
    serialize_a_number(builder, m_value);
    builder.append(unit_name());
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

}
