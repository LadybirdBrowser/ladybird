/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/Supports.h>

namespace Web::CSS {

Supports::Supports(NonnullOwnPtr<BooleanExpression>&& condition)
    : m_condition(move(condition))
{
    m_matches = m_condition->evaluate_to_boolean(nullptr);
}

MatchResult Supports::Declaration::evaluate(HTML::Window const*) const
{
    return as_match_result(m_matches);
}

String Supports::Declaration::to_string() const
{
    return MUST(String::formatted("({})", m_declaration));
}

void Supports::Declaration::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Declaration: `{}`, matches={}\n", m_declaration, m_matches);
}

MatchResult Supports::Selector::evaluate(HTML::Window const*) const
{
    return as_match_result(m_matches);
}

String Supports::Selector::to_string() const
{
    return MUST(String::formatted("selector({})", m_selector));
}

void Supports::Selector::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Selector: `{}` matches={}\n", m_selector, m_matches);
}

String Supports::to_string() const
{
    return m_condition->to_string();
}

void Supports::dump(StringBuilder& builder, int indent_levels) const
{
    m_condition->dump(builder, indent_levels);
}

}
