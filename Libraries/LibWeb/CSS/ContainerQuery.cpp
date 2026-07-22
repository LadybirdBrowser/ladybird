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
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::CSS {

Optional<SizeFeatureID> size_feature_id_from_string(Utf16View name)
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

static FeatureValue size_feature_value_for_query_container(SizeFeatureID id, Painting::Paintable const& paintable_box)
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

NonnullOwnPtr<StyleQueryFunction> StyleQueryFunction::create(NonnullOwnPtr<BooleanExpression>&& query)
{
    return adopt_own(*new StyleQueryFunction(move(query)));
}

StyleQueryFunction::StyleQueryFunction(NonnullOwnPtr<BooleanExpression>&& query)
    : m_query(move(query))
{
}

MatchResult StyleQueryFunction::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    return m_query->evaluate(context);
}

void StyleQueryFunction::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    m_query->collect_container_query_feature_requirements(requirements);
}

void StyleQueryFunction::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append("style("_utf16);
    m_query->serialize_to(builder);
    builder.append(")"_utf16);
}

void StyleQueryFunction::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("StyleQueryFunction:\n"sv);
    m_query->dump(builder, indent_levels + 1);
}

NonnullOwnPtr<StyleFeature> StyleFeature::create_boolean(PropertyNameAndID property)
{
    return adopt_own(*new StyleFeature(StyleFeaturePlain {
        .property = move(property),
        .value = {},
        .original_value_text = {},
    }));
}

NonnullOwnPtr<StyleFeature> StyleFeature::create_plain(PropertyNameAndID property, Vector<Parser::ComponentValue> value, Optional<String> original_value_text)
{
    return adopt_own(*new StyleFeature(StyleFeaturePlain {
        .property = move(property),
        .value = move(value),
        .original_value_text = move(original_value_text),
    }));
}

NonnullOwnPtr<StyleFeature> StyleFeature::create_range(StyleRangeValue left, FeatureComparison comparison, StyleRangeValue right)
{
    return adopt_own(*new StyleFeature(StyleRange {
        .left = left,
        .left_comparison = comparison,
        .middle = right,
    }));
}

NonnullOwnPtr<StyleFeature> StyleFeature::create_range(StyleRangeValue left, FeatureComparison left_comparison, StyleRangeValue middle, FeatureComparison right_comparison, StyleRangeValue right)
{
    return adopt_own(*new StyleFeature(StyleRange {
        .left = left,
        .left_comparison = left_comparison,
        .middle = middle,
        .right_comparison = right_comparison,
        .right = right,
    }));
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

static ColorResolutionContext fallback_color_resolution_context_for_style_query(AbstractOrHypotheticalElement const& element, ComputationContext const& computation_context)
{
    auto calculation_resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
    auto color_resolution_context_for_style = [&](ComputedValues const& style) {
        ColorResolutionContext color_resolution_context {
            .color_scheme = style.color_scheme(),
            .current_color = style.color(),
            .calculation_resolution_context = calculation_resolution_context,
        };
        return color_resolution_context;
    };

    auto abstract_element = element.abstract_element();

    if (auto const* style = abstract_element.computed_values())
        return color_resolution_context_for_style(*style);

    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value() && parent->computed_values())
        return color_resolution_context_for_style(*parent->computed_values());

    return {
        .color_scheme = element.document().page().preferred_color_scheme(),
        .current_color = InitialValues::color(),
        .calculation_resolution_context = calculation_resolution_context,
    };
}

enum class StyleRangeNumericType : u8 {
    Number,
    Length,
    Percentage,
    Angle,
    Time,
    Frequency,
    Resolution,
};

struct StyleRangeComparableValue {
    StyleRangeNumericType type;
    double value;
};

static Optional<StyleRangeComparableValue> comparable_style_range_value(NonnullRefPtr<StyleValue const> value, ComputationContext const& computation_context)
{
    auto comparable = value->absolutized(computation_context);
    if (comparable->is_calculated()) {
        auto calculation_resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
        auto resolved = comparable->as_calculated().resolve_as_style_value(calculation_resolution_context);
        if (!resolved)
            return {};
        comparable = resolved.release_nonnull();
    }

    if (comparable->is_integer())
        return StyleRangeComparableValue { StyleRangeNumericType::Number, static_cast<double>(comparable->as_integer().integer()) };
    if (comparable->is_number())
        return StyleRangeComparableValue { StyleRangeNumericType::Number, comparable->as_number().number() };
    if (comparable->is_length())
        return StyleRangeComparableValue { StyleRangeNumericType::Length, Length::from_style_value(comparable, {}).absolute_length_to_px_without_rounding() };
    if (comparable->is_percentage())
        return StyleRangeComparableValue { StyleRangeNumericType::Percentage, comparable->as_percentage().percentage().value() };
    if (comparable->is_angle())
        return StyleRangeComparableValue { StyleRangeNumericType::Angle, comparable->as_angle().angle().to_degrees() };
    if (comparable->is_time())
        return StyleRangeComparableValue { StyleRangeNumericType::Time, comparable->as_time().time().to_seconds() };
    if (comparable->is_frequency())
        return StyleRangeComparableValue { StyleRangeNumericType::Frequency, comparable->as_frequency().frequency().to_hertz() };
    if (comparable->is_resolution())
        return StyleRangeComparableValue { StyleRangeNumericType::Resolution, comparable->as_resolution().resolution().to_dots_per_pixel() };
    return {};
}

static bool style_range_type_can_compare(StyleRangeComparableValue const& left, StyleRangeComparableValue const& right)
{
    if (left.type == right.type)
        return true;

    auto is_unitless_zero = [](auto value) {
        return value.type == StyleRangeNumericType::Number && value.value == 0;
    };

    auto is_dimension = [](auto type) {
        return type != StyleRangeNumericType::Number && type != StyleRangeNumericType::Percentage;
    };

    return (is_unitless_zero(left) && is_dimension(right.type))
        || (is_unitless_zero(right) && is_dimension(left.type));
}

static bool compare_style_range_values(StyleRangeComparableValue const& left, FeatureComparison comparison, StyleRangeComparableValue const& right)
{
    if (!style_range_type_can_compare(left, right))
        return false;

    switch (comparison) {
    case FeatureComparison::Equal:
        return left.value == right.value;
    case FeatureComparison::LessThan:
        return left.value < right.value;
    case FeatureComparison::LessThanOrEqual:
        return left.value <= right.value;
    case FeatureComparison::GreaterThan:
        return left.value > right.value;
    case FeatureComparison::GreaterThanOrEqual:
        return left.value >= right.value;
    }
    VERIFY_NOT_REACHED();
}

static RefPtr<StyleValue const> parse_style_range_literal_value(DOM::Document const& document, ReadonlySpan<Parser::ComponentValue> tokens)
{
    if (Parser::contains_guaranteed_invalid_value(tokens))
        return {};

    // https://drafts.csswg.org/css-conditional-5/#style-container
    // To evaluate a <style-range>:
    // 1. If <style-range-value> is a <custom-property-name>, it needs to be substituted as if the
    //    <custom-property-name> was wrapped inside a var().
    // 2. Substitute arbitrary substitution function within <style-range-value>.
    // 3. Parse <style-range-value> to <number>, <percentage>, <length>, <angle>, <time>,
    //    <frequency> or <resolution>. If this cannot be done, evaluate to false.
    auto parse_as = [&](ValueType value_type) -> RefPtr<StyleValue const> {
        auto serialized_tokens = serialize_a_series_of_component_values(tokens);
        auto parser = Parser::Parser::create(Parser::ParsingParams { document }, serialized_tokens);
        return parser.parse_entirely_as_type(value_type);
    };

    for (auto value_type : { ValueType::Number, ValueType::Length, ValueType::Percentage, ValueType::Angle, ValueType::Time, ValueType::Frequency, ValueType::Resolution }) {
        if (auto value = parse_as(value_type))
            return value;
    }

    return {};
}

static Optional<StyleRangeComparableValue> evaluate_style_range_value(StyleFeature::StyleRangeValue const& range_value, AbstractOrHypotheticalElement const& element, DOM::Document const& document, ComputationContext const& computation_context, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts, bool* did_evaluate_attr_tainted_style_query)
{
    return range_value.visit(
        [&](PropertyNameAndID const& property) -> Optional<StyleRangeComparableValue> {
            if (!property.is_custom_property())
                return {};

            if (guarded_contexts.has_value()) {
                if (guarded_contexts->mark_existing_as_cyclic(Parser::SubstitutionContext { Parser::PropertySubstitutionContextDependency::create(property.name().to_utf16_string(), element) }))
                    return {};
            }

            auto computed_value = document.style_computer().compute_value_of_custom_property(nullptr, element, property.name(), guarded_contexts);
            auto computed_tokens = computed_value->tokenize();
            if (did_evaluate_attr_tainted_style_query && Parser::contains_attr_tainted_value(computed_tokens))
                *did_evaluate_attr_tainted_style_query = true;

            if (computed_value->is_guaranteed_invalid())
                return {};

            auto registration = document.get_registered_custom_property(property.name());
            RefPtr<StyleValue const> comparable_value = computed_value;
            if (registration.has_value() && computed_value->is_unresolved() && computed_value->as_unresolved().contains_attr_tainted_values()) {
                if (auto cached_parsed_value = computed_value->as_unresolved().parsed_value()) {
                    comparable_value = compute_registered_custom_property_value(registration.value(), cached_parsed_value.release_nonnull(), computation_context);
                } else {
                    VERIFY(registration->syntax->type() == Parser::SyntaxNode::NodeType::Universal);
                }
            } else if (!registration.has_value() || computed_value->is_unresolved()) {
                comparable_value = parse_style_range_literal_value(document, computed_tokens);
                if (!comparable_value)
                    return {};
            }

            return comparable_style_range_value(comparable_value.release_nonnull(), computation_context);
        },
        [&](Vector<Parser::ComponentValue> const& tokens) -> Optional<StyleRangeComparableValue> {
            if (did_evaluate_attr_tainted_style_query && Parser::contains_attr_tainted_value(tokens))
                *did_evaluate_attr_tainted_style_query = true;

            auto parsed_value = parse_style_range_literal_value(document, tokens);
            if (!parsed_value)
                return {};
            return comparable_style_range_value(parsed_value.release_nonnull(), computation_context);
        });
}

static MatchResult evaluate_style_range(StyleFeature::StyleRange const& range, BooleanExpressionEvaluationContext const& context)
{
    if (!context.style_query_element.has_value())
        return MatchResult::Unknown;

    auto element = context.style_query_element.value();
    auto const& document = context.document ? *context.document : element.document();
    auto computation_context = element.document().style_computer().fallback_computation_context_for_custom_property(element);
    Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts;
    if (context.guarded_contexts.has_value())
        guarded_contexts = const_cast<Parser::GuardedSubstitutionContexts&>(context.guarded_contexts.value());

    auto left = evaluate_style_range_value(range.left, element, document, computation_context, guarded_contexts, context.did_evaluate_attr_tainted_style_query);
    if (!left.has_value())
        return MatchResult::False;

    auto middle = evaluate_style_range_value(range.middle, element, document, computation_context, guarded_contexts, context.did_evaluate_attr_tainted_style_query);
    if (!middle.has_value())
        return MatchResult::False;

    if (!compare_style_range_values(left.value(), range.left_comparison, middle.value()))
        return MatchResult::False;

    if (!range.right.has_value())
        return MatchResult::True;

    auto right = evaluate_style_range_value(range.right.value(), element, document, computation_context, guarded_contexts, context.did_evaluate_attr_tainted_style_query);
    if (!right.has_value())
        return MatchResult::False;

    return as_match_result(compare_style_range_values(middle.value(), range.right_comparison.value(), right.value()));
}

// https://drafts.csswg.org/css-conditional-5/#style-container
MatchResult StyleFeature::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    if (!context.style_query_element.has_value())
        return MatchResult::Unknown;

    if (auto const* range = m_feature.get_pointer<StyleRange>())
        return evaluate_style_range(*range, context);

    auto const& feature = m_feature.get<StyleFeaturePlain>();
    auto const& property = feature.property;
    auto const& value = feature.value;

    // FIXME: Non-custom properties are valid style features, but if() is evaluated before the element's own
    //        non-custom computed values exist. Supporting these requires on-demand property resolution.
    if (!property.is_custom_property())
        return MatchResult::False;

    auto element = context.style_query_element.value();
    auto const& document = context.document ? *context.document : element.document();
    auto const& property_name = property.name();
    Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts;
    if (context.guarded_contexts.has_value())
        guarded_contexts = const_cast<Parser::GuardedSubstitutionContexts&>(context.guarded_contexts.value());
    Optional<Keyword> query_css_wide_keyword;

    if (guarded_contexts.has_value()) {
        if (guarded_contexts->mark_existing_as_cyclic(Parser::SubstitutionContext { Parser::PropertySubstitutionContextDependency::create(property.name().to_utf16_string(), element) }))
            return MatchResult::False;
    }

    if (value.has_value()) {
        auto const& query_value = *value;
        if (Parser::contains_guaranteed_invalid_value(query_value))
            return MatchResult::False;

        if (auto css_wide_keyword = single_css_wide_keyword(query_value); css_wide_keyword.has_value()) {
            if (first_is_one_of(css_wide_keyword.value(), Keyword::Revert, Keyword::RevertLayer))
                return MatchResult::False;
            query_css_wide_keyword = css_wide_keyword;
        }
    }

    auto computed_value = document.style_computer().compute_value_of_custom_property(nullptr, element, property_name, guarded_contexts);
    auto computed_tokens = computed_value->tokenize();
    if (context.did_evaluate_attr_tainted_style_query) {
        if (Parser::contains_attr_tainted_value(computed_tokens))
            *context.did_evaluate_attr_tainted_style_query = true;
    }

    auto registration = element.get_registered_custom_property(property_name);

    // FIXME: We should use the computed style that we are currently computing rather than the fallback (i.e. the previously applied style).
    auto computation_context = element.document().style_computer().fallback_computation_context_for_custom_property(element);
    auto const* computed_values = element.abstract_element().computed_values();
    auto const* computed_style_for_custom_property_resolution = computed_values ? document.style_computer().reconstruct_computed_properties(*computed_values).ptr() : nullptr;

    auto color_resolution_context = fallback_color_resolution_context_for_style_query(element, computation_context);
    auto comparable_computed_value = computed_value;
    if (registration.has_value() && computed_value->is_unresolved() && computed_value->as_unresolved().contains_attr_tainted_values()) {
        if (auto cached_parsed_value = computed_value->as_unresolved().parsed_value()) {
            comparable_computed_value = compute_registered_custom_property_value(registration.value(), cached_parsed_value.release_nonnull(), computation_context);
        } else {
            VERIFY(registration->syntax->type() == Parser::SyntaxNode::NodeType::Universal);
        }
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
    if (!value.has_value()) {
        auto initial_value = initial_custom_property_value(registration, element.document());
        return as_match_result(!style_values_are_equal(*comparable_computed_value, *initial_value));
    }

    auto const& query_value = *value;
    if (query_css_wide_keyword.has_value()) {
        switch (query_css_wide_keyword.value()) {
        case Keyword::Initial: {
            auto initial_value = initial_custom_property_value(registration, element.document());
            return as_match_result(style_values_are_equal(*comparable_computed_value, *initial_value));
        }
        case Keyword::Inherit: {
            auto inherited_value = inherited_custom_property_value(registration, element, property_name, computed_style_for_custom_property_resolution, guarded_contexts);
            return as_match_result(style_values_are_equal(*comparable_computed_value, *inherited_value));
        }
        case Keyword::Unset: {
            auto expected_value = !registration.has_value() || registration->inherit
                ? inherited_custom_property_value(registration, element, property_name, computed_style_for_custom_property_resolution, guarded_contexts)
                : initial_custom_property_value(registration, element.document());
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
        auto computed_value_string = serialize_a_series_of_component_values(computed_tokens).trim_ascii_whitespace();
        auto query_value_string = serialize_a_series_of_component_values(query_value).trim_ascii_whitespace();
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

static Utf16String serialize_style_range_value_to_utf16(StyleFeature::StyleRangeValue const& value)
{
    return value.visit(
        [](PropertyNameAndID const& property) {
            return property.to_utf16_string();
        },
        [](Vector<Parser::ComponentValue> const& component_values) {
            return serialize_a_series_of_component_values(component_values);
        });
}

void StyleFeature::serialize_to(Utf16StringBuilder& builder) const
{
    m_feature.visit(
        [&](StyleFeaturePlain const& feature) {
            auto serialized_property = feature.property.to_utf16_string();
            builder.append(serialized_property.utf16_view());
            if (!feature.value.has_value())
                return;
            builder.append_ascii(": "sv);
            if (feature.original_value_text.has_value()) {
                builder.append(feature.original_value_text->bytes_as_string_view());
            } else {
                auto serialized_value = serialize_a_series_of_component_values(feature.value.value());
                builder.append(serialized_value.utf16_view());
            }
        },
        [&](StyleRange const& range) {
            auto serialized_left = serialize_style_range_value_to_utf16(range.left);
            builder.append(serialized_left.utf16_view());
            builder.append_ascii(' ');
            builder.append_ascii(string_from_feature_comparison(range.left_comparison));
            builder.append_ascii(' ');
            auto serialized_middle = serialize_style_range_value_to_utf16(range.middle);
            builder.append(serialized_middle.utf16_view());
            if (!range.right.has_value())
                return;
            builder.append_ascii(' ');
            builder.append_ascii(string_from_feature_comparison(range.right_comparison.value()));
            builder.append_ascii(' ');
            auto serialized_right = serialize_style_range_value_to_utf16(range.right.value());
            builder.append(serialized_right.utf16_view());
        });
}

void StyleFeature::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("StyleFeature: {}\n", to_string().to_utf8());
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
    auto style = element.computed_values();
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
MatchResult ContainerQuery::evaluate(DOM::AbstractElement const& element, Optional<Utf16FlyString> const& container_name) const
{
    // If the <container-query> contains unknown or unsupported container features, no query container will be selected
    // for that <container-condition>.
    if (m_feature_requirements.has_unknown_or_unsupported_feature)
        return MatchResult::Unknown;

    // For each element, the query container to be queried is selected from among the element’s ancestor query
    // containers that are established as a valid query container for all the container features in the
    // <container-query>.
    for (auto const* container = element.flat_tree_parent_element(); container; container = container->flat_tree_parent_element()) {
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
            .style_query_element = DOM::AbstractElement { *container },
        });
    }

    // If no ancestor is an eligible query container, then the container query is unknown for that element.
    return MatchResult::Unknown;
}

Utf16String ContainerQuery::to_string() const
{
    return m_condition->to_string();
}

void ContainerQuery::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Container query: (matches = {})\n", m_matches);
    m_condition->dump(builder, indent_levels + 1);
}

bool container_name_matches(DOM::Element const& element, Optional<Utf16FlyString> const& container_name)
{
    if (!container_name.has_value())
        return true;

    if (auto style = element.computed_values())
        return style->container_name().contains_slow(*container_name);

    return false;
}

}
