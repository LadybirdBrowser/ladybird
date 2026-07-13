/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/CSS/BooleanExpression.h>

namespace Web::CSS {

bool BooleanExpression::evaluate_to_boolean(BooleanExpressionEvaluationContext const& context) const
{
    return evaluate(context) == MatchResult::True;
}

void BooleanExpression::indent(StringBuilder& builder, int levels)
{
    builder.append_repeated("  "sv, levels);
}

Utf16String BooleanExpression::to_string() const
{
    Utf16StringBuilder builder;
    serialize_to(builder);
    return builder.to_string();
}

void GeneralEnclosed::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    requirements.has_unknown_or_unsupported_feature = true;
}

void GeneralEnclosed::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append(m_serialized_contents.utf16_view());
}

void GeneralEnclosed::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("GeneralEnclosed: {}\n", to_string().to_utf8());
}

MatchResult BooleanNotExpression::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    // https://drafts.csswg.org/css-values-5/#boolean-logic
    // `not test` evaluates to true if its contained test is false, false if it’s true, and unknown if it’s unknown.
    switch (m_child->evaluate(context)) {
    case MatchResult::False:
        return MatchResult::True;
    case MatchResult::True:
        return MatchResult::False;
    case MatchResult::Unknown:
        return MatchResult::Unknown;
    }
    VERIFY_NOT_REACHED();
}

void BooleanNotExpression::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    m_child->collect_container_query_feature_requirements(requirements);
}

void BooleanNotExpression::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("not "sv);
    m_child->serialize_to(builder);
}

void BooleanNotExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("NOT:\n"sv);
    m_child->dump(builder, indent_levels + 1);
}

MatchResult BooleanExpressionInParens::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    return m_child->evaluate(context);
}

void BooleanExpressionInParens::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    m_child->collect_container_query_feature_requirements(requirements);
}

void BooleanExpressionInParens::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii('(');
    m_child->serialize_to(builder);
    builder.append_ascii(')');
}

void BooleanExpressionInParens::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("(\n"sv);
    m_child->dump(builder, indent_levels + 1);
    indent(builder, indent_levels);
    builder.append(")\n"sv);
}

MatchResult BooleanAndExpression::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    // https://drafts.csswg.org/css-values-5/#boolean-logic
    // Multiple tests connected with `and` evaluate to true if all of those tests are true, false if any of them are
    // false, and unknown otherwise (i.e. if at least one unknown, but no false).
    size_t true_results = 0;
    for (auto const& child : m_children) {
        auto child_match = child->evaluate(context);
        if (child_match == MatchResult::False)
            return MatchResult::False;
        if (child_match == MatchResult::True)
            true_results++;
    }
    if (true_results == m_children.size())
        return MatchResult::True;
    return MatchResult::Unknown;
}

void BooleanAndExpression::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    for (auto const& child : m_children)
        child->collect_container_query_feature_requirements(requirements);
}

void BooleanAndExpression::serialize_to(Utf16StringBuilder& builder) const
{
    bool first = true;
    for (auto const& child : m_children) {
        if (first)
            first = false;
        else
            builder.append_ascii(" and "sv);
        child->serialize_to(builder);
    }
}

void BooleanAndExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("AND:\n"sv);
    for (auto const& child : m_children)
        child->dump(builder, indent_levels + 1);
}

MatchResult BooleanOrExpression::evaluate(BooleanExpressionEvaluationContext const& context) const
{
    // https://drafts.csswg.org/css-values-5/#boolean-logic
    // Multiple tests connected with `or` evaluate to true if any of those tests are true, false if all of them are
    // false, and unknown otherwise (i.e. at least one unknown, but no true).
    size_t false_results = 0;
    for (auto const& child : m_children) {
        auto child_match = child->evaluate(context);
        if (child_match == MatchResult::True)
            return MatchResult::True;
        if (child_match == MatchResult::False)
            false_results++;
    }
    if (false_results == m_children.size())
        return MatchResult::False;
    return MatchResult::Unknown;
}

void BooleanOrExpression::collect_container_query_feature_requirements(ContainerQueryFeatureRequirements& requirements) const
{
    for (auto const& child : m_children)
        child->collect_container_query_feature_requirements(requirements);
}

void BooleanOrExpression::serialize_to(Utf16StringBuilder& builder) const
{
    bool first = true;
    for (auto const& child : m_children) {
        if (first)
            first = false;
        else
            builder.append_ascii(" or "sv);
        child->serialize_to(builder);
    }
}

void BooleanOrExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("OR:\n"sv);
    for (auto const& child : m_children)
        child->dump(builder, indent_levels + 1);
}

void ConstantBooleanExpression::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii(CSS::to_string(m_value));
}

void ConstantBooleanExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("CONSTANT: {}\n", to_string());
}

}
