/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/Dump.h>

namespace Web::CSS {

Supports::Supports(NonnullOwnPtr<BooleanExpression>&& condition)
    : m_condition(move(condition))
{
    m_matches = m_condition->evaluate_to_boolean({});
}

MatchResult Supports::Declaration::evaluate(BooleanExpressionEvaluationContext const&) const
{
    return as_match_result(m_matches);
}

void Supports::Declaration::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append(m_declaration.utf16_view());
}

void Supports::Declaration::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Declaration: `{}`, matches={}\n", m_declaration, m_matches);
}

MatchResult Supports::Selector::evaluate(BooleanExpressionEvaluationContext const&) const
{
    return as_match_result(m_matches);
}

void Supports::Selector::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("selector("sv);
    builder.append(m_selector.utf16_view());
    builder.append_ascii(')');
}

void Supports::Selector::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Selector: `{}` matches={}\n", m_selector, m_matches);
}

MatchResult Supports::FontTech::evaluate(BooleanExpressionEvaluationContext const&) const
{
    return as_match_result(m_matches);
}

void Supports::FontTech::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("font-tech("sv);
    builder.append(m_tech.view());
    builder.append_ascii(')');
}

void Supports::FontTech::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("FontTech: `{}` matches={}\n", m_tech, m_matches);
}

MatchResult Supports::FontFormat::evaluate(BooleanExpressionEvaluationContext const&) const
{
    return as_match_result(m_matches);
}

void Supports::FontFormat::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("font-format("sv);
    builder.append(m_format.view());
    builder.append_ascii(')');
}

void Supports::FontFormat::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("FontFormat: `{}` matches={}\n", m_format, m_matches);
}

MatchResult Supports::Env::evaluate(BooleanExpressionEvaluationContext const&) const
{
    return as_match_result(m_matches);
}

void Supports::Env::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("font-format("sv);
    serialize_an_identifier(builder, m_variable_name);
    builder.append_ascii(')');
}

void Supports::Env::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Env: `{}` matches={}\n", m_variable_name, m_matches);
}

MatchResult Supports::AtRule::evaluate(BooleanExpressionEvaluationContext const&) const
{
    return as_match_result(m_matches);
}

void Supports::AtRule::serialize_to(Utf16StringBuilder& builder) const
{
    builder.append_ascii("at-rule(@"sv);
    serialize_an_identifier(builder, m_name);
    builder.append_ascii(')');
}

void Supports::AtRule::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("AtRule: `@{}` matches={}\n", m_name, m_matches);
}

Utf16String Supports::to_string() const
{
    return m_condition->to_string();
}

void Supports::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Supports condition: (matches = {})\n", m_matches);
    m_condition->dump(builder, indent_levels + 1);
}

}
