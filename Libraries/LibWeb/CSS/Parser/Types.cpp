/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Types.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS::Parser {

String SimpleBlock::to_string() const
{
    StringBuilder builder;

    builder.append(token.bracket_string());
    builder.join(""sv, value);
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

void SimpleBlock::contains_arbitrary_substitution_function(SubstitutionFunctionsPresence& presence) const
{
    for (auto const& component_value : value) {
        if (component_value.is_function())
            component_value.function().contains_arbitrary_substitution_function(presence);
        if (component_value.is_block())
            component_value.block().contains_arbitrary_substitution_function(presence);
    }
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

void Function::contains_arbitrary_substitution_function(SubstitutionFunctionsPresence& presence) const
{
    if (name.equals_ignoring_ascii_case("attr"sv))
        presence.attr = true;
    else if (name.equals_ignoring_ascii_case("env"sv))
        presence.env = true;
    else if (name.equals_ignoring_ascii_case("var"sv))
        presence.var = true;
    for (auto const& component_value : value) {
        if (component_value.is_function())
            component_value.function().contains_arbitrary_substitution_function(presence);
        if (component_value.is_block())
            component_value.block().contains_arbitrary_substitution_function(presence);
    }
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
        [this](auto const& at_rule) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = MUST(String::formatted("@{}", at_rule.name)),
            });
        },
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = "qualified-rule"_fly_string,
            });
        },
        move(visit));
}

// https://drafts.csswg.org/css-syntax/#typedef-qualified-rule-list
void AtRule::for_each_as_qualified_rule_list(QualifiedRuleVisitor&& visit) const
{
    // <qualified-rule-list>: only qualified rules are allowed; declarations and at-rules are automatically invalid.
    for_each(
        [this](auto const& at_rule) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = MUST(String::formatted("@{}", at_rule.name)),
            });
        },
        move(visit),
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = "list-of-declarations"_fly_string,
            });
        });
}

// https://drafts.csswg.org/css-syntax/#typedef-at-rule-list
void AtRule::for_each_as_at_rule_list(AtRuleVisitor&& visit) const
{
    // <at-rule-list>: only at-rules are allowed; declarations and qualified rules are automatically invalid.
    for_each(
        move(visit),
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = "qualified-rule"_fly_string,
            });
        },
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = "list-of-declarations"_fly_string,
            });
        });
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-rule-list
void AtRule::for_each_as_declaration_rule_list(AtRuleVisitor&& visit_at_rule, DeclarationVisitor&& visit_declaration) const
{
    // <declaration-rule-list>: declarations and at-rules are allowed; qualified rules are automatically invalid.
    for_each(
        move(visit_at_rule),
        [this](auto const&) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = MUST(String::formatted("@{}", name)),
                .inner_rule_name = "qualified-rule"_fly_string,
            });
        },
        move(visit_declaration));
}

// https://drafts.csswg.org/css-syntax/#typedef-rule-list
void AtRule::for_each_as_rule_list(RuleVisitor&& visit) const
{
    // <rule-list>: qualified rules and at-rules are allowed; declarations are automatically invalid.
    for (auto const& child : child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) { visit(rule); },
            [&](Vector<Declaration> const&) {
                ErrorReporter::the().report(InvalidRuleLocationError {
                    .outer_rule_name = MUST(String::formatted("@{}", name)),
                    .inner_rule_name = "list-of-declarations"_fly_string,
                });
            });
    }
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-list
void QualifiedRule::for_each_as_declaration_list(FlyString const& rule_name, DeclarationVisitor&& visit) const
{
    // <declaration-list>: only declarations are allowed; at-rules and qualified rules are automatically invalid.
    for (auto const& declaration : declarations)
        visit(declaration);

    for (auto const& child : child_rules) {
        child.visit(
            [&](Rule const&) {
                ErrorReporter::the().report(InvalidRuleLocationError {
                    .outer_rule_name = rule_name,
                    .inner_rule_name = "qualified-rule"_fly_string,
                });
            },
            [&](Vector<Declaration> const& declarations) {
                for (auto const& declaration : declarations)
                    visit(declaration);
            });
    }
}

}
