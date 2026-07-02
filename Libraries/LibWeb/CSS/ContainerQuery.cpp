/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContainerQuery.h"
#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Page/Page.h>
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

NonnullOwnPtr<StyleFeature> StyleFeature::create_boolean(PropertyNameAndID property)
{
    return adopt_own(*new StyleFeature(move(property), {}));
}

NonnullOwnPtr<StyleFeature> StyleFeature::create_plain(PropertyNameAndID property, Vector<Parser::ComponentValue> value)
{
    return adopt_own(*new StyleFeature(move(property), move(value)));
}

static Optional<Keyword> single_css_wide_keyword(ReadonlySpan<Parser::ComponentValue> value)
{
    Parser::TokenStream tokens { value };
    tokens.discard_whitespace();
    if (!tokens.has_next_token())
        return {};

    auto const& token = tokens.consume_a_token();
    tokens.discard_whitespace();
    if (tokens.has_next_token() || !token.is(Parser::Token::Type::Ident))
        return {};

    auto keyword = keyword_from_string(token.token().ident());
    if (!keyword.has_value())
        return {};

    if (first_is_one_of(keyword.value(), Keyword::Initial, Keyword::Inherit, Keyword::Unset, Keyword::Revert, Keyword::RevertLayer))
        return keyword;
    return {};
}

static NonnullRefPtr<StyleValue const> inherited_custom_property_value(DOM::AbstractElement const& element, Utf16FlyString const& name, DOM::Document const& document, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts)
{
    if (auto parent = element.element_to_inherit_style_from(); parent.has_value())
        return StyleComputer::compute_value_of_custom_property(parent.value(), name, guarded_contexts);
    return document.custom_property_initial_value(name);
}

static ColorResolutionContext fallback_color_resolution_context_for_style_query(DOM::AbstractElement const& element, ComputationContext const& computation_context)
{
    auto calculation_resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
    auto color_resolution_context_for_style = [&](ComputedProperties const& style) {
        auto const& document = element.document();
        auto color_scheme = style.color_scheme(document.page().preferred_color_scheme(), document.supported_color_schemes());
        ColorResolutionContext color_resolution_context {
            .color_scheme = color_scheme,
            .current_color = InitialValues::color(),
            .calculation_resolution_context = calculation_resolution_context,
        };
        color_resolution_context.current_color = style.color(PropertyID::Color, color_resolution_context);
        return color_resolution_context;
    };

    if (auto const* style = element.computed_properties())
        return color_resolution_context_for_style(*style);

    if (auto parent = element.element_to_inherit_style_from(); parent.has_value() && parent->computed_properties())
        return color_resolution_context_for_style(*parent->computed_properties());

    return {
        .color_scheme = element.document().page().preferred_color_scheme(),
        .current_color = InitialValues::color(),
        .calculation_resolution_context = calculation_resolution_context,
    };
}

// https://drafts.csswg.org/css-conditional-5/#style-container
MatchResult StyleFeature::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    if (!context.style_query_element.has_value())
        return MatchResult::Unknown;

    // FIXME: Non-custom properties are valid style features, but if() is evaluated before the element's own
    //        non-custom computed values exist. Supporting these requires on-demand property resolution.
    if (!m_property.is_custom_property())
        return MatchResult::False;

    auto element = context.style_query_element.value();
    auto const& document = context.document ? *context.document : element.document();
    auto const& property_name = m_property.name();
    Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts;
    if (context.guarded_contexts.has_value())
        guarded_contexts = const_cast<Parser::GuardedSubstitutionContexts&>(context.guarded_contexts.value());
    Optional<Keyword> query_css_wide_keyword;

    if (guarded_contexts.has_value()) {
        if (guarded_contexts->mark_existing_as_cyclic({ Parser::SubstitutionContext::DependencyType::Property, m_property.to_string() }))
            return MatchResult::False;
    }

    if (m_value.has_value()) {
        auto const& query_value = m_value.value();
        if (Parser::contains_guaranteed_invalid_value(query_value))
            return MatchResult::False;

        if (auto css_wide_keyword = single_css_wide_keyword(query_value); css_wide_keyword.has_value()) {
            if (first_is_one_of(css_wide_keyword.value(), Keyword::Revert, Keyword::RevertLayer))
                return MatchResult::False;
            query_css_wide_keyword = css_wide_keyword;
        }
    }

    auto computed_value = StyleComputer::compute_value_of_custom_property(element, property_name, guarded_contexts);
    auto computed_tokens = computed_value->tokenize();
    if (context.did_evaluate_attr_tainted_style_query) {
        if (Parser::contains_attr_tainted_value(computed_tokens))
            *context.did_evaluate_attr_tainted_style_query = true;
    }

    auto registration = document.get_registered_custom_property(property_name);
    auto computation_context = element.document().style_computer().fallback_computation_context_for_custom_property(element);
    auto color_resolution_context = fallback_color_resolution_context_for_style_query(element, computation_context);
    auto comparable_computed_value = computed_value;
    if (registration.has_value() && computed_value->is_unresolved() && computed_value->as_unresolved().contains_attr_tainted_values()) {
        // Registered custom properties with attr-tainted values are wrapped as unresolved values so tokenize()
        // preserves the taint. Reparse that wrapper here so style queries can still compare the typed value.
        // FIXME: Store the attr-tainted flag in a more sensible way so we don't have to do this!
        auto parsed_computed_value = Parser::parse_with_a_syntax(Parser::ParsingParams { document }, computed_tokens, registration->syntax);
        if (!parsed_computed_value->is_guaranteed_invalid())
            comparable_computed_value = compute_registered_custom_property_value(registration.value(), move(parsed_computed_value), computation_context);
    }

    // A <style-feature-plain> evaluates to true if the computed value of the given property on the query container
    // matches the given value (which is also computed with respect to the query container), and false otherwise.
    auto style_values_are_equal = [&](StyleValue const& left, StyleValue const& right) -> bool {
        if (left.is_guaranteed_invalid() || right.is_guaranteed_invalid())
            return left.equals(right);

        auto left_absolutized = left.absolutized(computation_context);
        auto right_absolutized = right.absolutized(computation_context);

        auto left_color = left_absolutized->to_color(color_resolution_context);
        auto right_color = right_absolutized->to_color(color_resolution_context);
        if (left_color.has_value() || right_color.has_value())
            return left_color.has_value() && right_color.has_value() && left_color.value() == right_color.value();

        auto calculation_resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
        auto left_resolved = left_absolutized->is_calculated()
            ? left_absolutized->as_calculated().resolve_as_style_value(calculation_resolution_context)
            : nullptr;
        auto right_resolved = right_absolutized->is_calculated()
            ? right_absolutized->as_calculated().resolve_as_style_value(calculation_resolution_context)
            : nullptr;
        if (left_resolved || right_resolved) {
            auto const& comparable_left = left_resolved ? *left_resolved : *left_absolutized;
            auto const& comparable_right = right_resolved ? *right_resolved : *right_absolutized;
            return comparable_left.equals(comparable_right);
        }

        return left_absolutized->equals(*right_absolutized);
    };

    // A style feature without a value (<style-feature-boolean>) evaluates to true if the computed value is different
    // from the initial value for the given property.
    if (!m_value.has_value()) {
        auto initial_value = document.custom_property_initial_value(property_name);
        return as_match_result(!style_values_are_equal(*comparable_computed_value, *initial_value));
    }

    auto const& query_value = m_value.value();
    if (query_css_wide_keyword.has_value()) {
        switch (query_css_wide_keyword.value()) {
        case Keyword::Initial: {
            auto initial_value = document.custom_property_initial_value(property_name);
            return as_match_result(style_values_are_equal(*comparable_computed_value, *initial_value));
        }
        case Keyword::Inherit: {
            auto inherited_value = inherited_custom_property_value(element, property_name, document, guarded_contexts);
            return as_match_result(style_values_are_equal(*comparable_computed_value, *inherited_value));
        }
        case Keyword::Unset: {
            auto expected_value = !registration.has_value() || registration->inherit
                ? inherited_custom_property_value(element, property_name, document, guarded_contexts)
                : document.custom_property_initial_value(property_name);
            return as_match_result(style_values_are_equal(*comparable_computed_value, *expected_value));
        }
        case Keyword::Revert:
        case Keyword::RevertLayer:
            VERIFY_NOT_REACHED();
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (computed_value->is_guaranteed_invalid())
        return MatchResult::False;

    if (!registration.has_value()) {
        auto computed_value_string = MUST(serialize_a_series_of_component_values(computed_tokens).trim_ascii_whitespace());
        auto query_value_string = MUST(serialize_a_series_of_component_values(query_value).trim_ascii_whitespace());
        return as_match_result(computed_value_string == query_value_string);
    }

    auto parsed_query_value = Parser::parse_with_a_syntax(Parser::ParsingParams { document }, query_value, registration->syntax);
    if (parsed_query_value->is_guaranteed_invalid())
        return MatchResult::False;
    parsed_query_value = compute_registered_custom_property_value(registration.value(), move(parsed_query_value), computation_context);

    return as_match_result(style_values_are_equal(*comparable_computed_value, *parsed_query_value));
}

void StyleFeature::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    requirements.requires_style_container = true;
}

String StyleFeature::to_string() const
{
    if (!m_value.has_value())
        return m_property.to_string();
    return MUST(String::formatted("{}: {}", m_property.to_string(), serialize_a_series_of_component_values(m_value.value())));
}

void StyleFeature::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("StyleFeature: {}\n", to_string());
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
