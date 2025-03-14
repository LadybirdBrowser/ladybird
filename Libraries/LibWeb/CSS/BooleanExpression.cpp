/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/CSS/BooleanExpression.h>

namespace Web::CSS {

bool BooleanExpression::evaluate_to_boolean(HTML::Window const* window) const
{
    return evaluate(window) == MatchResult::True;
}

void BooleanExpression::indent(StringBuilder& builder, int levels)
{
    builder.append_repeated("  "sv, levels);
}

void GeneralEnclosed::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("GeneralEnclosed: {}\n", to_string());
}

MatchResult BooleanNotExpression::evaluate(HTML::Window const* window) const
{
    // https://drafts.csswg.org/css-values-5/#boolean-logic
    // `not test` evaluates to true if its contained test is false, false if it’s true, and unknown if it’s unknown.
    switch (m_child->evaluate(window)) {
    case MatchResult::False:
        return MatchResult::True;
    case MatchResult::True:
        return MatchResult::False;
    case MatchResult::Unknown:
        return MatchResult::Unknown;
    }
    VERIFY_NOT_REACHED();
}

String BooleanNotExpression::to_string() const
{
    return MUST(String::formatted("not {}", m_child->to_string()));
}

void BooleanNotExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("NOT:\n"sv);
    m_child->dump(builder, indent_levels + 1);
}

MatchResult BooleanExpressionInParens::evaluate(HTML::Window const* window) const
{
    return m_child->evaluate(window);
}

String BooleanExpressionInParens::to_string() const
{
    return MUST(String::formatted("({})", m_child->to_string()));
}

void BooleanExpressionInParens::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("(\n"sv);
    m_child->dump(builder, indent_levels + 1);
    indent(builder, indent_levels);
    builder.append(")\n"sv);
}

MatchResult BooleanAndExpression::evaluate(HTML::Window const* window) const
{
    // https://drafts.csswg.org/css-values-5/#boolean-logic
    // Multiple tests connected with `and` evaluate to true if all of those tests are true, false if any of them are
    // false, and unknown otherwise (i.e. if at least one unknown, but no false).
    size_t true_results = 0;
    for (auto const& child : m_children) {
        auto child_match = child->evaluate(window);
        if (child_match == MatchResult::False)
            return MatchResult::False;
        if (child_match == MatchResult::True)
            true_results++;
    }
    if (true_results == m_children.size())
        return MatchResult::True;
    return MatchResult::Unknown;
}

String BooleanAndExpression::to_string() const
{
    return MUST(String::join(" and "sv, m_children));
}

void BooleanAndExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("AND:\n"sv);
    for (auto const& child : m_children)
        child->dump(builder, indent_levels + 1);
}

MatchResult BooleanOrExpression::evaluate(HTML::Window const* window) const
{
    // https://drafts.csswg.org/css-values-5/#boolean-logic
    // Multiple tests connected with `or` evaluate to true if any of those tests are true, false if all of them are
    // false, and unknown otherwise (i.e. at least one unknown, but no true).
    size_t false_results = 0;
    for (auto const& child : m_children) {
        auto child_match = child->evaluate(window);
        if (child_match == MatchResult::True)
            return MatchResult::True;
        if (child_match == MatchResult::False)
            false_results++;
    }
    if (false_results == m_children.size())
        return MatchResult::False;
    return MatchResult::Unknown;
}

String BooleanOrExpression::to_string() const
{
    return MUST(String::join(" or "sv, m_children));
}

void BooleanOrExpression::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.append("OR:\n"sv);
    for (auto const& child : m_children)
        child->dump(builder, indent_levels + 1);
}

}
