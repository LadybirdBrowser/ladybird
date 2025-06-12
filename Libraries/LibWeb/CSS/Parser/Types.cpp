/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Types.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS::Parser {

String Declaration::to_string() const
{
    if (original_text.has_value())
        return original_text.value();

    StringBuilder builder;

    serialize_an_identifier(builder, name);
    builder.append(": "sv);
    builder.join(' ', value);

    if (important == Important::Yes)
        builder.append(" !important"sv);

    return MUST(builder.to_string());
}

String SimpleBlock::to_string() const
{
    StringBuilder builder;

    builder.append(token.bracket_string());
    builder.join(' ', value);
    builder.append(token.bracket_mirror_string());

    return builder.to_string_without_validation();
}

String SimpleBlock::original_source_text() const
{
    StringBuilder builder;
    builder.append(token.original_source_text());
    for (auto const& component_value : value) {
        builder.append(component_value.original_source_text());
    }
    builder.append(end_token.original_source_text());
    return builder.to_string_without_validation();
}

bool SimpleBlock::contains_arbitrary_substitution_function() const
{
    for (auto const& component_value : value) {
        if (component_value.is_function() && component_value.function().contains_arbitrary_substitution_function())
            return true;
        if (component_value.is_block() && component_value.block().contains_arbitrary_substitution_function())
            return true;
    }
    return false;
}

String Function::to_string() const
{
    StringBuilder builder;

    serialize_an_identifier(builder, name);
    builder.append('(');
    for (auto& item : value)
        builder.append(item.to_string());
    builder.append(')');

    return builder.to_string_without_validation();
}

String Function::original_source_text() const
{
    StringBuilder builder;
    builder.append(name_token.original_source_text());
    for (auto const& component_value : value) {
        builder.append(component_value.original_source_text());
    }
    builder.append(end_token.original_source_text());
    return builder.to_string_without_validation();
}

bool Function::contains_arbitrary_substitution_function() const
{
    if (name.equals_ignoring_ascii_case("var"sv) || name.equals_ignoring_ascii_case("attr"sv))
        return true;
    for (auto const& component_value : value) {
        if (component_value.is_function() && component_value.function().contains_arbitrary_substitution_function())
            return true;
        if (component_value.is_block() && component_value.block().contains_arbitrary_substitution_function())
            return true;
    }
    return false;
}

void AtRule::for_each(AtRuleVisitor&& visit_at_rule, QualifiedRuleVisitor&& visit_qualified_rule, DeclarationVisitor&& visit_declaration) const
{
    for (auto const& child : child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                rule.visit(
                    [&](AtRule const& at_rule) { visit_at_rule(at_rule); },
                    [&](QualifiedRule const& qualified_rule) { visit_qualified_rule(qualified_rule); });
            },
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit_declaration(declaration);
            });
    }
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-list
void AtRule::for_each_as_declaration_list(DeclarationVisitor&& visit) const
{
    // <declaration-list>: only declarations are allowed; at-rules and qualified rules are automatically invalid.
    for_each(
        [](auto const& at_rule) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal @{} rule in `<declaration-list>`; discarding.", at_rule.name); },
        [](auto const&) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal qualified rule in `<declaration-list>`; discarding."); },
        move(visit));
}

// https://drafts.csswg.org/css-syntax/#typedef-qualified-rule-list
void AtRule::for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const
{
    // <qualified-rule-list>: only qualified rules are allowed; declarations and at-rules are automatically invalid.
    for_each(
        [](auto const& at_rule) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal @{} rule in `<qualified-rule-list>`; discarding.", at_rule.name); },
        move(visit),
        [](auto const&) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal list of declarations in `<qualified-rule-list>`; discarding."); });
}

// https://drafts.csswg.org/css-syntax/#typedef-at-rule-list
void AtRule::for_each_as_at_rule_list(AtRuleVisitor&& visit) const
{
    // <at-rule-list>: only at-rules are allowed; declarations and qualified rules are automatically invalid.
    for_each(
        move(visit),
        [](auto const&) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal qualified rule in `<at-rule-list>`; discarding."); },
        [](auto const&) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal list of declarations in `<at-rule-list>`; discarding."); });
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-rule-list
void AtRule::for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const
{
    // <declaration-rule-list>: declarations and at-rules are allowed; qualified rules are automatically invalid.
    for_each(
        move(visit_at_rule),
        [](auto const&) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal qualified rule in `<declaration-rule-list>`; discarding."); },
        move(visit_declaration));
}

// https://drafts.csswg.org/css-syntax/#typedef-rule-list
void AtRule::for_each_as_rule_list(RuleVisitor&& visit) const
{
    // <rule-list>: qualified rules and at-rules are allowed; declarations are automatically invalid.
    for (auto const& child : child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) { visit(rule); },
            [&](Vector<Declaration> const&) { dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal list of declarations in `<rule-list>`; discarding."); });
    }
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-list
void QualifiedRule::for_each_as_declaration_list(DeclarationVisitor&& visit) const
{
    // <declaration-list>: only declarations are allowed; at-rules and qualified rules are automatically invalid.
    for (auto const& declaration : declarations)
        visit(declaration);

    for (auto const& child : child_rules) {
        child.visit(
            [&](Rule const&) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Found illegal qualified rule in `<declaration-list>`; discarding.");
            },
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit(declaration);
            });
    }
}

}
