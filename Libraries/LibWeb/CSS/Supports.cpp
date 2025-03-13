/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/CSS/Supports.h>

namespace Web::CSS {

static void indent(StringBuilder& builder, int levels)
{
    for (int i = 0; i < levels; i++)
        builder.append("  "sv);
}

Supports::Supports(NonnullOwnPtr<Condition>&& condition)
    : m_condition(move(condition))
{
    m_matches = m_condition->evaluate();
}

bool Supports::Condition::evaluate() const
{
    switch (type) {
    case Type::Not:
        return !children.first().evaluate();
    case Type::And:
        for (auto& child : children) {
            if (!child.evaluate())
                return false;
        }
        return true;
    case Type::Or:
        for (auto& child : children) {
            if (child.evaluate())
                return true;
        }
        return false;
    }
    VERIFY_NOT_REACHED();
}

bool Supports::InParens::evaluate() const
{
    return value.visit(
        [&](NonnullOwnPtr<Condition> const& condition) {
            return condition->evaluate();
        },
        [&](Feature const& feature) {
            return feature.evaluate();
        },
        [&](GeneralEnclosed const&) {
            return false;
        });
}

bool Supports::Feature::evaluate() const
{
    return value.visit(
        [&](Declaration const& declaration) {
            return declaration.evaluate();
        },
        [&](Selector const& selector) {
            return selector.evaluate();
        });
}

String Supports::Declaration::to_string() const
{
    return MUST(String::formatted("({})", declaration));
}

String Supports::Selector::to_string() const
{
    return MUST(String::formatted("selector({})", selector));
}

String Supports::Feature::to_string() const
{
    return value.visit([](auto& it) { return it.to_string(); });
}

String Supports::InParens::to_string() const
{
    return value.visit(
        [](NonnullOwnPtr<Condition> const& condition) { return MUST(String::formatted("({})", condition->to_string())); },
        [](Supports::Feature const& it) { return it.to_string(); },
        [](GeneralEnclosed const& it) { return it.to_string(); });
}

String Supports::Condition::to_string() const
{
    switch (type) {
    case Type::Not:
        return MUST(String::formatted("not {}", children.first().to_string()));
    case Type::And:
        return MUST(String::join(" and "sv, children));
    case Type::Or:
        return MUST(String::join(" or "sv, children));
    }
    VERIFY_NOT_REACHED();
}

String Supports::to_string() const
{
    return m_condition->to_string();
}

void Supports::Declaration::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Declaration: {}\n", declaration);
}

void Supports::Selector::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    builder.appendff("Selector: {}\n", selector);
}

void Supports::Feature::dump(StringBuilder& builder, int indent_levels) const
{
    value.visit([&](auto& it) { it.dump(builder, indent_levels); });
}

void Supports::InParens::dump(StringBuilder& builder, int indent_levels) const
{
    value.visit(
        [&](NonnullOwnPtr<Condition> const& condition) { condition->dump(builder, indent_levels); },
        [&](Supports::Feature const& it) { it.dump(builder, indent_levels); },
        [&](GeneralEnclosed const& it) {
            indent(builder, indent_levels);
            builder.appendff("GeneralEnclosed: {}\n", it.to_string());
        });
}

void Supports::Condition::dump(StringBuilder& builder, int indent_levels) const
{
    indent(builder, indent_levels);
    StringView type_name = [](Type type) {
        switch (type) {
        case Type::And:
            return "AND"sv;
        case Type::Or:
            return "OR"sv;
        case Type::Not:
            return "NOT"sv;
        }
        VERIFY_NOT_REACHED();
    }(type);
    builder.appendff("Condition: {}\n", type_name);
    for (auto const& child : children)
        child.dump(builder, indent_levels + 1);
}

void Supports::dump(StringBuilder& builder, int indent_levels) const
{
    m_condition->dump(builder, indent_levels);
}

}
