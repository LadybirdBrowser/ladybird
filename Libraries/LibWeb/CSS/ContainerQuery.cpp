/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContainerQuery.h"
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::CSS {

Optional<SizeFeatureID> size_feature_id_from_string(StringView name)
{
    if (name.equals_ignoring_ascii_case("aspect-ratio"sv))
        return SizeFeatureID::AspectRatio;
    if (name.equals_ignoring_ascii_case("block-size"sv))
        return SizeFeatureID::BlockSize;
    if (name.equals_ignoring_ascii_case("height"sv))
        return SizeFeatureID::Height;
    if (name.equals_ignoring_ascii_case("inline-size"sv))
        return SizeFeatureID::InlineSize;
    if (name.equals_ignoring_ascii_case("orientation"sv))
        return SizeFeatureID::Orientation;
    if (name.equals_ignoring_ascii_case("width"sv))
        return SizeFeatureID::Width;
    return {};
}

StringView string_from_size_feature_id(SizeFeatureID id)
{
    switch (id) {
    case SizeFeatureID::AspectRatio:
        return "aspect-ratio"sv;
    case SizeFeatureID::BlockSize:
        return "block-size"sv;
    case SizeFeatureID::Height:
        return "height"sv;
    case SizeFeatureID::InlineSize:
        return "inline-size"sv;
    case SizeFeatureID::Orientation:
        return "orientation"sv;
    case SizeFeatureID::Width:
        return "width"sv;
    }
    VERIFY_NOT_REACHED();
}

bool size_feature_type_is_range(SizeFeatureID id)
{
    switch (id) {
    case SizeFeatureID::AspectRatio:
    case SizeFeatureID::BlockSize:
    case SizeFeatureID::Height:
    case SizeFeatureID::InlineSize:
    case SizeFeatureID::Width:
        return true;
    case SizeFeatureID::Orientation:
        return false;
    }
    VERIFY_NOT_REACHED();
}

static FeatureValue size_feature_value_for_query_container(SizeFeatureID id, Painting::PaintableBox const& paintable_box)
{
    auto width = paintable_box.content_width();
    auto height = paintable_box.content_height();
    auto inline_axis_horizontal = paintable_box.computed_values().writing_mode() == WritingMode::HorizontalTb;

    auto length_feature_value = [](CSSPixels length) {
        return FeatureValue(FeatureValue::Type::Length, LengthStyleValue::create(Length::make_px(length)));
    };

    switch (id) {
    case SizeFeatureID::AspectRatio:
        return FeatureValue(
            FeatureValue::Type::Ratio,
            RatioStyleValue::create(
                NumberStyleValue::create(width.to_double()),
                NumberStyleValue::create(height.to_double())));
    case SizeFeatureID::BlockSize:
        return length_feature_value(inline_axis_horizontal ? height : width);
    case SizeFeatureID::Height:
        return length_feature_value(height);
    case SizeFeatureID::InlineSize:
        return length_feature_value(inline_axis_horizontal ? width : height);
    case SizeFeatureID::Orientation:
        return FeatureValue(
            FeatureValue::Type::Ident,
            KeywordStyleValue::create(height >= width ? Keyword::Portrait : Keyword::Landscape));
    case SizeFeatureID::Width:
        return length_feature_value(width);
    }
    VERIFY_NOT_REACHED();
}

StringView SizeFeature::serialize_feature_id(SizeFeatureID id)
{
    return string_from_size_feature_id(id);
}

bool SizeFeature::keyword_is_falsey(SizeFeatureID, Keyword)
{
    // Boolean evaluation is not valid in <size-feature>.
    return false;
}

MatchResult SizeFeature::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    if (!context.query_container)
        return MatchResult::Unknown;

    auto paintable_box = context.query_container->unsafe_paintable_box();
    if (!paintable_box) {
        if (!context.query_container->document().layout_is_up_to_date())
            const_cast<DOM::Document&>(context.query_container->document()).set_needs_container_query_evaluation_after_layout(*context.query_container);
        return MatchResult::Unknown;
    }

    auto queried_value = size_feature_value_for_query_container(id(), *paintable_box);
    ComputationContext computation_context {
        .length_resolution_context = Length::ResolutionContext::for_layout_node(paintable_box->layout_node()),
        .abstract_element = DOM::AbstractElement { *context.query_container },
    };

    return evaluate_internal(queried_value, computation_context);
}

void SizeFeature::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    switch (id()) {
    case SizeFeatureID::AspectRatio:
    case SizeFeatureID::Orientation:
        requirements.requires_width_container = true;
        requirements.requires_height_container = true;
        break;
    case SizeFeatureID::BlockSize:
        requirements.requires_block_size_container = true;
        break;
    case SizeFeatureID::Height:
        requirements.requires_height_container = true;
        break;
    case SizeFeatureID::InlineSize:
        requirements.requires_inline_size_container = true;
        break;
    case SizeFeatureID::Width:
        requirements.requires_width_container = true;
        break;
    }
}

void SizeFeature::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("SizeFeature: {}\n", to_string());
}

NonnullRefPtr<ContainerQuery> ContainerQuery::create(NonnullOwnPtr<BooleanExpression>&& condition)
{
    return adopt_ref(*new ContainerQuery(move(condition)));
}

ContainerQuery::ContainerQuery(NonnullOwnPtr<BooleanExpression>&& condition)
    : m_condition(move(condition))
    , m_matches(m_condition->evaluate_to_boolean({}))
{
    m_condition->collect_container_query_feature_requirements(m_feature_requirements);
}

static bool container_satisfies_requirements(DOM::Element const& element, ContainerQueryFeatureRequirements const& requirements)
{
    auto style = element.computed_properties();
    if (!style)
        return false;

    auto container_type = style->container_type();
    auto inline_axis_horizontal = style->writing_mode() == WritingMode::HorizontalTb;

    if (requirements.requires_width_container) {
        if (inline_axis_horizontal) {
            if (!(container_type.is_size_container || container_type.is_inline_size_container))
                return false;
        } else if (!container_type.is_size_container) {
            return false;
        }
    }

    if (requirements.requires_height_container) {
        if (inline_axis_horizontal) {
            if (!container_type.is_size_container)
                return false;
        } else if (!(container_type.is_size_container || container_type.is_inline_size_container)) {
            return false;
        }
    }

    if (requirements.requires_inline_size_container && !(container_type.is_size_container || container_type.is_inline_size_container))
        return false;

    if (requirements.requires_block_size_container && !container_type.is_size_container)
        return false;

    if (requirements.requires_scroll_state_container && !container_type.is_scroll_state_container)
        return false;

    return true;
}

// https://drafts.csswg.org/css-conditional-5/#container-rule
MatchResult ContainerQuery::evaluate(DOM::AbstractElement const& element, Optional<FlyString> const& container_name) const
{
    // If the <container-query> contains unknown or unsupported container features, no query container will be selected
    // for that <container-condition>.
    if (m_feature_requirements.has_unknown_or_unsupported_feature)
        return MatchResult::Unknown;

    // For each element, the query container to be queried is selected from among the element’s ancestor query
    // containers that are established as a valid query container for all the container features in the
    // <container-query>.
    for (auto const* container = element.element().flat_tree_parent_element(); container; container = container->flat_tree_parent_element()) {
        // The <container-name> filters the set of query containers considered to just those with a matching query
        // container name.
        if (!container_name_matches(*container, container_name))
            continue;

        if (!container_satisfies_requirements(*container, m_feature_requirements))
            continue;

        // Once an eligible query container has been selected for an element, each container feature in the
        // <container-query> is evaluated against that query container.
        return m_condition->evaluate({
            .document = &element.document(),
            .query_container = container,
        });
    }

    // If no ancestor is an eligible query container, then the container query is unknown for that element.
    return MatchResult::Unknown;
}

String ContainerQuery::to_string() const
{
    return m_condition->to_string();
}

void ContainerQuery::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Container query: (matches = {})\n", m_matches);
    m_condition->dump(builder, indent_levels + 1);
}

bool container_name_matches(DOM::Element const& element, Optional<FlyString> const& container_name)
{
    if (!container_name.has_value())
        return true;

    if (auto style = element.computed_properties())
        return style->container_name().contains_slow(*container_name);

    return false;
}

}
