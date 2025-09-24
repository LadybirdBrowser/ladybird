/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CalculatedStyleValue.h"
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/CSSMathClamp.h>
#include <LibWeb/CSS/CSSMathInvert.h>
#include <LibWeb/CSS/CSSMathMax.h>
#include <LibWeb/CSS/CSSMathMin.h>
#include <LibWeb/CSS/CSSMathNegate.h>
#include <LibWeb/CSS/CSSMathProduct.h>
#include <LibWeb/CSS/CSSMathSum.h>
#include <LibWeb/CSS/CSSNumericArray.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>

namespace Web::CSS {

static Optional<NumericType> add_the_types(Vector<NonnullRefPtr<CalculationNode const>> const& nodes)
{
    Optional<NumericType> left_type;
    for (auto const& value : nodes) {
        auto right_type = value->numeric_type();
        if (!right_type.has_value())
            return {};

        if (left_type.has_value()) {
            left_type = left_type->added_to(right_type.value());
        } else {
            left_type = right_type;
        }

        if (!left_type.has_value())
            return {};
    }

    return left_type;
}

static Optional<NumericType> add_the_types(CalculationNode const& a, CalculationNode const& b)
{
    auto a_type = a.numeric_type();
    auto b_type = b.numeric_type();
    if (!a_type.has_value() || !b_type.has_value())
        return {};
    return a_type->added_to(*b_type);
}

static Optional<NumericType> add_the_types(CalculationNode const& a, CalculationNode const& b, CalculationNode const& c)
{
    auto a_type = a.numeric_type();
    auto b_type = b.numeric_type();
    auto c_type = c.numeric_type();
    if (!a_type.has_value() || !b_type.has_value() || !c_type.has_value())
        return {};

    auto a_and_b_type = a_type->added_to(*b_type);
    if (!a_and_b_type.has_value())
        return {};

    return a_and_b_type->added_to(*c_type);
}

static Optional<NumericType> multiply_the_types(Vector<NonnullRefPtr<CalculationNode const>> const& nodes)
{
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    Optional<NumericType> left_type;
    for (auto const& value : nodes) {
        auto right_type = value->numeric_type();
        if (!right_type.has_value())
            return {};

        if (left_type.has_value()) {
            left_type = left_type->multiplied_by(right_type.value());
        } else {
            left_type = right_type;
        }

        if (!left_type.has_value())
            return {};
    }

    return left_type;
}

template<typename T>
static NonnullRefPtr<CalculationNode const> simplify_children_vector(T const& original, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    Vector<NonnullRefPtr<CalculationNode const>> simplified_children;
    simplified_children.ensure_capacity(original.children().size());

    bool any_changed = false;
    for (auto const& child : original.children()) {
        auto simplified = simplify_a_calculation_tree(child, context, resolution_context);
        if (simplified != child)
            any_changed = true;
        simplified_children.append(move(simplified));
    }

    if (any_changed)
        return T::create(move(simplified_children));
    return original;
}

template<typename T>
static NonnullRefPtr<CalculationNode const> simplify_child(T const& original, NonnullRefPtr<CalculationNode const> const& child, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    auto simplified = simplify_a_calculation_tree(child, context, resolution_context);
    if (simplified != child)
        return T::create(move(simplified));
    return original;
}

template<typename T>
static NonnullRefPtr<CalculationNode const> simplify_2_children(T const& original, NonnullRefPtr<CalculationNode const> const& child_1, NonnullRefPtr<CalculationNode const> const& child_2, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    auto simplified_1 = simplify_a_calculation_tree(child_1, context, resolution_context);
    auto simplified_2 = simplify_a_calculation_tree(child_2, context, resolution_context);
    if (simplified_1 != child_1 || simplified_2 != child_2)
        return T::create(move(simplified_1), move(simplified_2));
    return original;
}

static CalculationNode::NumericValue clamp_and_censor_numeric_value(NumericCalculationNode const& node, CalculationContext const& context)
{
    auto value = node.value();

    Optional<AcceptedTypeRange> accepted_range = value.visit(
        [&](Number const&) { return context.resolve_numbers_as_integers ? context.accepted_type_ranges.get(ValueType::Integer) : context.accepted_type_ranges.get(ValueType::Number); },
        [&](Angle const&) { return context.accepted_type_ranges.get(ValueType::Angle); },
        [&](Flex const&) { return context.accepted_type_ranges.get(ValueType::Flex); },
        [&](Frequency const&) { return context.accepted_type_ranges.get(ValueType::Frequency); },
        [&](Length const&) { return context.accepted_type_ranges.get(ValueType::Length); },
        [&](Percentage const&) { return context.accepted_type_ranges.get(ValueType::Percentage); },
        [&](Resolution const&) { return context.accepted_type_ranges.get(ValueType::Resolution); },
        [&](Time const&) { return context.accepted_type_ranges.get(ValueType::Time); });

    if (!accepted_range.has_value()) {
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Calculation context missing accepted type range {}", node.numeric_type());
        // FIXME: Min and max values for Integer should be based on i32 rather than float
        accepted_range = { AK::NumericLimits<float>::lowest(), AK::NumericLimits<float>::max() };
    }

    auto clamp_and_censor = [&](double value, double min, double max) {
        // https://drafts.csswg.org/css-values/#calc-ieee
        // NaN does not escape a top-level calculation; it’s censored into a zero value.
        if (isnan(value))
            value = 0;

        // https://drafts.csswg.org/css-values/#calc-range
        // the value resulting from a top-level calculation must be clamped to the range allowed in the target context.
        // Clamping is performed on computed values to the extent possible, and also on used values if computation was
        // unable to sufficiently simplify the expression to allow range-checking.
        return clamp(value, min, max);
    };

    return value.visit(
        [&](Number const& value) -> CalculationNode::NumericValue {
            return Number { value.type(), clamp_and_censor(context.resolve_numbers_as_integers ? value.integer_value() : value.value(), accepted_range->min, accepted_range->max) };
        },
        [&](Angle const& value) -> CalculationNode::NumericValue {
            return Angle { clamp_and_censor(value.raw_value(), accepted_range->min, accepted_range->max), value.unit() };
        },
        [&](Flex const& value) -> CalculationNode::NumericValue {
            return Flex { clamp_and_censor(value.raw_value(), accepted_range->min, accepted_range->max), value.unit() };
        },
        [&](Frequency const& value) -> CalculationNode::NumericValue {
            return Frequency { clamp_and_censor(value.raw_value(), accepted_range->min, accepted_range->max), value.unit() };
        },
        [&](Length const& value) -> CalculationNode::NumericValue {
            return Length { clamp_and_censor(value.raw_value(), accepted_range->min, accepted_range->max), value.unit() };
        },
        [&](Percentage const& value) -> CalculationNode::NumericValue {
            return Percentage { clamp_and_censor(value.value(), accepted_range->min, accepted_range->max) };
        },
        [&](Resolution const& value) -> CalculationNode::NumericValue {
            return Resolution { clamp_and_censor(value.raw_value(), accepted_range->min, accepted_range->max), value.unit() };
        },
        [&](Time const& value) -> CalculationNode::NumericValue {
            return Time { clamp_and_censor(value.raw_value(), accepted_range->min, accepted_range->max), value.unit() };
        });
}

static GC::Ptr<CSSNumericArray> reify_children(JS::Realm& realm, ReadonlySpan<NonnullRefPtr<CalculationNode const>> children)
{
    GC::RootVector<GC::Ref<CSSNumericValue>> reified_children { realm.heap() };
    for (auto const& child : children) {
        auto reified_child = child->reify(realm);
        if (!reified_child)
            return nullptr;
        reified_children.append(reified_child.as_nonnull());
    }
    return CSSNumericArray::create(realm, move(reified_children));
}

static String serialize_a_calculation_tree(CalculationNode const&, CalculationContext const&, SerializationMode);

// https://drafts.csswg.org/css-values-4/#serialize-a-math-function
static String serialize_a_math_function(CalculationNode const& fn, CalculationContext const& context, SerializationMode serialization_mode)
{
    // To serialize a math function fn:

    // 1. If the root of the calculation tree fn represents is a numeric value (number, percentage, or dimension), and
    //    the serialization being produced is of a computed value or later, then clamp the value to the range allowed
    //    for its context (if necessary), then serialize the value as normal and return the result.
    if (fn.type() == CalculationNode::Type::Numeric && serialization_mode == SerializationMode::ResolvedValue) {
        auto clamped_value = clamp_and_censor_numeric_value(static_cast<NumericCalculationNode const&>(fn), context);

        return clamped_value.visit([&](auto const& value) { return value.to_string(serialization_mode); });
    }

    // 2. If fn represents an infinite or NaN value:
    if (fn.type() == CalculationNode::Type::Numeric) {
        auto const& numeric_node = static_cast<NumericCalculationNode const&>(fn);
        if (auto infinite_or_nan = numeric_node.infinite_or_nan_value(); infinite_or_nan.has_value()) {
            // 1. Let s be the string "calc(".
            StringBuilder builder;
            builder.append("calc("sv);

            // 2. Serialize the keyword infinity, -infinity, or NaN, as appropriate to represent the value, and append it to s.
            switch (infinite_or_nan.value()) {
            case NonFiniteValue::Infinity:
                builder.append("infinity"sv);
                break;
            case NonFiniteValue::NegativeInfinity:
                builder.append("-infinity"sv);
                break;
            case NonFiniteValue::NaN:
                builder.append("NaN"sv);
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            // 3. If fn’s type is anything other than «[ ]» (empty, representing a <number>), append " * " to s.
            //    Create a numeric value in the canonical unit for fn’s type (such as px for <length>), with a value of 1.
            //    Serialize this numeric value and append it to s.
            if (!numeric_node.value().has<Number>()) {
                numeric_node.value().visit(
                    [&builder](Angle const&) { builder.append(" * 1deg"sv); },
                    [&builder](Flex const&) { builder.append(" * 1fr"sv); },
                    [&builder](Frequency const&) { builder.append(" * 1hz"sv); },
                    [&builder](Length const&) { builder.append(" * 1px"sv); },
                    [](Number const&) { VERIFY_NOT_REACHED(); },
                    [&builder](Percentage const&) { builder.append(" * 1%"sv); },
                    [&builder](Resolution const&) { builder.append(" * 1dppx"sv); },
                    [&builder](Time const&) { builder.append(" * 1s"sv); });
            }

            // 4. Append ")" to s, then return it.
            builder.append(')');
            return builder.to_string_without_validation();
        }
    }

    // 3. If the calculation tree’s root node is a numeric value, or a calc-operator node, let s be a string initially
    //    containing "calc(".
    //    Otherwise, let s be a string initially containing the name of the root node, lowercased (such as "sin" or
    //    "max"), followed by a "(" (open parenthesis).
    StringBuilder builder;
    if (fn.type() == CalculationNode::Type::Numeric || fn.is_calc_operator_node()) {
        builder.append("calc("sv);
    } else {
        builder.appendff("{}(", fn.name());
    }

    // 4. For each child of the root node, serialize the calculation tree.
    //    If a result of this serialization starts with a "(" (open parenthesis) and ends with a ")" (close parenthesis),
    //    remove those characters from the result.
    //    Concatenate all of the results using ", " (comma followed by space), then append the result to s.

    auto serialized_tree_without_parentheses = [&](CalculationNode const& tree) {
        auto tree_serialized = serialize_a_calculation_tree(tree, context, serialization_mode);
        if (tree_serialized.starts_with('(') && tree_serialized.ends_with(')')) {
            tree_serialized = MUST(tree_serialized.substring_from_byte_offset_with_shared_superstring(1, tree_serialized.byte_count() - 2));
        }
        return tree_serialized;
    };

    // Spec issue: https://github.com/w3c/csswg-drafts/issues/11783
    //             The three AD-HOCs in this step are mentioned there.
    // AD-HOC: Numeric nodes have no children and should serialize directly.
    // AD-HOC: calc-operator nodes should also serialize directly, instead of separating their children by commas.#
    if (fn.type() == CalculationNode::Type::Numeric || fn.is_calc_operator_node()) {
        builder.append(serialized_tree_without_parentheses(fn));
    } else {
        Vector<String> serialized_children;
        // AD-HOC: For `clamp()`, the first child is a <rounding-strategy>, which is incompatible with "serialize a calculation tree".
        //         So, we serialize it directly first, and hope for the best.
        if (fn.type() == CalculationNode::Type::Round) {
            auto rounding_strategy = static_cast<RoundCalculationNode const&>(fn).rounding_strategy();
            serialized_children.append(MUST(String::from_utf8(CSS::to_string(rounding_strategy))));
        }
        for (auto const& child : fn.children()) {
            serialized_children.append(serialized_tree_without_parentheses(child));
        }
        builder.join(", "sv, serialized_children);
    }

    // 5. Append ")" (close parenthesis) to s.
    builder.append(')');

    // 6. Return s.
    return builder.to_string_without_validation();
}

// https://drafts.csswg.org/css-values-4/#sort-a-calculations-children
static Vector<NonnullRefPtr<CalculationNode const>> sort_a_calculations_children(Vector<NonnullRefPtr<CalculationNode const>> nodes)
{
    // 1. Let ret be an empty list.
    Vector<NonnullRefPtr<CalculationNode const>> ret;

    // 2. If nodes contains a number, remove it from nodes and append it to ret.
    auto index_of_number = nodes.find_first_index_if([](NonnullRefPtr<CalculationNode const> const& node) {
        if (node->type() != CalculationNode::Type::Numeric)
            return false;
        return static_cast<NumericCalculationNode const&>(*node).value().has<Number>();
    });
    if (index_of_number.has_value()) {
        ret.append(nodes.take(*index_of_number));
    }

    // 3. If nodes contains a percentage, remove it from nodes and append it to ret.
    auto index_of_percentage = nodes.find_first_index_if([](NonnullRefPtr<CalculationNode const> const& node) {
        if (node->type() != CalculationNode::Type::Numeric)
            return false;
        return static_cast<NumericCalculationNode const&>(*node).value().has<Percentage>();
    });
    if (index_of_percentage.has_value()) {
        ret.append(nodes.take(*index_of_percentage));
    }

    // 4. If nodes contains any dimensions, remove them from nodes, sort them by their units, ordered ASCII
    //    case-insensitively, and append them to ret.
    Vector<NonnullRefPtr<CalculationNode const>> dimensions;
    dimensions.ensure_capacity(nodes.size());

    auto next_dimension_index = [&nodes]() {
        return nodes.find_first_index_if([](NonnullRefPtr<CalculationNode const> const& node) {
            if (node->type() != CalculationNode::Type::Numeric)
                return false;
            return static_cast<NumericCalculationNode const&>(*node).value().visit(
                [](Number const&) { return false; },
                [](Percentage const&) { return false; },
                [](auto const&) { return true; });
        });
    };

    for (auto index_of_dimension = next_dimension_index(); index_of_dimension.has_value(); index_of_dimension = next_dimension_index()) {
        dimensions.append(nodes.take(*index_of_dimension));
    }

    quick_sort(dimensions, [](NonnullRefPtr<CalculationNode const> const& a, NonnullRefPtr<CalculationNode const> const& b) {
        auto get_unit = [](NonnullRefPtr<CalculationNode const> const& node) -> FlyString {
            auto const& numeric_node = static_cast<NumericCalculationNode const&>(*node);
            return numeric_node.value().visit(
                [](Number const&) -> FlyString { VERIFY_NOT_REACHED(); },
                [](Percentage const&) -> FlyString { VERIFY_NOT_REACHED(); },
                [](auto const& dimension) -> FlyString { return dimension.unit_name(); });
        };

        auto a_unit = get_unit(a);
        auto b_unit = get_unit(b);
        // NOTE: Our unit name strings are always lowercase, so we don't have to do anything special for a case-insensitive match.
        return a_unit < b_unit;
    });
    ret.extend(dimensions);

    // 5. If nodes still contains any items, append them to ret in the same order.
    if (!nodes.is_empty())
        ret.extend(nodes);

    // 6. Return ret.
    return ret;
}

// https://drafts.csswg.org/css-values-4/#serialize-a-calculation-tree
static String serialize_a_calculation_tree(CalculationNode const& root, CalculationContext const& context, SerializationMode serialization_mode)
{
    // 1. Let root be the root node of the calculation tree.
    // NOTE: Already the case.

    // 2. If root is a numeric value, or a non-math function, serialize root per the normal rules for it and return the result.
    // FIXME: Support non-math functions in calculation trees.
    if (root.type() == CalculationNode::Type::Numeric)
        return static_cast<NumericCalculationNode const&>(root).value_to_string();

    // 3. If root is anything but a Sum, Negate, Product, or Invert node, serialize a math function for the function
    //    corresponding to the node type, treating the node’s children as the function’s comma-separated calculation
    //    arguments, and return the result.
    if (!first_is_one_of(root.type(), CalculationNode::Type::Sum, CalculationNode::Type::Product, CalculationNode::Type::Negate, CalculationNode::Type::Invert)) {
        return serialize_a_math_function(root, context, serialization_mode);
    }

    // 4. If root is a Negate node, let s be a string initially containing "(-1 * ".
    if (root.type() == CalculationNode::Type::Negate) {
        StringBuilder builder;
        builder.append("(-1 * "sv);

        // Serialize root’s child, and append it to s.
        builder.append(serialize_a_calculation_tree(root.children().first(), context, serialization_mode));

        // Append ")" to s, then return it.
        builder.append(')');
        return builder.to_string_without_validation();
    }

    // 5. If root is an Invert node, let s be a string initially containing "(1 / ".
    if (root.type() == CalculationNode::Type::Invert) {
        StringBuilder builder;
        builder.append("(1 / "sv);

        // Serialize root’s child, and append it to s.
        builder.append(serialize_a_calculation_tree(root.children().first(), context, serialization_mode));

        // Append ")" to s, then return it.
        builder.append(')');
        return builder.to_string_without_validation();
    }

    // 6. If root is a Sum node, let s be a string initially containing "(".
    if (root.type() == CalculationNode::Type::Sum) {
        StringBuilder builder;
        builder.append('(');

        auto sorted_children = sort_a_calculations_children(root.children());

        // Serialize root’s first child, and append it to s.
        builder.append(serialize_a_calculation_tree(sorted_children.first(), context, serialization_mode));

        // For each child of root beyond the first:
        for (auto i = 1u; i < sorted_children.size(); ++i) {
            auto& child = *sorted_children[i];

            // 1. If child is a Negate node, append " - " to s, then serialize the Negate’s child and append the
            //    result to s.
            if (child.type() == CalculationNode::Type::Negate) {
                builder.append(" - "sv);
                builder.append(serialize_a_calculation_tree(static_cast<NegateCalculationNode const&>(child).child(), context, serialization_mode));
            }

            // 2. If child is a negative numeric value, append " - " to s, then serialize the negation of child as
            //    normal and append the result to s.
            else if (child.type() == CalculationNode::Type::Numeric && static_cast<NumericCalculationNode const&>(child).is_negative()) {
                auto const& numeric_node = static_cast<NumericCalculationNode const&>(child);
                builder.append(" - "sv);
                builder.append(serialize_a_calculation_tree(numeric_node.negated(context), context, serialization_mode));
            }

            // 3. Otherwise, append " + " to s, then serialize child and append the result to s.
            else {
                builder.append(" + "sv);
                builder.append(serialize_a_calculation_tree(child, context, serialization_mode));
            }
        }

        // Finally, append ")" to s and return it.
        builder.append(')');
        return builder.to_string_without_validation();
    }

    // 7. If root is a Product node, let s be a string initially containing "(".
    if (root.type() == CalculationNode::Type::Product) {
        StringBuilder builder;
        builder.append('(');

        auto sorted_children = sort_a_calculations_children(root.children());

        // Serialize root’s first child, and append it to s.
        builder.append(serialize_a_calculation_tree(sorted_children.first(), context, serialization_mode));

        // For each child of root beyond the first:
        for (auto i = 1u; i < sorted_children.size(); ++i) {
            auto& child = *sorted_children[i];

            // 1. If child is an Invert node, append " / " to s, then serialize the Invert’s child and append the result to s.
            if (child.type() == CalculationNode::Type::Invert) {
                builder.append(" / "sv);
                builder.append(serialize_a_calculation_tree(static_cast<InvertCalculationNode const&>(child).child(), context, serialization_mode));
            }

            // 2. Otherwise, append " * " to s, then serialize child and append the result to s.
            else {
                builder.append(" * "sv);
                builder.append(serialize_a_calculation_tree(child, context, serialization_mode));
            }
        }

        // Finally, append ")" to s and return it.
        builder.append(')');
        return builder.to_string_without_validation();
    }

    VERIFY_NOT_REACHED();
}

CalculationNode::CalculationNode(Type type, Optional<NumericType> numeric_type)
    : m_type(type)
    , m_numeric_type(move(numeric_type))
{
}

CalculationNode::~CalculationNode() = default;

StringView CalculationNode::name() const
{
    switch (m_type) {
    case Type::Min:
        return "min"sv;
    case Type::Max:
        return "max"sv;
    case Type::Clamp:
        return "clamp"sv;
    case Type::Abs:
        return "abs"sv;
    case Type::Sign:
        return "sign"sv;
    case Type::Sin:
        return "sin"sv;
    case Type::Cos:
        return "cos"sv;
    case Type::Tan:
        return "tan"sv;
    case Type::Asin:
        return "asin"sv;
    case Type::Acos:
        return "acos"sv;
    case Type::Atan:
        return "atan"sv;
    case Type::Atan2:
        return "atan2"sv;
    case Type::Pow:
        return "pow"sv;
    case Type::Sqrt:
        return "sqrt"sv;
    case Type::Hypot:
        return "hypot"sv;
    case Type::Log:
        return "log"sv;
    case Type::Exp:
        return "exp"sv;
    case Type::Round:
        return "round"sv;
    case Type::Mod:
        return "mod"sv;
    case Type::Rem:
        return "rem"sv;
    case Type::Numeric:
    case Type::Sum:
    case Type::Product:
    case Type::Negate:
    case Type::Invert:
        return "calc"sv;
    }
    VERIFY_NOT_REACHED();
}

static NumericType numeric_type_from_calculated_style_value(CalculatedStyleValue::CalculationResult::Value const& value, CalculationContext const& context)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // Anything else is a terminal value, whose type is determined based on its CSS type.
    // (Unless otherwise specified, the type’s associated percent hint is null.)
    return value.visit(
        [](Number const&) {
            // -> <number>
            // -> <integer>
            //    the type is «[ ]» (empty map)
            return NumericType {};
        },
        [](Length const&) {
            // -> <length>
            //    the type is «[ "length" → 1 ]»
            return NumericType { NumericType::BaseType::Length, 1 };
        },
        [](Angle const&) {
            // -> <angle>
            //    the type is «[ "angle" → 1 ]»
            return NumericType { NumericType::BaseType::Angle, 1 };
        },
        [](Time const&) {
            // -> <time>
            //    the type is «[ "time" → 1 ]»
            return NumericType { NumericType::BaseType::Time, 1 };
        },
        [](Frequency const&) {
            // -> <frequency>
            //    the type is «[ "frequency" → 1 ]»
            return NumericType { NumericType::BaseType::Frequency, 1 };
        },
        [](Resolution const&) {
            // -> <resolution>
            //    the type is «[ "resolution" → 1 ]»
            return NumericType { NumericType::BaseType::Resolution, 1 };
        },
        [](Flex const&) {
            // -> <flex>
            //    the type is «[ "flex" → 1 ]»
            return NumericType { NumericType::BaseType::Flex, 1 };
        },
        // NOTE: <calc-constant> is a separate node type. (FIXME: Should it be?)
        [&context](Percentage const&) {
            // -> <percentage>
            //    If, in the context in which the math function containing this calculation is placed,
            //    <percentage>s are resolved relative to another type of value (such as in width,
            //    where <percentage> is resolved against a <length>), and that other type is not <number>,
            //    the type is determined as the other type, but with a percent hint set to that other type.
            if (context.percentages_resolve_as.has_value() && context.percentages_resolve_as != ValueType::Number && context.percentages_resolve_as != ValueType::Percentage) {
                auto base_type = NumericType::base_type_from_value_type(*context.percentages_resolve_as);
                VERIFY(base_type.has_value());
                auto result = NumericType { base_type.value(), 1 };
                result.set_percent_hint(base_type);
                return result;
            }

            //    Otherwise, the type is «[ "percent" → 1 ]», with a percent hint of "percent".
            auto result = NumericType { NumericType::BaseType::Percent, 1 };
            // FIXME: Setting the percent hint to "percent" causes us to fail tests.
            // result.set_percent_hint(NumericType::BaseType::Percent);
            return result;
        });
}

NonnullRefPtr<NumericCalculationNode const> NumericCalculationNode::create(NumericValue value, CalculationContext const& context)
{
    auto numeric_type = numeric_type_from_calculated_style_value(value, context);
    return adopt_ref(*new (nothrow) NumericCalculationNode(move(value), numeric_type));
}

RefPtr<NumericCalculationNode const> NumericCalculationNode::from_keyword(Keyword keyword, CalculationContext const& context)
{
    switch (keyword) {
    case Keyword::E:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-e
        return create(Number { Number::Type::Number, AK::E<double> }, context);
    case Keyword::Pi:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-pi
        return create(Number { Number::Type::Number, AK::Pi<double> }, context);
    case Keyword::Infinity:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-infinity
        return create(Number { Number::Type::Number, AK::Infinity<double> }, context);
    case Keyword::NegativeInfinity:
        // https://drafts.csswg.org/css-values-4/#valdef-calc--infinity
        return create(Number { Number::Type::Number, -AK::Infinity<double> }, context);
    case Keyword::Nan:
        // https://drafts.csswg.org/css-values-4/#valdef-calc-nan
        return create(Number { Number::Type::Number, AK::NaN<double> }, context);
    default:
        return nullptr;
    }
}

NumericCalculationNode::NumericCalculationNode(NumericValue value, NumericType numeric_type)
    : CalculationNode(Type::Numeric, move(numeric_type))
    , m_value(move(value))
{
}

NumericCalculationNode::~NumericCalculationNode() = default;

String NumericCalculationNode::value_to_string() const
{
    return m_value.visit([](auto& value) { return value.to_string(); });
}

bool NumericCalculationNode::contains_percentage() const
{
    return m_value.has<Percentage>();
}

bool NumericCalculationNode::is_in_canonical_unit() const
{
    return m_value.visit(
        [](Angle const& angle) { return angle.unit() == AngleUnit::Deg; },
        [](Flex const& flex) { return flex.unit() == FlexUnit::Fr; },
        [](Frequency const& frequency) { return frequency.unit() == FrequencyUnit::Hz; },
        [](Length const& length) { return length.unit() == LengthUnit::Px; },
        [](Number const&) { return true; },
        [](Percentage const&) { return true; },
        [](Resolution const& resolution) { return resolution.unit() == ResolutionUnit::Dppx; },
        [](Time const& time) { return time.unit() == TimeUnit::S; });
}

static Optional<CalculatedStyleValue::CalculationResult> try_get_value_with_canonical_unit(CalculationNode const& child, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    if (child.type() != CalculationNode::Type::Numeric)
        return {};
    auto const& numeric_child = as<NumericCalculationNode>(child);

    // Can't run with non-canonical units or unresolved percentages.
    // We've already attempted to resolve both in with_simplified_children().
    if (!numeric_child.is_in_canonical_unit()
        || (numeric_child.value().has<Percentage>() && context.percentages_resolve_as.has_value()))
        return {};

    // Can't run if a child has an invalid type.
    if (!numeric_child.numeric_type().has_value())
        return {};

    return CalculatedStyleValue::CalculationResult::from_value(numeric_child.value(), resolution_context, numeric_child.numeric_type());
}

static Optional<double> try_get_number(CalculationNode const& child)
{
    if (child.type() != CalculationNode::Type::Numeric)
        return {};
    auto const* maybe_number = as<NumericCalculationNode>(child).value().get_pointer<Number>();
    if (!maybe_number)
        return {};
    return maybe_number->value();
}

RefPtr<StyleValue const> NumericCalculationNode::to_style_value(CalculationContext const& context) const
{
    // TODO: Clamp values to the range allowed by the context.
    return m_value.visit(
        [&](Number const& number) -> RefPtr<StyleValue const> {
            // FIXME: Returning infinity or NaN as a NumberStyleValue isn't valid.
            //        This is a temporary fix until value-clamping is implemented here.
            //        In future, we can remove these two lines and return NonnullRefPtr again.
            if (!isfinite(number.value()))
                return nullptr;

            if (context.resolve_numbers_as_integers)
                return IntegerStyleValue::create(llround(number.value()));
            return NumberStyleValue::create(number.value());
        },
        [](Angle const& angle) -> RefPtr<StyleValue const> { return AngleStyleValue::create(angle); },
        [](Flex const& flex) -> RefPtr<StyleValue const> { return FlexStyleValue::create(flex); },
        [](Frequency const& frequency) -> RefPtr<StyleValue const> { return FrequencyStyleValue::create(frequency); },
        [](Length const& length) -> RefPtr<StyleValue const> { return LengthStyleValue::create(length); },
        [](Percentage const& percentage) -> RefPtr<StyleValue const> { return PercentageStyleValue::create(percentage); },
        [](Resolution const& resolution) -> RefPtr<StyleValue const> { return ResolutionStyleValue::create(resolution); },
        [](Time const& time) -> RefPtr<StyleValue const> { return TimeStyleValue::create(time); });
}

Optional<NonFiniteValue> NumericCalculationNode::infinite_or_nan_value() const
{
    auto raw_value = m_value.visit(
        [](Number const& number) { return number.value(); },
        [](Percentage const& percentage) { return percentage.as_fraction(); },
        [](auto const& dimension) { return dimension.raw_value(); });

    if (isnan(raw_value))
        return NonFiniteValue::NaN;
    if (!isfinite(raw_value)) {
        if (raw_value < 0)
            return NonFiniteValue::NegativeInfinity;
        return NonFiniteValue::Infinity;
    }

    return {};
}

bool NumericCalculationNode::is_negative() const
{
    return m_value.visit(
        [&](Number const& number) { return number.value() < 0; },
        [](Percentage const& percentage) { return percentage.value() < 0; },
        [](auto const& dimension) { return dimension.raw_value() < 0; });
}

NonnullRefPtr<NumericCalculationNode const> NumericCalculationNode::negated(CalculationContext const& context) const
{
    return value().visit(
        [&](Percentage const& percentage) {
            return create(Percentage(-percentage.value()), context);
        },
        [&](Number const& number) {
            return create(Number(number.type(), -number.value()), context);
        },
        [&]<typename T>(T const& value) {
            return create(T(-value.raw_value(), value.unit()), context);
        });
}

void NumericCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NUMERIC({})\n", "", indent, m_value.visit([](auto& it) { return it.to_string(); }));
}

bool NumericCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value == static_cast<NumericCalculationNode const&>(other).m_value;
}

GC::Ptr<CSSNumericValue> NumericCalculationNode::reify(JS::Realm& realm) const
{
    return m_value.visit(
        [&realm](Number const& number) { return CSSUnitValue::create(realm, number.value(), "number"_fly_string); },
        [&realm](Percentage const& percentage) { return CSSUnitValue::create(realm, percentage.value(), "percent"_fly_string); },
        [&realm](auto const& dimension) { return CSSUnitValue::create(realm, dimension.raw_value(), dimension.unit_name()); });
}

NonnullRefPtr<SumCalculationNode const> SumCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // At a + or - sub-expression, attempt to add the types of the left and right arguments.
    // If this returns failure, the entire calculation’s type is failure.
    // Otherwise, the sub-expression’s type is the returned type.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) SumCalculationNode(move(values), move(numeric_type)));
}

SumCalculationNode::SumCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Sum, move(numeric_type))
    , m_values(move(values))
{
    VERIFY(!m_values.is_empty());
}

SumCalculationNode::~SumCalculationNode() = default;

bool SumCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

NonnullRefPtr<CalculationNode const> SumCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

void SumCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SUM:\n", "", indent);
    for (auto const& item : m_values)
        item->dump(builder, indent + 2);
}

bool SumCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<SumCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<SumCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> SumCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathSum::create(realm, numeric_type().value(), reified_children.as_nonnull());
}

NonnullRefPtr<ProductCalculationNode const> ProductCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // At a * sub-expression, multiply the types of the left and right arguments.
    // The sub-expression’s type is the returned result.
    auto numeric_type = multiply_the_types(values);
    return adopt_ref(*new (nothrow) ProductCalculationNode(move(values), move(numeric_type)));
}

ProductCalculationNode::ProductCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Product, move(numeric_type))
    , m_values(move(values))
{
    VERIFY(!m_values.is_empty());
}

ProductCalculationNode::~ProductCalculationNode() = default;

bool ProductCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }
    return false;
}

NonnullRefPtr<CalculationNode const> ProductCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

void ProductCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}PRODUCT:\n", "", indent);
    for (auto const& item : m_values)
        item->dump(builder, indent + 2);
}

bool ProductCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<ProductCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<ProductCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> ProductCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathProduct::create(realm, numeric_type().value(), reified_children.as_nonnull());
}

NonnullRefPtr<NegateCalculationNode const> NegateCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) NegateCalculationNode(move(value)));
}

NegateCalculationNode::NegateCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // NOTE: `- foo` doesn't change the type
    : CalculationNode(Type::Negate, value->numeric_type())
    , m_value(move(value))
{
}

NegateCalculationNode::~NegateCalculationNode() = default;

bool NegateCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> NegateCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

void NegateCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}NEGATE:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool NegateCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<NegateCalculationNode const&>(other).m_value);
}

GC::Ptr<CSSNumericValue> NegateCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_value = m_value->reify(realm);
    if (!reified_value)
        return nullptr;
    return CSSMathNegate::create(realm, numeric_type().value(), reified_value.as_nonnull());
}

NonnullRefPtr<InvertCalculationNode const> InvertCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // At a / sub-expression, let left type be the result of finding the types of its left argument,
    // and right type be the result of finding the types of its right argument and then inverting it.
    // The sub-expression’s type is the result of multiplying the left type and right type.
    // NOTE: An InvertCalculationNode only represents the right argument here, and the multiplication
    //       is handled in the parent ProductCalculationNode.
    auto numeric_type = value->numeric_type().map([](auto& it) { return it.inverted(); });
    return adopt_ref(*new (nothrow) InvertCalculationNode(move(value), move(numeric_type)));
}

InvertCalculationNode::InvertCalculationNode(NonnullRefPtr<CalculationNode const> value, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Invert, move(numeric_type))
    , m_value(move(value))
{
}

InvertCalculationNode::~InvertCalculationNode() = default;

bool InvertCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> InvertCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

void InvertCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}INVERT:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool InvertCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<InvertCalculationNode const&>(other).m_value);
}

GC::Ptr<CSSNumericValue> InvertCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_value = m_value->reify(realm);
    if (!reified_value)
        return nullptr;
    return CSSMathInvert::create(realm, numeric_type().value(), reified_value.as_nonnull());
}

NonnullRefPtr<MinCalculationNode const> MinCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) MinCalculationNode(move(values), move(numeric_type)));
}

MinCalculationNode::MinCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Min, move(numeric_type))
    , m_values(move(values))
{
}

MinCalculationNode::~MinCalculationNode() = default;

bool MinCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

NonnullRefPtr<CalculationNode const> MinCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-min
enum class MinOrMax {
    Min,
    Max,
};
static Optional<CalculatedStyleValue::CalculationResult> run_min_or_max_operation_if_possible(Vector<NonnullRefPtr<CalculationNode const>> const& children, CalculationContext const& context, CalculationResolutionContext const& resolution_context, MinOrMax min_or_max)
{
    // The min() or max() functions contain one or more comma-separated calculations, and represent the smallest
    // (most negative) or largest (most positive) of them, respectively.
    Optional<CalculatedStyleValue::CalculationResult> result;
    for (auto const& child : children) {
        auto child_value = try_get_value_with_canonical_unit(child, context, resolution_context);
        if (!child_value.has_value())
            return {};

        if (!result.has_value()) {
            result = child_value.release_value();
        } else {
            auto consistent_type = result->type()->consistent_type(child_value->type().value());
            if (!consistent_type.has_value())
                return {};

            // https://drafts.csswg.org/css-values-4/#calc-ieee
            // Any operation with at least one NaN argument produces NaN.
            if (isnan(child_value->value()) || isnan(result->value())) {
                result = CalculatedStyleValue::CalculationResult { AK::NaN<double>, consistent_type };
                continue;
            }

            if (min_or_max == MinOrMax::Min) {
                if (child_value->value() < result->value()) {
                    result = CalculatedStyleValue::CalculationResult { child_value->value(), consistent_type };
                } else {
                    result = CalculatedStyleValue::CalculationResult { result->value(), consistent_type };
                }
            } else {
                if (child_value->value() > result->value()) {
                    result = CalculatedStyleValue::CalculationResult { child_value->value(), consistent_type };
                } else {
                    result = CalculatedStyleValue::CalculationResult { result->value(), consistent_type };
                }
            }
        }
    }
    return result;
}

// https://drafts.csswg.org/css-values-4/#funcdef-min
Optional<CalculatedStyleValue::CalculationResult> MinCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_min_or_max_operation_if_possible(m_values, context, resolution_context, MinOrMax::Min);
}

void MinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MIN:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

bool MinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<MinCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<MinCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> MinCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathMin::create(realm, numeric_type().value(), reified_children.as_nonnull());
}

NonnullRefPtr<MaxCalculationNode const> MaxCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) MaxCalculationNode(move(values), move(numeric_type)));
}

MaxCalculationNode::MaxCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Max, move(numeric_type))
    , m_values(move(values))
{
}

MaxCalculationNode::~MaxCalculationNode() = default;

bool MaxCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

NonnullRefPtr<CalculationNode const> MaxCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-max
Optional<CalculatedStyleValue::CalculationResult> MaxCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_min_or_max_operation_if_possible(m_values, context, resolution_context, MinOrMax::Max);
}

void MaxCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MAX:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

bool MaxCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    if (m_values.size() != static_cast<MaxCalculationNode const&>(other).m_values.size())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<MaxCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

GC::Ptr<CSSNumericValue> MaxCalculationNode::reify(JS::Realm& realm) const
{
    auto reified_children = reify_children(realm, m_values);
    if (!reified_children)
        return nullptr;
    return CSSMathMax::create(realm, numeric_type().value(), reified_children.as_nonnull());
}

NonnullRefPtr<ClampCalculationNode const> ClampCalculationNode::create(NonnullRefPtr<CalculationNode const> min, NonnullRefPtr<CalculationNode const> center, NonnullRefPtr<CalculationNode const> max)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*min, *center, *max);
    return adopt_ref(*new (nothrow) ClampCalculationNode(move(min), move(center), move(max), move(numeric_type)));
}

ClampCalculationNode::ClampCalculationNode(NonnullRefPtr<CalculationNode const> min, NonnullRefPtr<CalculationNode const> center, NonnullRefPtr<CalculationNode const> max, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Clamp, move(numeric_type))
    , m_min_value(move(min))
    , m_center_value(move(center))
    , m_max_value(move(max))
{
}

ClampCalculationNode::~ClampCalculationNode() = default;

bool ClampCalculationNode::contains_percentage() const
{
    return m_min_value->contains_percentage() || m_center_value->contains_percentage() || m_max_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> ClampCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    auto simplified_min = simplify_a_calculation_tree(m_min_value, context, resolution_context);
    auto simplified_center = simplify_a_calculation_tree(m_center_value, context, resolution_context);
    auto simplified_max = simplify_a_calculation_tree(m_max_value, context, resolution_context);
    if (simplified_min != m_min_value || simplified_center != m_center_value || simplified_max != m_max_value)
        return create(move(simplified_min), move(simplified_center), move(simplified_max));
    return *this;
}

// https://drafts.csswg.org/css-values-4/#funcdef-clamp
Optional<CalculatedStyleValue::CalculationResult> ClampCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The clamp() function takes three calculations — a minimum value, a central value, and a maximum value — and
    // represents its central calculation, clamped according to its min and max calculations, favoring the min
    // calculation if it conflicts with the max. (That is, given clamp(MIN, VAL, MAX), it represents exactly the
    // same value as max(MIN, min(VAL, MAX))).
    //
    // Either the min or max calculations (or even both) can instead be the keyword none, which indicates the value
    // is not clamped from that side. (That is, clamp(MIN, VAL, none) is equivalent to max(MIN, VAL), clamp(none,
    // VAL, MAX) is equivalent to min(VAL, MAX), and clamp(none, VAL, none) is equivalent to just calc(VAL).)
    //
    // For all three functions, the argument calculations can resolve to any <number>, <dimension>, or <percentage>,
    // but must have a consistent type or else the function is invalid; the result’s type will be the consistent type.

    auto min_result = try_get_value_with_canonical_unit(m_min_value, context, resolution_context);
    if (!min_result.has_value())
        return {};

    auto center_result = try_get_value_with_canonical_unit(m_center_value, context, resolution_context);
    if (!center_result.has_value())
        return {};

    auto max_result = try_get_value_with_canonical_unit(m_max_value, context, resolution_context);
    if (!max_result.has_value())
        return {};

    auto consistent_type = min_result->type()->consistent_type(center_result->type().value()).map([&](auto& it) { return it.consistent_type(max_result->type().value()); });
    if (!consistent_type.has_value())
        return {};

    // https://drafts.csswg.org/css-values-4/#calc-ieee
    // Any operation with at least one NaN argument produces NaN.
    if (isnan(min_result->value()) || isnan(center_result->value()) || isnan(max_result->value()))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, consistent_type.release_value() };

    auto chosen_value = max(min_result->value(), min(center_result->value(), max_result->value()));
    return CalculatedStyleValue::CalculationResult { chosen_value, consistent_type.release_value() };
}

void ClampCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CLAMP:\n", "", indent);
    m_min_value->dump(builder, indent + 2);
    m_center_value->dump(builder, indent + 2);
    m_max_value->dump(builder, indent + 2);
}

bool ClampCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_min_value->equals(*static_cast<ClampCalculationNode const&>(other).m_min_value)
        && m_center_value->equals(*static_cast<ClampCalculationNode const&>(other).m_center_value)
        && m_max_value->equals(*static_cast<ClampCalculationNode const&>(other).m_max_value);
}

GC::Ptr<CSSNumericValue> ClampCalculationNode::reify(JS::Realm& realm) const
{
    auto lower = m_min_value->reify(realm);
    auto value = m_center_value->reify(realm);
    auto upper = m_max_value->reify(realm);
    if (!lower || !value || !upper)
        return nullptr;

    return CSSMathClamp::create(realm, numeric_type().value(), lower.as_nonnull(), value.as_nonnull(), upper.as_nonnull());
}

NonnullRefPtr<AbsCalculationNode const> AbsCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AbsCalculationNode(move(value)));
}

AbsCalculationNode::AbsCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The type of its contained calculation.
    : CalculationNode(Type::Abs, value->numeric_type())
    , m_value(move(value))
{
}

AbsCalculationNode::~AbsCalculationNode() = default;

bool AbsCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AbsCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-abs
Optional<CalculatedStyleValue::CalculationResult> AbsCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The abs(A) function contains one calculation A, and returns the absolute value of A, as the same type as the input:
    // if A’s numeric value is positive or 0⁺, just A again; otherwise -1 * A.
    auto child_value = try_get_value_with_canonical_unit(m_value, context, resolution_context);
    if (!child_value.has_value())
        return {};
    return CalculatedStyleValue::CalculationResult { fabs(child_value->value()), child_value->type() };
}

void AbsCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ABS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AbsCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AbsCalculationNode const&>(other).m_value);
}

NonnullRefPtr<SignCalculationNode const> SignCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) SignCalculationNode(move(value)));
}

SignCalculationNode::SignCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Sign, NumericType {})
    , m_value(move(value))
{
}

SignCalculationNode::~SignCalculationNode() = default;

bool SignCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> SignCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-sign
Optional<CalculatedStyleValue::CalculationResult> SignCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The sign(A) function contains one calculation A, and returns -1 if A’s numeric value is negative,
    // +1 if A’s numeric value is positive, 0⁺ if A’s numeric value is 0⁺, and 0⁻ if A’s numeric value is 0⁻.
    // The return type is a <number>, made consistent with the input calculation’s type.

    if (m_value->type() != CalculationNode::Type::Numeric)
        return {};
    auto const& numeric_child = as<NumericCalculationNode>(*m_value);
    double raw_value = numeric_child.value().visit(
        [](Number const& number) { return number.value(); },
        [](Percentage const& percentage) { return percentage.as_fraction(); },
        [](auto const& dimension) { return dimension.raw_value(); });

    double sign = 0;
    if (raw_value < 0) {
        sign = -1;
    } else if (raw_value > 0) {
        sign = 1;
    } else {
        FloatExtractor<double> const extractor { .d = raw_value };
        sign = extractor.sign ? -0.0 : 0.0;
    }

    return CalculatedStyleValue::CalculationResult { sign, NumericType {}.made_consistent_with(numeric_child.numeric_type().value_or({})) };
}

void SignCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIGN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool SignCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SignCalculationNode const&>(other).m_value);
}

NonnullRefPtr<SinCalculationNode const> SinCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) SinCalculationNode(move(value)));
}

SinCalculationNode::SinCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // «[ ]» (empty map).
    : CalculationNode(Type::Sin, NumericType {})
    , m_value(move(value))
{
}

SinCalculationNode::~SinCalculationNode() = default;

bool SinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> SinCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

enum class SinCosOrTan {
    Sin,
    Cos,
    Tan,
};
static Optional<CalculatedStyleValue::CalculationResult> run_sin_cos_or_tan_operation_if_possible(CalculationNode const& child, SinCosOrTan trig_function)
{
    // The sin(A), cos(A), and tan(A) functions all contain a single calculation which must resolve to either a <number>
    // or an <angle>, and compute their corresponding function by interpreting the result of their argument as radians.
    // (That is, sin(45deg), sin(.125turn), and sin(3.14159 / 4) all represent the same value, approximately .707.) They
    // all represent a <number>, with the return type made consistent with the input calculation’s type. sin() and cos()
    // will always return a number between −1 and 1, while tan() can return any number between −∞ and +∞.
    // (See § 10.9 Type Checking for details on how math functions handle ∞.)

    if (child.type() != CalculationNode::Type::Numeric)
        return {};
    auto const& numeric_child = as<NumericCalculationNode>(child);

    auto radians = numeric_child.value().visit(
        [](Angle const& angle) { return angle.to_radians(); },
        [](Number const& number) { return number.value(); },
        [](auto const&) -> double { VERIFY_NOT_REACHED(); });

    double result = 0;
    switch (trig_function) {
    case SinCosOrTan::Sin:
        result = sin(radians);
        break;
    case SinCosOrTan::Cos:
        result = cos(radians);
        break;
    case SinCosOrTan::Tan:
        result = tan(radians);
        break;
    }

    return CalculatedStyleValue::CalculationResult { result, NumericType {}.made_consistent_with(child.numeric_type().value()) };
}

// https://drafts.csswg.org/css-values-4/#funcdef-sin
Optional<CalculatedStyleValue::CalculationResult> SinCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_sin_cos_or_tan_operation_if_possible(m_value, SinCosOrTan::Sin);
}

void SinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SIN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool SinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SinCalculationNode const&>(other).m_value);
}

NonnullRefPtr<CosCalculationNode const> CosCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) CosCalculationNode(move(value)));
}

CosCalculationNode::CosCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Cos, NumericType {})
    , m_value(move(value))
{
}

CosCalculationNode::~CosCalculationNode() = default;

bool CosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> CosCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-cos
Optional<CalculatedStyleValue::CalculationResult> CosCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_sin_cos_or_tan_operation_if_possible(m_value, SinCosOrTan::Cos);
}

void CosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}COS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool CosCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<CosCalculationNode const&>(other).m_value);
}

NonnullRefPtr<TanCalculationNode const> TanCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) TanCalculationNode(move(value)));
}

TanCalculationNode::TanCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Tan, NumericType {})
    , m_value(move(value))
{
}

TanCalculationNode::~TanCalculationNode() = default;

bool TanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> TanCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-tan
Optional<CalculatedStyleValue::CalculationResult> TanCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_sin_cos_or_tan_operation_if_possible(m_value, SinCosOrTan::Tan);
}

void TanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}TAN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool TanCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<TanCalculationNode const&>(other).m_value);
}

NonnullRefPtr<AsinCalculationNode const> AsinCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AsinCalculationNode(move(value)));
}

AsinCalculationNode::AsinCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Asin, NumericType { NumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AsinCalculationNode::~AsinCalculationNode() = default;

bool AsinCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AsinCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

enum class AsinAcosOrAtan {
    Asin,
    Acos,
    Atan,
};
static Optional<CalculatedStyleValue::CalculationResult> run_asin_acos_or_atan_operation_if_possible(CalculationNode const& child, AsinAcosOrAtan trig_function)
{
    // The asin(A), acos(A), and atan(A) functions are the "arc" or "inverse" trigonometric functions, representing
    // the inverse function to their corresponding "normal" trig functions. All of them contain a single calculation
    // which must resolve to a <number>, and compute their corresponding function, interpreting their result as a
    // number of radians, representing an <angle> with the return type made consistent with the input calculation’s
    // type. The angle returned by asin() must be normalized to the range [-90deg, 90deg]; the angle returned by acos()
    // to the range [0deg, 180deg]; and the angle returned by atan() to the range [-90deg, 90deg].

    auto maybe_number = try_get_number(child);
    if (!maybe_number.has_value())
        return {};
    auto number = maybe_number.release_value();

    auto normalize_angle = [](double radians, double min_degrees, double max_degrees) -> double {
        auto degrees = AK::to_degrees(radians);
        while (degrees < min_degrees)
            degrees += 360;
        while (degrees > max_degrees)
            degrees -= 360;
        return degrees;
    };

    double result = 0;
    switch (trig_function) {
    case AsinAcosOrAtan::Asin:
        result = normalize_angle(asin(number), -90, 90);
        break;
    case AsinAcosOrAtan::Acos:
        result = normalize_angle(acos(number), 0, 180);
        break;
    case AsinAcosOrAtan::Atan:
        result = normalize_angle(atan(number), -90, 90);
        break;
    }

    return CalculatedStyleValue::CalculationResult { result, NumericType { NumericType::BaseType::Angle, 1 }.made_consistent_with(child.numeric_type().value()) };
}

// https://drafts.csswg.org/css-values-4/#funcdef-asin
Optional<CalculatedStyleValue::CalculationResult> AsinCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_asin_acos_or_atan_operation_if_possible(m_value, AsinAcosOrAtan::Asin);
}

void AsinCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ASIN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AsinCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AsinCalculationNode const&>(other).m_value);
}

NonnullRefPtr<AcosCalculationNode const> AcosCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AcosCalculationNode(move(value)));
}

AcosCalculationNode::AcosCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Acos, NumericType { NumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AcosCalculationNode::~AcosCalculationNode() = default;

bool AcosCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AcosCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-acos
Optional<CalculatedStyleValue::CalculationResult> AcosCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_asin_acos_or_atan_operation_if_possible(m_value, AsinAcosOrAtan::Acos);
}

void AcosCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ACOS:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AcosCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AcosCalculationNode const&>(other).m_value);
}

NonnullRefPtr<AtanCalculationNode const> AtanCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) AtanCalculationNode(move(value)));
}

AtanCalculationNode::AtanCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Atan, NumericType { NumericType::BaseType::Angle, 1 })
    , m_value(move(value))
{
}

AtanCalculationNode::~AtanCalculationNode() = default;

bool AtanCalculationNode::contains_percentage() const
{
    return m_value->contains_percentage();
}

NonnullRefPtr<CalculationNode const> AtanCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-atan
Optional<CalculatedStyleValue::CalculationResult> AtanCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    return run_asin_acos_or_atan_operation_if_possible(m_value, AsinAcosOrAtan::Atan);
}

void AtanCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool AtanCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<AtanCalculationNode const&>(other).m_value);
}

NonnullRefPtr<Atan2CalculationNode const> Atan2CalculationNode::create(NonnullRefPtr<CalculationNode const> y, NonnullRefPtr<CalculationNode const> x)
{
    return adopt_ref(*new (nothrow) Atan2CalculationNode(move(y), move(x)));
}

Atan2CalculationNode::Atan2CalculationNode(NonnullRefPtr<CalculationNode const> y, NonnullRefPtr<CalculationNode const> x)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ "angle" → 1 ]».
    : CalculationNode(Type::Atan2, NumericType { NumericType::BaseType::Angle, 1 })
    , m_y(move(y))
    , m_x(move(x))
{
}

Atan2CalculationNode::~Atan2CalculationNode() = default;

bool Atan2CalculationNode::contains_percentage() const
{
    return m_y->contains_percentage() || m_x->contains_percentage();
}

NonnullRefPtr<CalculationNode const> Atan2CalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-atan2
Optional<CalculatedStyleValue::CalculationResult> Atan2CalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The atan2(A, B) function contains two comma-separated calculations, A and B. A and B can resolve to any <number>,
    // <dimension>, or <percentage>, but must have a consistent type or else the function is invalid. The function
    // returns the <angle> between the positive X-axis and the point (B,A), with the return type made consistent with the
    // input calculation’s type. The returned angle must be normalized to the interval (-180deg, 180deg] (that is,
    // greater than -180deg, and less than or equal to 180deg).
    auto x_value = try_get_value_with_canonical_unit(m_x, context, resolution_context);
    if (!x_value.has_value())
        return {};
    auto y_value = try_get_value_with_canonical_unit(m_y, context, resolution_context);
    if (!y_value.has_value())
        return {};

    auto input_consistent_type = x_value->type()->consistent_type(y_value->type().value());
    if (!input_consistent_type.has_value())
        return {};

    auto degrees = AK::to_degrees(atan2(y_value->value(), x_value->value()));
    while (degrees <= -180)
        degrees += 360;
    while (degrees > 180)
        degrees -= 360;

    return CalculatedStyleValue::CalculationResult { degrees, NumericType { NumericType::BaseType::Angle, 1 }.made_consistent_with(*input_consistent_type) };
}

void Atan2CalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ATAN2:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool Atan2CalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<Atan2CalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<Atan2CalculationNode const&>(other).m_y);
}

NonnullRefPtr<PowCalculationNode const> PowCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    return adopt_ref(*new (nothrow) PowCalculationNode(move(x), move(y)));
}

PowCalculationNode::PowCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Pow, NumericType {})
    , m_x(move(x))
    , m_y(move(y))
{
}

PowCalculationNode::~PowCalculationNode() = default;

NonnullRefPtr<CalculationNode const> PowCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-pow
Optional<CalculatedStyleValue::CalculationResult> PowCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The pow(A, B) function contains two comma-separated calculations A and B, both of which must resolve to <number>s,
    // and returns the result of raising A to the power of B, returning the value as a <number>. The input calculations
    // must have a consistent type or else the function is invalid; the result’s type will be the consistent type.
    auto a = try_get_number(m_x);
    auto b = try_get_number(m_y);
    if (!a.has_value() || !b.has_value())
        return {};

    auto consistent_type = m_x->numeric_type()->consistent_type(m_y->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { pow(*a, *b), consistent_type };
}

void PowCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}POW:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool PowCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<PowCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<PowCalculationNode const&>(other).m_y);
}

NonnullRefPtr<SqrtCalculationNode const> SqrtCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) SqrtCalculationNode(move(value)));
}

SqrtCalculationNode::SqrtCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Sqrt, NumericType {})
    , m_value(move(value))
{
}

SqrtCalculationNode::~SqrtCalculationNode() = default;

NonnullRefPtr<CalculationNode const> SqrtCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-sqrt
Optional<CalculatedStyleValue::CalculationResult> SqrtCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The sqrt(A) function contains a single calculation which must resolve to a <number>, and returns the square root
    // of the value as a <number>, with the return type made consistent with the input calculation’s type.
    // (sqrt(X) and pow(X, .5) are basically equivalent, differing only in some error-handling; sqrt() is a common enough
    // function that it is provided as a convenience.)
    auto number = try_get_number(m_value);
    if (!number.has_value())
        return {};

    auto consistent_type = NumericType {}.made_consistent_with(m_value->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { sqrt(*number), consistent_type };
}

void SqrtCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}SQRT:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool SqrtCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<SqrtCalculationNode const&>(other).m_value);
}

NonnullRefPtr<HypotCalculationNode const> HypotCalculationNode::create(Vector<NonnullRefPtr<CalculationNode const>> values)
{
    // https://drafts.csswg.org/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(values);
    return adopt_ref(*new (nothrow) HypotCalculationNode(move(values), move(numeric_type)));
}

HypotCalculationNode::HypotCalculationNode(Vector<NonnullRefPtr<CalculationNode const>> values, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Hypot, move(numeric_type))
    , m_values(move(values))
{
}

HypotCalculationNode::~HypotCalculationNode() = default;

bool HypotCalculationNode::contains_percentage() const
{
    for (auto const& value : m_values) {
        if (value->contains_percentage())
            return true;
    }

    return false;
}

NonnullRefPtr<CalculationNode const> HypotCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_children_vector(*this, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-hypot
Optional<CalculatedStyleValue::CalculationResult> HypotCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The hypot(A, …) function contains one or more comma-separated calculations, and returns the length of an
    // N-dimensional vector with components equal to each of the calculations. (That is, the square root of the sum of
    // the squares of its arguments.) The argument calculations can resolve to any <number>, <dimension>, or
    // <percentage>, but must have a consistent type or else the function is invalid; the result’s type will be the
    // consistent type.

    Optional<NumericType> consistent_type;
    double value = 0;

    for (auto const& child : m_values) {
        auto canonical_child = try_get_value_with_canonical_unit(child, context, resolution_context);
        if (!canonical_child.has_value())
            return {};

        if (!consistent_type.has_value())
            consistent_type = canonical_child->type();
        else
            consistent_type = consistent_type->consistent_type(canonical_child->type().value());

        if (!consistent_type.has_value())
            return {};

        value += canonical_child->value() * canonical_child->value();
    }

    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { sqrt(value), consistent_type };
}

void HypotCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}HYPOT:\n", "", indent);
    for (auto const& value : m_values)
        value->dump(builder, indent + 2);
}

bool HypotCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    for (size_t i = 0; i < m_values.size(); ++i) {
        if (!m_values[i]->equals(*static_cast<HypotCalculationNode const&>(other).m_values[i]))
            return false;
    }
    return true;
}

NonnullRefPtr<LogCalculationNode const> LogCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    return adopt_ref(*new (nothrow) LogCalculationNode(move(x), move(y)));
}

LogCalculationNode::LogCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Log, NumericType {})
    , m_x(move(x))
    , m_y(move(y))
{
}

LogCalculationNode::~LogCalculationNode() = default;

NonnullRefPtr<CalculationNode const> LogCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-log
Optional<CalculatedStyleValue::CalculationResult> LogCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The log(A, B?) function contains one or two calculations (representing the value to be logarithmed, and the
    // base of the logarithm, defaulting to e), which must resolve to <number>s, and returns the logarithm base B of
    // the value A, as a <number> with the return type made consistent with the input calculation’s type.

    auto number = try_get_number(m_x);
    auto base = try_get_number(m_y);
    if (!number.has_value() || !base.has_value())
        return {};

    auto consistent_type = NumericType {}.made_consistent_with(m_x->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { log(*number) / log(*base), consistent_type };
}

void LogCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}LOG:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool LogCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<LogCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<LogCalculationNode const&>(other).m_y);
}

NonnullRefPtr<ExpCalculationNode const> ExpCalculationNode::create(NonnullRefPtr<CalculationNode const> value)
{
    return adopt_ref(*new (nothrow) ExpCalculationNode(move(value)));
}

ExpCalculationNode::ExpCalculationNode(NonnullRefPtr<CalculationNode const> value)
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // «[ ]» (empty map).
    : CalculationNode(Type::Exp, NumericType {})
    , m_value(move(value))
{
}

ExpCalculationNode::~ExpCalculationNode() = default;

NonnullRefPtr<CalculationNode const> ExpCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_child(*this, m_value, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-exp
Optional<CalculatedStyleValue::CalculationResult> ExpCalculationNode::run_operation_if_possible(CalculationContext const&, CalculationResolutionContext const&) const
{
    // The exp(A) function contains one calculation which must resolve to a <number>, and returns the same value as
    // pow(e, A) as a <number> with the return type made consistent with the input calculation’s type.

    auto number = try_get_number(m_value);
    if (!number.has_value())
        return {};

    auto consistent_type = NumericType {}.made_consistent_with(m_value->numeric_type().value());
    if (!consistent_type.has_value())
        return {};

    return CalculatedStyleValue::CalculationResult { exp(*number), consistent_type };
}

void ExpCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}EXP:\n", "", indent);
    m_value->dump(builder, indent + 2);
}

bool ExpCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_value->equals(*static_cast<ExpCalculationNode const&>(other).m_value);
}

NonnullRefPtr<RoundCalculationNode const> RoundCalculationNode::create(RoundingStrategy strategy, NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_ref(*new (nothrow) RoundCalculationNode(strategy, move(x), move(y), move(numeric_type)));
}

RoundCalculationNode::RoundCalculationNode(RoundingStrategy mode, NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Round, move(numeric_type))
    , m_strategy(mode)
    , m_x(move(x))
    , m_y(move(y))
{
}

RoundCalculationNode::~RoundCalculationNode() = default;

bool RoundCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

NonnullRefPtr<CalculationNode const> RoundCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    auto simplified_x = simplify_a_calculation_tree(m_x, context, resolution_context);
    auto simplified_y = simplify_a_calculation_tree(m_y, context, resolution_context);
    if (simplified_x != m_x || simplified_y != m_y)
        return create(m_strategy, move(simplified_x), move(simplified_y));
    return *this;
}

// https://drafts.csswg.org/css-values-4/#funcdef-round
Optional<CalculatedStyleValue::CalculationResult> RoundCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    // The round(<rounding-strategy>?, A, B?) function contains an optional rounding strategy, and two calculations A
    // and B, and returns the value of A, rounded according to the rounding strategy, to the nearest integer multiple of
    // B either above or below A. The argument calculations can resolve to any <number>, <dimension>, or <percentage>,
    // but must have a consistent type or else the function is invalid; the result’s type will be the consistent type.

    auto maybe_a = try_get_value_with_canonical_unit(m_x, context, resolution_context);
    auto maybe_b = try_get_value_with_canonical_unit(m_y, context, resolution_context);
    if (!maybe_a.has_value() || !maybe_b.has_value())
        return {};

    auto consistent_type = maybe_a->type()->made_consistent_with(maybe_b->type().value());
    if (!consistent_type.has_value())
        return {};

    auto a = maybe_a->value();
    auto b = maybe_b->value();

    // https://drafts.csswg.org/css-values-4/#round-infinities
    // In round(A, B), if B is 0, the result is NaN. If A and B are both infinite, the result is NaN.
    if (b == 0 || (isinf(a) && isinf(b)))
        return CalculatedStyleValue::CalculationResult { AK::NaN<double>, consistent_type };

    // If A is infinite but B is finite, the result is the same infinity.
    if (isinf(a) && isfinite(b))
        return CalculatedStyleValue::CalculationResult { a, consistent_type };

    // If A is finite but B is infinite, the result depends on the <rounding-strategy> and the sign of A:
    if (isfinite(a) && isinf(b)) {
        FloatExtractor<double> const extractor { .d = a };

        switch (m_strategy) {
        // nearest, to-zero:
        case RoundingStrategy::Nearest:
        case RoundingStrategy::ToZero: {
            // If A is positive or 0⁺, return 0⁺. Otherwise, return 0⁻.
            return CalculatedStyleValue::CalculationResult { !extractor.sign ? 0.0 : -0.0, consistent_type };
        }
        // up:
        case RoundingStrategy::Up: {
            double result;
            if (a > 0) {
                // If A is positive(not zero), return +∞.
                result = AK::Infinity<double>;
            } else {
                // If A is 0⁺, return 0⁺. Otherwise, return 0⁻.
                result = !extractor.sign ? 0.0 : -0.0;
            }

            return CalculatedStyleValue::CalculationResult { result, consistent_type };
        }
        // down:
        case RoundingStrategy::Down: {
            double result;
            if (a < 0) {
                // If A is negative (not zero), return −∞.
                result = -AK::Infinity<double>;
            } else {
                // If A is 0⁻, return 0⁻. Otherwise, return 0⁺.
                result = extractor.sign ? -0.0 : 0.0;
            }

            return CalculatedStyleValue::CalculationResult { result, consistent_type };
        }
        }
    }

    // If A is exactly equal to an integer multiple of B, round() resolves to A exactly (preserving whether A is 0⁻ or
    // 0⁺, if relevant).
    if (fmod(a, b) == 0)
        return maybe_a.release_value();

    // Otherwise, there are two integer multiples of B that are potentially "closest" to A, lower B which is closer to
    // −∞ and upper B which is closer to +∞. The following <rounding-strategy>s dictate how to choose between them:

    // FIXME: If lower B would be zero, it is specifically equal to 0⁺;
    //        if upper B would be zero, it is specifically equal to 0⁻.
    auto get_lower_b = [&]() {
        return floor(a / b) * b;
    };
    auto get_upper_b = [&]() {
        return ceil(a / b) * b;
    };

    double rounded = 0;
    switch (m_strategy) {
    // -> nearest
    case RoundingStrategy::Nearest: {
        // Choose whichever of lower B and upper B that has the smallest absolute difference from A.
        // If both have an equal difference (A is exactly between the two values), choose upper B.
        auto lower_b = get_lower_b();
        auto upper_b = get_upper_b();
        auto lower_diff = fabs(lower_b - a);
        auto upper_diff = fabs(upper_b - a);
        rounded = upper_diff <= lower_diff ? upper_b : lower_b;
        break;
    }
    // -> up
    case RoundingStrategy::Up:
        // Choose upper B.
        rounded = get_upper_b();
        break;
    // -> down
    case RoundingStrategy::Down:
        // Choose lower B.
        rounded = get_lower_b();
        break;
    // -> to-zero
    case RoundingStrategy::ToZero: {
        // Choose whichever of lower B and upper B that has the smallest absolute difference from 0.
        auto lower_b = get_lower_b();
        auto upper_b = get_upper_b();
        rounded = fabs(upper_b) < fabs(lower_b) ? upper_b : lower_b;
        break;
    }
    }

    return CalculatedStyleValue::CalculationResult { rounded, consistent_type };
}

void RoundCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}ROUND: {}\n", "", indent, CSS::to_string(m_strategy));
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool RoundCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_strategy == static_cast<RoundCalculationNode const&>(other).m_strategy
        && m_x->equals(*static_cast<RoundCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<RoundCalculationNode const&>(other).m_y);
}

NonnullRefPtr<ModCalculationNode const> ModCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_ref(*new (nothrow) ModCalculationNode(move(x), move(y), move(numeric_type)));
}

ModCalculationNode::ModCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Mod, move(numeric_type))
    , m_x(move(x))
    , m_y(move(y))
{
}

ModCalculationNode::~ModCalculationNode() = default;

bool ModCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

NonnullRefPtr<CalculationNode const> ModCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

enum class ModOrRem {
    Mod,
    Rem,
};
// https://drafts.csswg.org/css-values-4/#funcdef-mod
static Optional<CalculatedStyleValue::CalculationResult> run_mod_or_rem_operation_if_possible(CalculationNode const& numerator, CalculationNode const& denominator, CalculationContext const& context, CalculationResolutionContext const& resolution_context, ModOrRem mod_or_rem)
{
    // The modulus functions mod(A, B) and rem(A, B) similarly contain two calculations A and B, and return the
    // difference between A and the nearest integer multiple of B either above or below A. The argument calculations
    // can resolve to any <number>, <dimension>, or <percentage>, but must have the same type, or else the function
    // is invalid; the result will have the same type as the arguments.
    auto numerator_value = try_get_value_with_canonical_unit(numerator, context, resolution_context);
    auto denominator_value = try_get_value_with_canonical_unit(denominator, context, resolution_context);
    if (!numerator_value.has_value() || !denominator_value.has_value())
        return {};

    if (numerator_value->type() != denominator_value->type())
        return {};

    // The two functions are very similar, and in fact return identical results if both arguments are positive or both
    // are negative: the value of the function is equal to the value of A shifted by the integer multiple of B that
    // brings the value between zero and B. (Specifically, the range includes zero and excludes B.More specifically,
    // if B is positive the range starts at 0⁺, and if B is negative it starts at 0⁻.)
    //
    // Their behavior diverges if the A value and the B step are on opposite sides of zero: mod() (short for “modulus”)
    // continues to choose the integer multiple of B that puts the value between zero and B, as above (guaranteeing
    // that the result will either be zero or share the sign of B, not A), while rem() (short for "remainder") chooses
    // the integer multiple of B that puts the value between zero and -B, avoiding changing the sign of the value.

    double result = 0;
    if (mod_or_rem == ModOrRem::Mod) {
        auto quotient = floor(numerator_value->value() / denominator_value->value());
        result = numerator_value->value() - (denominator_value->value() * quotient);
    } else {
        result = fmod(numerator_value->value(), denominator_value->value());
    }

    return CalculatedStyleValue::CalculationResult { result, numerator_value->type() };
}

// https://drafts.csswg.org/css-values-4/#funcdef-mod
Optional<CalculatedStyleValue::CalculationResult> ModCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_mod_or_rem_operation_if_possible(m_x, m_y, context, resolution_context, ModOrRem::Mod);
}

void ModCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}MOD:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool ModCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<ModCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<ModCalculationNode const&>(other).m_y);
}

NonnullRefPtr<RemCalculationNode const> RemCalculationNode::create(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y)
{
    // https://www.w3.org/TR/css-values-4/#determine-the-type-of-a-calculation
    // The result of adding the types of its comma-separated calculations.
    auto numeric_type = add_the_types(*x, *y);
    return adopt_ref(*new (nothrow) RemCalculationNode(move(x), move(y), move(numeric_type)));
}

RemCalculationNode::RemCalculationNode(NonnullRefPtr<CalculationNode const> x, NonnullRefPtr<CalculationNode const> y, Optional<NumericType> numeric_type)
    : CalculationNode(Type::Rem, move(numeric_type))
    , m_x(move(x))
    , m_y(move(y))
{
}

RemCalculationNode::~RemCalculationNode() = default;

bool RemCalculationNode::contains_percentage() const
{
    return m_x->contains_percentage() || m_y->contains_percentage();
}

NonnullRefPtr<CalculationNode const> RemCalculationNode::with_simplified_children(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return simplify_2_children(*this, m_x, m_y, context, resolution_context);
}

// https://drafts.csswg.org/css-values-4/#funcdef-mod
Optional<CalculatedStyleValue::CalculationResult> RemCalculationNode::run_operation_if_possible(CalculationContext const& context, CalculationResolutionContext const& resolution_context) const
{
    return run_mod_or_rem_operation_if_possible(m_x, m_y, context, resolution_context, ModOrRem::Rem);
}

void RemCalculationNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}REM:\n", "", indent);
    m_x->dump(builder, indent + 2);
    m_y->dump(builder, indent + 2);
}

bool RemCalculationNode::equals(CalculationNode const& other) const
{
    if (this == &other)
        return true;
    if (type() != other.type())
        return false;
    return m_x->equals(*static_cast<RemCalculationNode const&>(other).m_x)
        && m_y->equals(*static_cast<RemCalculationNode const&>(other).m_y);
}

CalculatedStyleValue::CalculationResult CalculatedStyleValue::CalculationResult::from_value(Value const& value, CalculationResolutionContext const& context, Optional<NumericType> numeric_type)
{
    auto number = value.visit(
        [](Number const& number) { return number.value(); },
        [](Angle const& angle) { return angle.to_degrees(); },
        [](Flex const& flex) { return flex.to_fr(); },
        [](Frequency const& frequency) { return frequency.to_hertz(); },
        [&context](Length const& length) {
            // Handle some common cases first, so we can resolve more without a context
            if (length.is_absolute())
                return length.absolute_length_to_px_without_rounding();

            // If we don't have a context, we cant resolve the length, so return NAN
            if (!context.length_resolution_context.has_value()) {
                dbgln("Failed to resolve length `{}`, likely due to calc() being used with relative units and a property not taking it into account", length.to_string());
                return AK::NaN<double>;
            }

            return length.to_px(context.length_resolution_context.value()).to_double();
        },
        [](Resolution const& resolution) { return resolution.to_dots_per_pixel(); },
        [](Time const& time) { return time.to_seconds(); },
        [](Percentage const& percentage) { return percentage.value(); });

    return CalculationResult { number, move(numeric_type) };
}

void CalculatedStyleValue::CalculationResult::add(CalculationResult const& other)
{
    m_value = m_value + other.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->added_to(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::subtract(CalculationResult const& other)
{
    m_value = m_value - other.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->added_to(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::multiply_by(CalculationResult const& other)
{
    m_value = m_value * other.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->multiplied_by(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::divide_by(CalculationResult const& other)
{
    auto other_copy = other;
    other_copy.invert();
    m_value = m_value * other_copy.m_value;
    m_type = m_type.has_value() && other.m_type.has_value() ? m_type->multiplied_by(*other.m_type) : OptionalNone {};
}

void CalculatedStyleValue::CalculationResult::negate()
{
    m_value = 0 - m_value;
}

void CalculatedStyleValue::CalculationResult::invert()
{
    // FIXME: Correctly handle division by zero.
    m_value = 1.0 / m_value;
    if (m_type.has_value())
        m_type = m_type->inverted();
}

String CalculatedStyleValue::to_string(SerializationMode serialization_mode) const
{
    return serialize_a_math_function(m_calculation, m_context, serialization_mode);
}

ValueComparingNonnullRefPtr<StyleValue const> CalculatedStyleValue::absolutized(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    Length::ResolutionContext length_resolution_context {
        .viewport_rect = viewport_rect,
        .font_metrics = font_metrics,
        .root_font_metrics = root_font_metrics
    };

    auto simplified_calculation_tree = simplify_a_calculation_tree(m_calculation, m_context, { .length_resolution_context = length_resolution_context });

    auto const simplified_percentage_dimension_mix = [&]() -> Optional<ValueComparingNonnullRefPtr<StyleValue const>> {
        // NOTE: A percentage dimension mix is a SumCalculationNode with two NumericCalculationNode children which have
        //       matching base types - only the first of which has a percent hint.
        if (simplified_calculation_tree->type() != CalculationNode::Type::Sum)
            return {};

        auto const& sum_node = as<SumCalculationNode>(*simplified_calculation_tree);

        if (sum_node.children()[0]->type() != CalculationNode::Type::Numeric || sum_node.children()[1]->type() != CalculationNode::Type::Numeric)
            return {};

        auto const& first_node = as<NumericCalculationNode>(*sum_node.children()[0]);
        auto const& second_node = as<NumericCalculationNode>(*sum_node.children()[1]);

        auto first_base_type = first_node.numeric_type()->entry_with_value_1_while_all_others_are_0();
        auto second_base_type = second_node.numeric_type()->entry_with_value_1_while_all_others_are_0();

        if (!first_base_type.has_value() || first_base_type != second_base_type)
            return {};

        if (!first_node.numeric_type()->percent_hint().has_value() || second_node.numeric_type()->percent_hint().has_value())
            return {};

        auto dimension_component = try_get_value_with_canonical_unit(second_node, m_context, {});

        // https://drafts.csswg.org/css-values-4/#combine-mixed
        // The computed value of a percentage-dimension mix is defined as
        //  - a computed percentage if the dimension component is zero
        if (dimension_component->value() == 0)
            return PercentageStyleValue::create(first_node.value().get<Percentage>());

        return {};
    }();

    if (simplified_percentage_dimension_mix.has_value())
        return simplified_percentage_dimension_mix.value();

    return CalculatedStyleValue::create(simplified_calculation_tree, m_resolved_type, m_context);
}

bool CalculatedStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    return m_calculation->equals(*other.as_calculated().m_calculation);
}

// https://drafts.csswg.org/css-values-4/#calc-computed-value
Optional<CalculatedStyleValue::ResolvedValue> CalculatedStyleValue::resolve_value(CalculationResolutionContext const& resolution_context) const
{
    // The calculation tree is again simplified at used value time; with used value time information.
    auto simplified_tree = simplify_a_calculation_tree(m_calculation, m_context, resolution_context);

    if (!is<NumericCalculationNode>(*simplified_tree))
        return {};

    auto value = try_get_value_with_canonical_unit(simplified_tree, m_context, resolution_context);

    VERIFY(value.has_value());

    auto raw_value = value->value();

    // https://drafts.csswg.org/css-values/#calc-ieee
    // NaN does not escape a top-level calculation; it’s censored into a zero value.
    if (isnan(raw_value))
        raw_value = 0;

    // https://drafts.csswg.org/css-values/#calc-range
    // the value resulting from a top-level calculation must be clamped to the range allowed in the target context.
    // Clamping is performed on computed values to the extent possible, and also on used values if computation was
    // unable to sufficiently simplify the expression to allow range-checking.
    Optional<AcceptedTypeRange> accepted_range;

    if (value->type()->matches_number(m_context.percentages_resolve_as))
        accepted_range = m_context.resolve_numbers_as_integers ? m_context.accepted_type_ranges.get(ValueType::Integer) : m_context.accepted_type_ranges.get(ValueType::Number);
    else if (value->type()->matches_angle(m_context.percentages_resolve_as))
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Angle);
    else if (value->type()->matches_flex(m_context.percentages_resolve_as))
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Flex);
    else if (value->type()->matches_frequency(m_context.percentages_resolve_as))
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Frequency);
    else if (value->type()->matches_length(m_context.percentages_resolve_as))
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Length);
    else if (value->type()->matches_percentage())
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Percentage);
    else if (value->type()->matches_resolution(m_context.percentages_resolve_as))
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Resolution);
    else if (value->type()->matches_time(m_context.percentages_resolve_as))
        accepted_range = m_context.accepted_type_ranges.get(ValueType::Time);

    if (!accepted_range.has_value()) {
        dbgln_if(LIBWEB_CSS_DEBUG, "FIXME: Calculation context missing accepted type range {}", value->type());
        // FIXME: Infinity for integers should be i32 max rather than float max
        accepted_range = { AK::NumericLimits<float>::lowest(), AK::NumericLimits<float>::max() };
    }

    raw_value = clamp(raw_value, accepted_range->min, accepted_range->max);

    return ResolvedValue { raw_value, value->type() };
}

Optional<Angle> CalculatedStyleValue::resolve_angle(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_angle(m_context.percentages_resolve_as))
        return Angle::make_degrees(result->value);

    return {};
}

Optional<Flex> CalculatedStyleValue::resolve_flex(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_flex(m_context.percentages_resolve_as))
        return Flex::make_fr(result->value);

    return {};
}

Optional<Frequency> CalculatedStyleValue::resolve_frequency(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_frequency(m_context.percentages_resolve_as))
        return Frequency::make_hertz(result->value);

    return {};
}

Optional<Length> CalculatedStyleValue::resolve_length(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_length(m_context.percentages_resolve_as))
        return Length::make_px(result->value);

    return {};
}

Optional<Percentage> CalculatedStyleValue::resolve_percentage(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_percentage())
        return Percentage { result->value };

    return {};
}

Optional<Resolution> CalculatedStyleValue::resolve_resolution(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_resolution(m_context.percentages_resolve_as))
        return Resolution::make_dots_per_pixel(result->value);

    return {};
}

Optional<Time> CalculatedStyleValue::resolve_time(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_time(m_context.percentages_resolve_as))
        return Time::make_seconds(result->value);

    return {};
}

Optional<double> CalculatedStyleValue::resolve_number(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_number(m_context.percentages_resolve_as))
        return result->value;

    return {};
}

Optional<i64> CalculatedStyleValue::resolve_integer(CalculationResolutionContext const& context) const
{
    auto result = resolve_value(context);

    if (result.has_value() && result->type.has_value() && result->type->matches_number(m_context.percentages_resolve_as))
        return llround(result->value);

    return {};
}

bool CalculatedStyleValue::contains_percentage() const
{
    return m_calculation->contains_percentage();
}

String CalculatedStyleValue::dump() const
{
    StringBuilder builder;
    m_calculation->dump(builder, 0);
    return builder.to_string_without_validation();
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-a-math-expression
GC::Ref<CSSStyleValue> CalculatedStyleValue::reify(JS::Realm& realm, String const& associated_property) const
{
    // NB: This spec algorithm isn't really implementable here - it's incomplete, and assumes we don't already have a
    //     calculation tree. So we have a per-node method instead.
    if (auto reified = m_calculation->reify(realm))
        return *reified;
    // Some math functions are not reifiable yet. If we contain one, we have to fall back to CSSStyleValue.
    // https://github.com/w3c/css-houdini-drafts/issues/1090
    return StyleValue::reify(realm, associated_property);
}

struct NumericChildAndIndex {
    NonnullRefPtr<NumericCalculationNode const> child;
    size_t index;
};
static Optional<NumericChildAndIndex> find_numeric_child_with_same_unit(Vector<NonnullRefPtr<CalculationNode const>> children, NumericCalculationNode const& target)
{
    for (auto i = 0u; i < children.size(); ++i) {
        auto& child = children[i];
        if (child->type() != CalculationNode::Type::Numeric)
            continue;
        auto const& child_numeric = as<NumericCalculationNode>(*child);
        if (child_numeric.value().index() != target.value().index())
            continue;

        auto matches = child_numeric.value().visit(
            [&](Percentage const&) {
                return target.value().has<Percentage>();
            },
            [&](Number const&) {
                return target.value().has<Number>();
            },
            [&]<typename T>(T const& value) {
                if (auto const* other = target.value().get_pointer<T>(); other && other->unit() == value.unit()) {
                    return true;
                }
                return false;
            });

        if (matches)
            return NumericChildAndIndex { child_numeric, i };
    }

    return {};
}

static RefPtr<NumericCalculationNode const> make_calculation_node(CalculatedStyleValue::CalculationResult const& calculation_result, CalculationContext const& context)
{
    auto const& accumulated_type = calculation_result.type().value();
    if (accumulated_type.matches_number(context.percentages_resolve_as))
        return NumericCalculationNode::create(Number { Number::Type::Number, calculation_result.value() }, context);
    if (accumulated_type.matches_percentage())
        return NumericCalculationNode::create(Percentage { calculation_result.value() }, context);
    if (accumulated_type.matches_angle(context.percentages_resolve_as))
        return NumericCalculationNode::create(Angle::make_degrees(calculation_result.value()), context);
    if (accumulated_type.matches_flex(context.percentages_resolve_as))
        return NumericCalculationNode::create(Flex::make_fr(calculation_result.value()), context);
    if (accumulated_type.matches_frequency(context.percentages_resolve_as))
        return NumericCalculationNode::create(Frequency::make_hertz(calculation_result.value()), context);
    if (accumulated_type.matches_length(context.percentages_resolve_as))
        return NumericCalculationNode::create(Length::make_px(calculation_result.value()), context);
    if (accumulated_type.matches_resolution(context.percentages_resolve_as))
        return NumericCalculationNode::create(Resolution::make_dots_per_pixel(calculation_result.value()), context);
    if (accumulated_type.matches_time(context.percentages_resolve_as))
        return NumericCalculationNode::create(Time::make_seconds(calculation_result.value()), context);

    return nullptr;
}

// https://drafts.csswg.org/css-values-4/#calc-simplification
NonnullRefPtr<CalculationNode const> simplify_a_calculation_tree(CalculationNode const& original_root, CalculationContext const& context, CalculationResolutionContext const& resolution_context)
{
    // To simplify a calculation tree root:
    // FIXME: If needed, we could detect that nothing has changed and then return the original `root`, in more places.
    NonnullRefPtr<CalculationNode const> root = original_root;

    // 1. If root is a numeric value:
    if (root->type() == CalculationNode::Type::Numeric) {
        auto const& root_numeric = as<NumericCalculationNode>(*root);

        // 1. If root is a percentage that will be resolved against another value, and there is enough information
        //    available to resolve it, do so, and express the resulting numeric value in the appropriate canonical unit.
        //    Return the value.
        if (auto const* percentage = root_numeric.value().get_pointer<Percentage>(); percentage && context.percentages_resolve_as.has_value()) {
            // NOTE: We use nullptr here to signify "use the original".
            RefPtr<NumericCalculationNode const> resolved = resolution_context.percentage_basis.visit(
                [](Empty const&) -> RefPtr<NumericCalculationNode const> { return nullptr; },
                [&](Angle const& angle) -> RefPtr<NumericCalculationNode const> {
                    VERIFY(context.percentages_resolve_as == ValueType::Angle);
                    if (angle.unit() == AngleUnit::Deg)
                        return NumericCalculationNode::create(angle.percentage_of(*percentage), context);
                    return NumericCalculationNode::create(Angle::make_degrees(angle.to_degrees()).percentage_of(*percentage), context);
                },
                [&](Frequency const& frequency) -> RefPtr<NumericCalculationNode const> {
                    VERIFY(context.percentages_resolve_as == ValueType::Frequency);
                    if (frequency.unit() == FrequencyUnit::Hz)
                        return NumericCalculationNode::create(frequency.percentage_of(*percentage), context);
                    return NumericCalculationNode::create(Frequency::make_hertz(frequency.to_hertz()).percentage_of(*percentage), context);
                },
                [&](Length const& length) -> RefPtr<NumericCalculationNode const> {
                    VERIFY(context.percentages_resolve_as == ValueType::Length);
                    if (length.unit() == LengthUnit::Px)
                        return NumericCalculationNode::create(length.percentage_of(*percentage), context);
                    if (length.is_absolute())
                        return NumericCalculationNode::create(Length::make_px(length.absolute_length_to_px()).percentage_of(*percentage), context);
                    if (resolution_context.length_resolution_context.has_value())
                        return NumericCalculationNode::create(Length::make_px(length.to_px(resolution_context.length_resolution_context.value())).percentage_of(*percentage), context);
                    return nullptr;
                },
                [&](Time const& time) -> RefPtr<NumericCalculationNode const> {
                    VERIFY(context.percentages_resolve_as == ValueType::Time);
                    if (time.unit() == TimeUnit::S)
                        return NumericCalculationNode::create(time.percentage_of(*percentage), context);
                    return NumericCalculationNode::create(Time::make_seconds(time.to_seconds()).percentage_of(*percentage), context);
                });

            if (resolved)
                return resolved.release_nonnull();
        }

        // 2. If root is a dimension that is not expressed in its canonical unit, and there is enough information available
        //    to convert it to the canonical unit, do so, and return the value.
        else {
            // NOTE: We use nullptr here to signify "use the original".
            RefPtr<CalculationNode const> resolved = root_numeric.value().visit(
                [&](Angle const& angle) -> RefPtr<CalculationNode const> {
                    if (angle.unit() == AngleUnit::Deg)
                        return nullptr;
                    return NumericCalculationNode::create(Angle::make_degrees(angle.to_degrees()), context);
                },
                [&](Flex const& flex) -> RefPtr<CalculationNode const> {
                    if (flex.unit() == FlexUnit::Fr)
                        return nullptr;
                    return NumericCalculationNode::create(Flex::make_fr(flex.to_fr()), context);
                },
                [&](Frequency const& frequency) -> RefPtr<CalculationNode const> {
                    if (frequency.unit() == FrequencyUnit::Hz)
                        return nullptr;
                    return NumericCalculationNode::create(Frequency::make_hertz(frequency.to_hertz()), context);
                },
                [&](Length const& length) -> RefPtr<CalculationNode const> {
                    if (length.unit() == LengthUnit::Px)
                        return nullptr;
                    if (length.is_absolute())
                        return NumericCalculationNode::create(Length::make_px(length.absolute_length_to_px()), context);
                    if (resolution_context.length_resolution_context.has_value())
                        return NumericCalculationNode::create(Length::make_px(length.to_px(resolution_context.length_resolution_context.value())), context);
                    return nullptr;
                },
                [&](Number const&) -> RefPtr<CalculationNode const> {
                    return nullptr;
                },
                [&](Percentage const&) -> RefPtr<CalculationNode const> {
                    return nullptr;
                },
                [&](Resolution const& resolution) -> RefPtr<CalculationNode const> {
                    if (resolution.unit() == ResolutionUnit::Dppx)
                        return nullptr;
                    return NumericCalculationNode::create(Resolution::make_dots_per_pixel(resolution.to_dots_per_pixel()), context);
                },
                [&](Time const& time) -> RefPtr<CalculationNode const> {
                    if (time.unit() == TimeUnit::S)
                        return nullptr;
                    return NumericCalculationNode::create(Time::make_seconds(time.to_seconds()), context);
                });
            if (resolved)
                return resolved.release_nonnull();
        }

        // 3. If root is a <calc-keyword> that can be resolved, return what it resolves to, simplified.
        // NOTE: We already resolve our `<calc-keyword>`s at parse-time.
        // FIXME: Revisit this once we support any keywords that need resolving later.

        // 4. Otherwise, return root.
        return root;
    }

    // 2. If root is any other leaf node (not an operator node):
    // FIXME: We don't yet allow any of these inside a calculation tree. Revisit once we do.

    // 3. At this point, root is an operator node. Simplify all the calculation children of root.
    root = root->with_simplified_children(context, resolution_context);

    // 4. If root is an operator node that’s not one of the calc-operator nodes, and all of its calculation children
    //    are numeric values with enough information to compute the operation root represents, return the result of
    //    running root’s operation using its children, expressed in the result’s canonical unit.
    if (root->is_math_function_node()) {
        if (auto maybe_simplified = root->run_operation_if_possible(context, resolution_context); maybe_simplified.has_value()) {
            if (auto node = make_calculation_node(maybe_simplified.release_value(), context))
                return node.release_nonnull();
            return root;
        }
    }

    // 5. If root is a Min or Max node, attempt to partially simplify it:
    if (root->type() == CalculationNode::Type::Min || root->type() == CalculationNode::Type::Max) {
        bool const is_min = root->type() == CalculationNode::Type::Min;
        auto const& children = is_min ? as<MinCalculationNode>(*root).children() : as<MaxCalculationNode>(*root).children();

        // 1. For each node child of root’s children:
        //    If child is a numeric value with enough information to compare magnitudes with another child of the same
        //    unit (see note in previous step), and there are other children of root that are numeric values with the
        //    same unit, combine all such children with the appropriate operator per root, and replace child with the
        //    result, removing all other child nodes involved.
        Vector<NonnullRefPtr<CalculationNode const>> simplified_children;
        simplified_children.ensure_capacity(children.size());
        for (auto const& child : children) {
            if (child->type() != CalculationNode::Type::Numeric || simplified_children.is_empty()) {
                simplified_children.append(child);
                continue;
            }

            auto const& child_numeric = as<NumericCalculationNode>(*child);
            if (context.percentages_resolve_as.has_value() && child_numeric.value().has<Percentage>()) {
                // NOTE: We can't compare this percentage yet.
                simplified_children.append(child);
                continue;
            }

            auto existing_child_and_index = find_numeric_child_with_same_unit(simplified_children, child_numeric);
            if (existing_child_and_index.has_value()) {
                bool const should_replace_existing_value = existing_child_and_index->child->value().visit(
                    [&](Percentage const& percentage) {
                        if (is_min)
                            return child_numeric.value().get_pointer<Percentage>()->value() < percentage.value();
                        return child_numeric.value().get_pointer<Percentage>()->value() > percentage.value();
                    },
                    [&](Number const& number) {
                        if (is_min)
                            return child_numeric.value().get_pointer<Number>()->value() < number.value();
                        return child_numeric.value().get_pointer<Number>()->value() > number.value();
                    },
                    [&]<typename T>(T const& value) {
                        if (is_min)
                            return child_numeric.value().get_pointer<T>()->raw_value() < value.raw_value();
                        return child_numeric.value().get_pointer<T>()->raw_value() > value.raw_value();
                    });

                if (should_replace_existing_value)
                    simplified_children[existing_child_and_index->index] = child_numeric;

            } else {
                simplified_children.append(child);
            }
        }

        // 2. If root has only one child, return the child.
        //    Otherwise, return root.
        if (simplified_children.size() == 1)
            return simplified_children.first();
        // NOTE: Because our root is immutable, we have to return a new node with the modified children.
        if (is_min)
            return MinCalculationNode::create(move(simplified_children));
        return MaxCalculationNode::create(move(simplified_children));
    }

    // 6. If root is a Negate node:
    if (root->type() == CalculationNode::Type::Negate) {
        auto const& root_negate = as<NegateCalculationNode>(*root);
        auto const& child = root_negate.child();
        // 1. If root’s child is a numeric value, return an equivalent numeric value, but with the value negated (0 - value).
        if (child.type() == CalculationNode::Type::Numeric)
            return as<NumericCalculationNode>(child).negated(context);

        // 2. If root’s child is a Negate node, return the child’s child.
        if (child.type() == CalculationNode::Type::Negate)
            return as<NegateCalculationNode>(child).child();

        // 3. Return root.
        // NOTE: Because our root is immutable, we have to return a new node if the child was modified.
        if (&child == &root_negate.child())
            return root;
        return NegateCalculationNode::create(move(child));
    }

    // 7. If root is an Invert node:
    if (root->type() == CalculationNode::Type::Invert) {
        auto const& root_invert = as<InvertCalculationNode>(*root);
        auto const& child = root_invert.child();

        // 1. If root’s child is a number (not a percentage or dimension) return the reciprocal of the child’s value.
        if (child.type() == CalculationNode::Type::Numeric) {
            if (auto const* number = as<NumericCalculationNode>(child).value().get_pointer<Number>()) {
                // TODO: Ensure we're doing the right thing for weird divisions.
                return NumericCalculationNode::create(Number(Number::Type::Number, 1.0 / number->value()), context);
            }
        }

        // 2. If root’s child is an Invert node, return the child’s child.
        if (child.type() == CalculationNode::Type::Invert)
            return as<InvertCalculationNode>(child).child();

        // 3. Return root.
        // NOTE: Because our root is immutable, we have to return a new node if the child was modified.
        if (&child == &root_invert.child())
            return root;
        return InvertCalculationNode::create(move(child));
    }

    // 8. If root is a Sum node:
    if (root->type() == CalculationNode::Type::Sum) {
        auto const& root_sum = as<SumCalculationNode>(*root);

        Vector<NonnullRefPtr<CalculationNode const>> flattened_children;
        flattened_children.ensure_capacity(root_sum.children().size());
        // 1. For each of root’s children that are Sum nodes, replace them with their children.
        for (auto const& child : root_sum.children()) {
            if (child->type() == CalculationNode::Type::Sum) {
                flattened_children.extend(as<SumCalculationNode>(*child).children());
            } else {
                flattened_children.append(child);
            }
        }

        // 2. For each set of root’s children that are numeric values with identical units, remove those children and
        //    replace them with a single numeric value containing the sum of the removed nodes, and with the same unit.
        //    (E.g. combine numbers, combine percentages, combine px values, etc.)

        // NOTE: For each child, scan this summed_children list for the first one that has the same type, then replace that with the new summed value.
        Vector<NonnullRefPtr<CalculationNode const>> summed_children;
        for (auto const& child : flattened_children) {
            if (child->type() != CalculationNode::Type::Numeric) {
                summed_children.append(child);
                continue;
            }
            auto const& child_numeric = as<NumericCalculationNode>(*child);

            auto existing_child_and_index = find_numeric_child_with_same_unit(summed_children, child_numeric);
            if (existing_child_and_index.has_value()) {
                auto new_value = existing_child_and_index->child->value().visit(
                    [&](Percentage const& percentage) {
                        return NumericCalculationNode::create(Percentage(percentage.value() + child_numeric.value().get<Percentage>().value()), context);
                    },
                    [&](Number const& number) {
                        return NumericCalculationNode::create(Number(Number::Type::Number, number.value() + child_numeric.value().get<Number>().value()), context);
                    },
                    [&]<typename T>(T const& value) {
                        return NumericCalculationNode::create(T(value.raw_value() + child_numeric.value().get<T>().raw_value(), value.unit()), context);
                    });
                summed_children[existing_child_and_index->index] = move(new_value);
            } else {
                summed_children.append(child);
            }
        }

        // 3. If root has only a single child at this point, return the child. Otherwise, return root.
        if (summed_children.size() == 1)
            return summed_children.first();

        // NOTE: Because our root is immutable, we have to return a new node with the modified children.
        return SumCalculationNode::create(move(summed_children));
    }

    // 9. If root is a Product node:
    if (root->type() == CalculationNode::Type::Product) {
        auto const& root_product = as<ProductCalculationNode>(*root);

        Vector<NonnullRefPtr<CalculationNode const>> children;
        children.ensure_capacity(root_product.children().size());

        // 1. For each of root’s children that are Product nodes, replace them with their children.
        for (auto const& child : root_product.children()) {
            if (child->type() == CalculationNode::Type::Product) {
                children.extend(as<ProductCalculationNode>(*child).children());
            } else {
                children.append(child);
            }
        }

        // 2. If root has multiple children that are numbers (not percentages or dimensions),
        //    remove them and replace them with a single number containing the product of the removed nodes.
        Optional<size_t> number_index;
        for (auto i = 0u; i < children.size(); ++i) {
            if (children[i]->type() == CalculationNode::Type::Numeric) {
                if (auto const* number = as<NumericCalculationNode>(*children[i]).value().get_pointer<Number>()) {
                    if (!number_index.has_value()) {
                        number_index = i;
                        continue;
                    }
                    children[*number_index] = NumericCalculationNode::create(as<NumericCalculationNode>(*children[*number_index]).value().get<Number>() * *number, context);
                    children.remove(i);
                    --i; // Look at this same index again next loop.
                }
            }
        }

        // 3. If root contains only two children, one of which is a number(not a percentage or dimension) and the other
        //    of which is a Sum whose children are all numeric values, multiply all of the Sum’ s children by the number,
        //    then return the Sum.
        if (children.size() == 2) {
            auto const& child_1 = children[0];
            auto const& child_2 = children[1];

            Optional<Number> multiplier;
            RefPtr<SumCalculationNode const> sum;

            if (child_1->type() == CalculationNode::Type::Numeric && child_2->type() == CalculationNode::Type::Sum) {
                if (auto const* maybe_multiplier = as<NumericCalculationNode>(*child_1).value().get_pointer<Number>()) {
                    multiplier = *maybe_multiplier;
                    sum = as<SumCalculationNode>(*child_2);
                }
            }
            if (child_1->type() == CalculationNode::Type::Sum && child_2->type() == CalculationNode::Type::Numeric) {
                if (auto const* maybe_multiplier = as<NumericCalculationNode>(*child_2).value().get_pointer<Number>()) {
                    multiplier = *maybe_multiplier;
                    sum = as<SumCalculationNode>(*child_1);
                }
            }

            if (multiplier.has_value() && sum) {
                Vector<NonnullRefPtr<CalculationNode const>> multiplied_children;
                multiplied_children.ensure_capacity(sum->children().size());

                bool all_numeric = true;
                for (auto const& sum_child : sum->children()) {
                    if (sum_child->type() != CalculationNode::Type::Numeric) {
                        all_numeric = false;
                        break;
                    }
                    auto& child_value = as<NumericCalculationNode>(*sum_child).value();
                    multiplied_children.append(child_value.visit(
                        [&](Percentage const& percentage) {
                            return NumericCalculationNode::create(Percentage(percentage.value() * multiplier->value()), context);
                        },
                        [&](Number const& number) {
                            return NumericCalculationNode::create(Number(Number::Type::Number, number.value() * multiplier->value()), context);
                        },
                        [&]<typename T>(T const& value) {
                            return NumericCalculationNode::create(T(value.raw_value() * multiplier->value(), value.unit()), context);
                        }));
                }

                if (all_numeric)
                    return SumCalculationNode::create(move(multiplied_children));
            }
        }

        // 4. If root contains only numeric values and/or Invert nodes containing numeric values, and multiplying the
        //    types of all the children (noting that the type of an Invert node is the inverse of its child’s type)
        //    results in a type that matches any of the types that a math function can resolve to, return the result of
        //    multiplying all the values of the children (noting that the value of an Invert node is the reciprocal of
        //    its child’s value), expressed in the result’s canonical unit.
        Optional<CalculatedStyleValue::CalculationResult> accumulated_result;
        bool is_valid = true;

        auto accumulate = [&accumulated_result, &resolution_context](NumericCalculationNode const& numeric_child, bool invert) {
            auto child_type = numeric_child.numeric_type();

            if (!child_type.has_value())
                return false;

            // FIXME: The spec doesn't cover how to handle values in non-canonical units
            if (!numeric_child.is_in_canonical_unit())
                return false;

            // AD-HOC: The spec doesn't cover how to handle unresolved percentages, to handle this we force percentages
            //         back to the percent type (e.g. { hint: None, "percent" → 1 } rather than
            //         { hint: length, "length" → 1 }), this avoids a situation calling make_calculation_node below
            //         where we would treat the value as an absolute value expressed in canonical units rather than a
            //         percent. `make_calculation_node` will still calculate the correct numeric type for the
            //         simplified node. See spec issue: https://github.com/w3c/csswg-drafts/issues/11588
            if (numeric_child.value().has<Percentage>())
                child_type = NumericType { NumericType::BaseType::Percent, 1 };

            auto child_value = CalculatedStyleValue::CalculationResult::from_value(numeric_child.value(), resolution_context, child_type);

            if (invert)
                child_value.invert();

            if (accumulated_result.has_value())
                accumulated_result->multiply_by(child_value);
            else
                accumulated_result = child_value;

            if (!accumulated_result->type().has_value())
                return false;

            return true;
        };

        for (auto const& child : children) {
            if (child->type() == CalculationNode::Type::Numeric) {
                if (!accumulate(as<NumericCalculationNode>(*child), false)) {
                    is_valid = false;
                    break;
                }
                continue;
            }
            if (child->type() == CalculationNode::Type::Invert) {
                auto const& invert_child = as<InvertCalculationNode>(*child);
                if (invert_child.child().type() != CalculationNode::Type::Numeric || !accumulate(as<NumericCalculationNode>(invert_child.child()), true)) {
                    is_valid = false;
                    break;
                }
                continue;
            }
            is_valid = false;
            break;
        }
        if (is_valid) {
            if (auto node = make_calculation_node(*accumulated_result, context))
                return node.release_nonnull();
        }

        // 5. Return root.
        // NOTE: Because our root is immutable, we have to return a new node with the modified children.
        return ProductCalculationNode::create(move(children));
    }

    // 10. Return root.
    return root;
}

}
