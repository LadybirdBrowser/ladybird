/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContainerQuery.h"
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
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
#include <LibWeb/Painting/BoxViews.h>

namespace Web::CSS {

struct ActiveStyleQueryResolution {
    AbstractOrHypotheticalElement element;
    Utf16FlyString property_name;

    bool operator==(ActiveStyleQueryResolution const&) const = default;
};

static thread_local bool s_style_query_cycle_detected = false;

static Vector<ActiveStyleQueryResolution>& active_style_query_resolutions()
{
    static thread_local NeverDestroyed<Vector<ActiveStyleQueryResolution>> resolutions;
    return *resolutions;
}

static bool begin_style_query_resolution(AbstractOrHypotheticalElement const& element, Utf16FlyString const& property_name)
{
    auto& resolutions = active_style_query_resolutions();
    if (resolutions.contains_slow(ActiveStyleQueryResolution { element, property_name }))
        return false;
    resolutions.append({ element, property_name });
    return true;
}

static void end_style_query_resolution()
{
    active_style_query_resolutions().take_last();
}

void prepare_for_style_query_evaluation()
{
    if (active_style_query_resolutions().is_empty())
        s_style_query_cycle_detected = false;
}

bool style_query_cycle_detected()
{
    return s_style_query_cycle_detected;
}

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

enum class StyleFeatureComparison : u8 {
    Equal,
    LessThan,
    LessThanOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
};

using StyleRangeValue = Variant<PropertyNameAndID, Utf16String>;

struct StyleRange {
    StyleRangeValue left;
    StyleFeatureComparison left_comparison;
    StyleRangeValue middle;
    Optional<StyleFeatureComparison> right_comparison;
    Optional<StyleRangeValue> right;
};

struct StyleFeaturePlain {
    PropertyNameAndID property;
    Optional<Utf16String> value;
};

using EvaluatedStyleFeature = Variant<StyleFeaturePlain, StyleRange>;

static MatchResult match_result(bool value)
{
    return value ? MatchResult::True : MatchResult::False;
}

static Optional<Keyword> single_css_wide_keyword(Utf16View value)
{
    auto keyword = keyword_from_string(value.trim_ascii_whitespace());
    if (!keyword.has_value())
        return {};

    if (first_is_one_of(keyword.value(), Keyword::Initial, Keyword::Inherit, Keyword::Unset, Keyword::Revert, Keyword::RevertLayer))
        return keyword;
    return {};
}

static ColorResolutionContext fallback_color_resolution_context_for_style_query(AbstractOrHypotheticalElement const& element, ComputationContext const& computation_context)
{
    auto calculation_resolution_context = CalculationResolutionContext::from_computation_context(computation_context);
    auto color_resolution_context_for_style = [&](DOM::AbstractElement const& styled_element) {
        auto const* ui_values = styled_element.style_group<ComputedValues::InheritedUIValues>();
        auto const* text_values = styled_element.style_group<ComputedValues::InheritedTextValues>();
        VERIFY(ui_values);
        VERIFY(text_values);
        ColorResolutionContext color_resolution_context {
            .color_scheme = ui_values->color_scheme_value(),
            .current_color = text_values->color_value(),
            .calculation_resolution_context = calculation_resolution_context,
        };
        return color_resolution_context;
    };

    auto abstract_element = element.abstract_element();

    if (abstract_element.has_style())
        return color_resolution_context_for_style(abstract_element);

    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value() && parent->has_style())
        return color_resolution_context_for_style(*parent);

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

static bool compare_style_range_values(StyleRangeComparableValue const& left, StyleFeatureComparison comparison, StyleRangeComparableValue const& right)
{
    if (!style_range_type_can_compare(left, right))
        return false;

    switch (comparison) {
    case StyleFeatureComparison::Equal:
        return left.value == right.value;
    case StyleFeatureComparison::LessThan:
        return left.value < right.value;
    case StyleFeatureComparison::LessThanOrEqual:
        return left.value <= right.value;
    case StyleFeatureComparison::GreaterThan:
        return left.value > right.value;
    case StyleFeatureComparison::GreaterThanOrEqual:
        return left.value >= right.value;
    }
    VERIFY_NOT_REACHED();
}

static RefPtr<StyleValue const> parse_style_range_literal_value(DOM::Document const& document, Utf16View source)
{
    // https://drafts.csswg.org/css-conditional-5/#style-container
    // To evaluate a <style-range>:
    // 1. If <style-range-value> is a <custom-property-name>, it needs to be substituted as if the
    //    <custom-property-name> was wrapped inside a var().
    // 2. Substitute arbitrary substitution function within <style-range-value>.
    // 3. Parse <style-range-value> to <number>, <percentage>, <length>, <angle>, <time>,
    //    <frequency> or <resolution>. If this cannot be done, evaluate to false.
    auto parse_as = [&](ValueType value_type) -> RefPtr<StyleValue const> {
        auto parser = Parser::Parser::create(Parser::ParsingParams { document }, source);
        return parser.parse_entirely_as_type(value_type);
    };

    for (auto value_type : { ValueType::Number, ValueType::Length, ValueType::Percentage, ValueType::Angle, ValueType::Time, ValueType::Frequency, ValueType::Resolution }) {
        if (auto value = parse_as(value_type))
            return value;
    }

    return {};
}

static Optional<StyleRangeComparableValue> evaluate_style_range_value(StyleRangeValue const& range_value, AbstractOrHypotheticalElement const& element, DOM::Document const& document, ComputationContext const& computation_context)
{
    return range_value.visit(
        [&](PropertyNameAndID const& property) -> Optional<StyleRangeComparableValue> {
            if (!property.is_custom_property())
                return {};

            if (!begin_style_query_resolution(element, property.name())) {
                s_style_query_cycle_detected = true;
                return {};
            }
            ScopeGuard end_resolution = end_style_query_resolution;

            auto computed_value = document.style_computer().compute_value_of_custom_property(nullptr, element, property.name());

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
                auto computed_source = computed_value->is_unresolved()
                    ? computed_value->as_unresolved().token_source()
                    : computed_value->to_utf16_string(SerializationMode::ResolvedValueForReparse);
                comparable_value = parse_style_range_literal_value(document, computed_source);
                if (!comparable_value)
                    return {};
            }

            return comparable_style_range_value(comparable_value.release_nonnull(), computation_context);
        },
        [&](Utf16String const& source) -> Optional<StyleRangeComparableValue> {
            auto parsed_value = parse_style_range_literal_value(document, source);
            if (!parsed_value)
                return {};
            return comparable_style_range_value(parsed_value.release_nonnull(), computation_context);
        });
}

static MatchResult evaluate_style_range(StyleRange const& range, DOM::Document const& document, AbstractOrHypotheticalElement element)
{
    auto computation_context = element.document().style_computer().fallback_computation_context_for_custom_property(element);
    auto left = evaluate_style_range_value(range.left, element, document, computation_context);
    if (!left.has_value())
        return MatchResult::False;

    auto middle = evaluate_style_range_value(range.middle, element, document, computation_context);
    if (!middle.has_value())
        return MatchResult::False;

    if (!compare_style_range_values(left.value(), range.left_comparison, middle.value()))
        return MatchResult::False;

    if (!range.right.has_value())
        return MatchResult::True;

    auto right = evaluate_style_range_value(range.right.value(), element, document, computation_context);
    if (!right.has_value())
        return MatchResult::False;

    return match_result(compare_style_range_values(middle.value(), range.right_comparison.value(), right.value()));
}

// https://drafts.csswg.org/css-conditional-5/#style-container
static MatchResult evaluate_style_feature(EvaluatedStyleFeature const& style_feature, DOM::Document const& document, AbstractOrHypotheticalElement element)
{
    if (auto const* range = style_feature.get_pointer<StyleRange>())
        return evaluate_style_range(*range, document, element);

    auto const& feature = style_feature.get<StyleFeaturePlain>();
    auto const& property = feature.property;
    auto const& value = feature.value;

    // FIXME: Non-custom properties are valid style features, but if() is evaluated before the element's own
    //        non-custom computed values exist. Supporting these requires on-demand property resolution.
    if (!property.is_custom_property())
        return MatchResult::False;

    auto const& property_name = property.name();
    Optional<Keyword> query_css_wide_keyword;

    if (!begin_style_query_resolution(element, property_name)) {
        s_style_query_cycle_detected = true;
        return MatchResult::False;
    }
    ScopeGuard end_resolution = end_style_query_resolution;

    if (value.has_value()) {
        auto const& query_value = *value;
        if (auto css_wide_keyword = single_css_wide_keyword(query_value); css_wide_keyword.has_value()) {
            if (first_is_one_of(css_wide_keyword.value(), Keyword::Revert, Keyword::RevertLayer))
                return MatchResult::False;
            query_css_wide_keyword = css_wide_keyword;
        }
    }

    auto computed_value = document.style_computer().compute_value_of_custom_property(nullptr, element, property_name);

    auto registration = element.get_registered_custom_property(property_name);

    // FIXME: We should use the computed style that we are currently computing rather than the fallback (i.e. the previously applied style).
    auto computation_context = element.document().style_computer().fallback_computation_context_for_custom_property(element);
    auto computed_values = element.abstract_element().computed_style();
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
        return match_result(!style_values_are_equal(*comparable_computed_value, *initial_value));
    }

    auto const& query_value = *value;
    if (query_css_wide_keyword.has_value()) {
        switch (query_css_wide_keyword.value()) {
        case Keyword::Initial: {
            auto initial_value = initial_custom_property_value(registration, element.document());
            return match_result(style_values_are_equal(*comparable_computed_value, *initial_value));
        }
        case Keyword::Inherit: {
            auto inherited_value = inherited_custom_property_value(registration, element, property_name, computed_style_for_custom_property_resolution);
            return match_result(style_values_are_equal(*comparable_computed_value, *inherited_value));
        }
        case Keyword::Unset: {
            auto expected_value = !registration.has_value() || registration->inherit
                ? inherited_custom_property_value(registration, element, property_name, computed_style_for_custom_property_resolution)
                : initial_custom_property_value(registration, element.document());
            return match_result(style_values_are_equal(*comparable_computed_value, *expected_value));
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
        auto computed_value_string = (computed_value->is_unresolved()
                ? computed_value->as_unresolved().serialized_components()
                : computed_value->to_utf16_string(SerializationMode::Normal))
                                         .trim_ascii_whitespace();
        auto query_value_string = query_value.trim_ascii_whitespace();
        return match_result(computed_value_string == query_value_string);
    }

    auto parsed_query_value = Parser::parse_with_a_syntax(Parser::ParsingParams { document }, query_value, registration->syntax);
    if (parsed_query_value->is_guaranteed_invalid())
        return MatchResult::False;
    parsed_query_value = compute_registered_custom_property_value(registration.value(), move(parsed_query_value), computation_context);

    return match_result(style_values_are_equal(*comparable_computed_value, *parsed_query_value));
}

NonnullRefPtr<ContainerQuery> ContainerQuery::create(RustQueryHandle handle)
{
    return adopt_ref(*new ContainerQuery(move(handle)));
}

ContainerQuery::ContainerQuery(RustQueryHandle handle)
    : m_rust_query_handle(move(handle))
{
    auto requirements = Parser::ValueParserFFI::css_query_container_requirements(m_rust_query_handle.data());
    m_feature_requirements = {
        .requires_width_container = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_REQUIRES_WIDTH),
        .requires_height_container = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_REQUIRES_HEIGHT),
        .requires_inline_size_container = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_REQUIRES_INLINE_SIZE),
        .requires_block_size_container = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_REQUIRES_BLOCK_SIZE),
        .requires_style_container = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_REQUIRES_STYLE),
        .requires_scroll_state_container = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_REQUIRES_SCROLL_STATE),
        .has_unknown_or_unsupported_feature = static_cast<bool>(requirements & Parser::ValueParserFFI::CONTAINER_QUERY_HAS_UNKNOWN_FEATURE),
    };
}

static bool container_satisfies_requirements(DOM::Element const& element, ContainerQueryFeatureRequirements const& requirements)
{
    auto const* box_values = element.style_group<ComputedValues::BoxValues>();
    auto const* inherited_box_values = element.style_group<ComputedValues::InheritedBoxValues>();
    if (!box_values || !inherited_box_values)
        return false;

    ContainerType container_type { box_values->is_size_container, box_values->is_inline_size_container, box_values->is_scroll_state_container };
    auto inline_axis_horizontal = static_cast<WritingMode>(inherited_box_values->writing_mode) == WritingMode::HorizontalTb;

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

struct ContainerStyleEvaluationContext {
    GC::Ref<DOM::Document const> document;
    AbstractOrHypotheticalElement element;
};

static Utf16View ffi_utf16_view(Parser::ValueParserFFI::FfiUtf16View const& value)
{
    VERIFY(!value.ascii);
    VERIFY(value.utf16 || value.length == 0);
    return { reinterpret_cast<char16_t const*>(value.utf16), value.length };
}

static Optional<StyleFeatureComparison> ffi_feature_comparison(u8 comparison)
{
    if (comparison > to_underlying(StyleFeatureComparison::GreaterThanOrEqual))
        return {};
    return static_cast<StyleFeatureComparison>(comparison);
}

static Optional<StyleRangeValue> ffi_style_range_value(Parser::ValueParserFFI::FfiStyleRangeValue const& value)
{
    auto source = ffi_utf16_view(value.value);
    switch (value.kind) {
    case Parser::ValueParserFFI::FfiStyleRangeValueKind::Property: {
        auto property = PropertyNameAndID::from_name(Utf16FlyString::from_utf16(source));
        if (!property.has_value())
            return {};
        return StyleRangeValue { property.release_value() };
    }
    case Parser::ValueParserFFI::FfiStyleRangeValueKind::Components:
        return StyleRangeValue { Utf16String::from_utf16(source) };
    }
    VERIFY_NOT_REACHED();
}

static u8 evaluate_container_style_feature(void* context, Parser::ValueParserFFI::FfiContainerStyleFeature feature)
{
    VERIFY(context);
    VERIFY(feature.values || feature.value_count == 0);
    auto values = ReadonlySpan<Parser::ValueParserFFI::FfiStyleRangeValue> { feature.values, feature.value_count };
    Optional<EvaluatedStyleFeature> style_feature;
    switch (feature.kind) {
    case Parser::ValueParserFFI::FfiContainerStyleFeatureKind::Boolean: {
        if (values.size() != 1)
            return to_underlying(MatchResult::Unknown);
        auto property = ffi_style_range_value(values[0]);
        if (!property.has_value() || !property->has<PropertyNameAndID>())
            return to_underlying(MatchResult::Unknown);
        style_feature = StyleFeaturePlain {
            .property = property->get<PropertyNameAndID>(),
            .value = {},
        };
        break;
    }
    case Parser::ValueParserFFI::FfiContainerStyleFeatureKind::Plain: {
        if (values.size() != 2)
            return to_underlying(MatchResult::Unknown);
        auto property = ffi_style_range_value(values[0]);
        if (!property.has_value() || !property->has<PropertyNameAndID>())
            return to_underlying(MatchResult::Unknown);
        style_feature = StyleFeaturePlain {
            .property = property->get<PropertyNameAndID>(),
            .value = Utf16String::from_utf16(ffi_utf16_view(values[1].value)),
        };
        break;
    }
    case Parser::ValueParserFFI::FfiContainerStyleFeatureKind::Range: {
        if (values.size() < 2 || values.size() > 3)
            return to_underlying(MatchResult::Unknown);
        auto left = ffi_style_range_value(values[0]);
        auto middle = ffi_style_range_value(values[1]);
        if (!left.has_value() || !middle.has_value())
            return to_underlying(MatchResult::Unknown);
        auto left_comparison = ffi_feature_comparison(feature.first_comparison);
        if (!left_comparison.has_value())
            return to_underlying(MatchResult::Unknown);
        if (values.size() == 2) {
            style_feature = StyleRange {
                .left = left.release_value(),
                .left_comparison = left_comparison.release_value(),
                .middle = middle.release_value(),
                .right_comparison = {},
                .right = {},
            };
            break;
        }
        auto right = ffi_style_range_value(values[2]);
        if (!right.has_value())
            return to_underlying(MatchResult::Unknown);
        auto right_comparison = ffi_feature_comparison(feature.second_comparison);
        if (!right_comparison.has_value())
            return to_underlying(MatchResult::Unknown);
        style_feature = StyleRange {
            .left = left.release_value(),
            .left_comparison = left_comparison.release_value(),
            .middle = middle.release_value(),
            .right_comparison = right_comparison.release_value(),
            .right = right.release_value(),
        };
        break;
    }
    default:
        return to_underlying(MatchResult::Unknown);
    }

    auto& evaluation_context = *static_cast<ContainerStyleEvaluationContext*>(context);
    if (!style_feature.has_value())
        return to_underlying(MatchResult::Unknown);
    return to_underlying(evaluate_style_feature(*style_feature, *evaluation_context.document, evaluation_context.element));
}

MatchResult evaluate_style_query(RustQueryHandle const& handle, AbstractOrHypotheticalElement element)
{
    ContainerStyleEvaluationContext style_context { element.document(), element };
    Parser::ValueParserFFI::FfiContainerFacts facts {
        .container_available = true,
        .size_available = false,
        .width = 0,
        .height = 0,
        .inline_axis_horizontal = false,
        .length_resolution_context = nullptr,
        .style_context = &style_context,
        .evaluate_style_feature = evaluate_container_style_feature,
    };
    auto result = Parser::ValueParserFFI::css_query_evaluate_container(handle.data(), facts);
    VERIFY(result <= to_underlying(MatchResult::Unknown));
    return static_cast<MatchResult>(result);
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

        // A style feature asks about the container's own computed style, so the container has to know
        // that a style change on it is a change for something under it. Nothing else says so: the
        // dependency is recorded on the element that asked, which is not the element that moves.
        // The value the query compares against is resolved too, and that resolution can read the
        // root - `style(--length: calc(1rem * 10))` moves when the root font size does - so the root
        // is named as well.
        // A size feature asks about the container's own box, and the scan a resize does for the
        // dependents under it starts from the same fact: whether anything ever asked.
        if (m_feature_requirements.contains_size_feature())
            const_cast<DOM::Element&>(*container).set_is_size_query_container();

        if (m_feature_requirements.contains_style_feature()) {
            const_cast<DOM::Element&>(*container).set_is_style_query_container();
            if (auto* root = element.document().document_element())
                root->set_is_style_query_container();
        }

        Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_resolution_context;
        Parser::ValueParserFFI::FfiContainerFacts facts {
            .container_available = true,
            .size_available = false,
            .width = 0,
            .height = 0,
            .inline_axis_horizontal = false,
            .length_resolution_context = nullptr,
            .style_context = nullptr,
            .evaluate_style_feature = evaluate_container_style_feature,
        };
        if (auto const* layout_node = container->unsafe_layout_node(); layout_node && Painting::has_committed_box(*layout_node)) {
            facts.size_available = true;
            facts.width = Painting::content_width(*layout_node).to_double();
            facts.height = Painting::content_height(*layout_node).to_double();
            facts.inline_axis_horizontal = layout_node->writing_mode() == WritingMode::HorizontalTb;
            auto computation_context = ComputationContext {
                .length_resolution_context = Length::ResolutionContext::for_layout_node(*layout_node),
                .abstract_element = DOM::AbstractElement { *container },
            };
            length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
                computation_context.length_resolution_context, all_container_relative_length_units_mask);
            facts.length_resolution_context = &*length_resolution_context;
        } else if (!container->document().layout_is_up_to_date()) {
            const_cast<DOM::Document&>(container->document()).set_needs_container_query_evaluation_after_layout(*container);
        }

        ContainerStyleEvaluationContext style_context { element.document(), DOM::AbstractElement { *container } };
        facts.style_context = &style_context;
        auto result = Parser::ValueParserFFI::css_query_evaluate_container(m_rust_query_handle.data(), facts);
        VERIFY(result <= to_underlying(MatchResult::Unknown));
        return static_cast<MatchResult>(result);
    }

    // If no ancestor is an eligible query container, then the container query is unknown for that element.
    return MatchResult::Unknown;
}

Utf16String ContainerQuery::to_string() const
{
    Utf16String serialized;
    auto set_serialized_query = [](void* context, u16 const* code_units, size_t length) {
        *static_cast<Utf16String*>(context) = Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(code_units), length });
    };
    VERIFY(Parser::ValueParserFFI::css_query_serialize_condition(m_rust_query_handle.data(), &serialized, set_serialized_query));
    return serialized;
}

void ContainerQuery::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Container query: `{}`\n", to_string());
}

bool container_name_matches(DOM::Element const& element, Optional<Utf16FlyString> const& container_name)
{
    if (!container_name.has_value())
        return true;

    if (auto const* values = element.style_group<ComputedValues::BoxValues>()) {
        for (size_t i = 0; i < values->container_name.length; ++i) {
            if (Utf16FlyString::from_raw(values->container_name.pointer[i].raw) == *container_name)
                return true;
        }
    }

    return false;
}

}
