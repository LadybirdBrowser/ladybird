/*
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetState.h>

namespace Web::CSS {

using Parser::ValueParserFFI::NativeRule;

// NB: Implemented in Libraries/LibWeb/Bindings/Rust/src/css/exports.rs.
extern "C" void* web_css_rule_child_rules(void* vm, void* rule);

// NB: What the implementation of CSSGroupingRule in Rust asks of the C++ code of LibWeb.
extern "C" {
NativeRule const* web_css_grouping_rule_parse_rule(void* rule, FlatPtr text, bool* declarations_failed);
void web_css_grouping_rule_inserted(void* rule, NativeRule const* inserted);
void web_css_grouping_rule_removed(void* rule, NativeRule const* removed);
}

CSSRuleList& CSSGroupingRule::css_rules() const
{
    return *static_cast<CSSRuleList*>(web_css_rule_child_rules(JS::HostABI::vm_to_abi(JS::VM::the()), const_cast<CSSGroupingRule*>(this)));
}

static CSSRule& rule_from_cell(void* cell)
{
    return *static_cast<CSSRule*>(cell);
}

static Vector<Parser::RuleContext> rule_context(CSSRule& owner_rule)
{
    Vector<Parser::RuleContext> context;
    for (auto* rule = &owner_rule; rule; rule = rule->parent_rule())
        context.append(Parser::rule_context_type_for_rule(rule->type()));
    context.reverse();
    return context;
}

// https://drafts.csswg.org/cssom/#insert-a-css-rule
NativeRule const* web_css_grouping_rule_parse_rule(void* cell, FlatPtr raw_text, bool* declarations_failed)
{
    auto& owner_rule = rule_from_cell(cell);
    auto text = Utf16String::adopt_raw(raw_text);
    auto rule = text.utf16_view();

    // AD-HOC: The spec doesn't include a declared_namespaces parameter, but we need it to handle parsing of namespaced
    //         selectors.
    RustNamespaceContext declared_namespaces;
    if (auto* sheet = owner_rule.parent_style_sheet())
        declared_namespaces = sheet->declared_namespaces();

    // 3. Set new rule to the results of performing parse a CSS rule on argument rule.
    Parser::ParsingParams parsing_params {};
    parsing_params.rule_context = rule_context(owner_rule);
    parsing_params.declared_namespaces = declared_namespaces;
    auto new_rule = parse_css_rule(parsing_params, rule, true);
    if (!new_rule.has_value()) {
        auto trimmed_rule = rule.trim(" \t\n\f\r"sv, TrimMode::Left);
        if (trimmed_rule.starts_with("@import"sv) || trimmed_rule.starts_with("@namespace"sv)) {
            auto top_level_params = parsing_params;
            top_level_params.rule_context.clear();
            new_rule = parse_css_rule(top_level_params, rule);
        }
    }

    // 4. If new rule is a syntax error, and nested is set, perform the following substeps:
    if (!new_rule.has_value()) {
        // - Set declarations to the results of performing parse a CSS declaration block, on argument rule.
        auto declarations = parse_css_property_declaration_block(parsing_params, rule);

        // - If declarations is empty, throw a SyntaxError exception.
        if (declarations.is_empty()) {
            *declarations_failed = true;
            return nullptr;
        }

        // - Otherwise, set new rule to a new nested declarations rule with declarations as it contents.
        new_rule = RustRule { declarations };
    }

    return Parser::ValueParserFFI::rust_rule_retain(new_rule->handle());
}

void web_css_grouping_rule_inserted(void* cell, NativeRule const* inserted)
{
    auto& rule = rule_from_cell(cell);
    auto* sheet = rule.parent_style_sheet();
    if (!sheet)
        return;
    sheet->invalidate_image_resource_registration();
    if (auto* function = Parser::ValueParserFFI::rust_rule_containing_function(rule.native_rule().handle()))
        record_style_rule_declarations_changed(RustRule { function }, *sheet);
    else
        record_style_rule_inserted(RustRule { inserted }, *sheet);
    sheet->invalidate_owners();
    sheet->synchronize_fonts_after_rule_change();
}

void web_css_grouping_rule_removed(void* cell, NativeRule const* removed)
{
    auto& rule = rule_from_cell(cell);
    auto* sheet = rule.parent_style_sheet();
    if (!sheet)
        return;
    if (auto* function = Parser::ValueParserFFI::rust_rule_containing_function(rule.native_rule().handle()))
        record_style_rule_declarations_changed(RustRule { function }, *sheet);
    else
        record_style_rule_removed(*sheet, RustRule { removed });
    sheet->invalidate_owners();
    sheet->synchronize_fonts_after_rule_change();
}

}
