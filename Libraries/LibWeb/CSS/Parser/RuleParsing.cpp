/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tommy van der Vorst <tommy@pixelspark.nl>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/HeapVector.h>
#include <LibWeb/CSS/CSSContainerRule.h>
#include <LibWeb/CSS/CSSCounterStyleRule.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSFunctionDeclarations.h>
#include <LibWeb/CSS/CSSFunctionRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSPageRule.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

static bool selector_list_contains_pseudo_element(SelectorList const& selectors)
{
    return any_of(selectors, [](auto const& selector) { return selector->target_pseudo_element().has_value(); });
}

static Vector<Descriptor> copy_descriptors(ReadonlySpan<Descriptor> descriptors)
{
    Vector<Descriptor> copy;
    copy.ensure_capacity(descriptors.size());
    for (auto const& descriptor : descriptors)
        copy.unchecked_append({ descriptor.descriptor_name_and_id, descriptor.value });
    return copy;
}

template<typename NestedDeclarationsRule>
GC::Ptr<CSSRule> Parser::convert_to_rule(Rule const& rule, Nested nested)
{
    return rule.visit(
        [this, nested](AtRule const& at_rule) -> GC::Ptr<CSSRule> {
            switch (at_rule.kind) {
            case ValueParserFFI::FfiRuleKind::Qualified:
                VERIFY_NOT_REACHED();
            case ValueParserFFI::FfiRuleKind::Unknown:
            case ValueParserFFI::FfiRuleKind::FontFeatureValuesRule:
                break;
            case ValueParserFFI::FfiRuleKind::IgnoredVendor:
                return {};
            case ValueParserFFI::FfiRuleKind::Container:
                return convert_to_container_rule<NestedDeclarationsRule>(at_rule, nested);
            case ValueParserFFI::FfiRuleKind::CounterStyle:
                return convert_to_counter_style_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::FontFace:
                return convert_to_font_face_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::FontFeatureValues:
                return convert_to_font_feature_values_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Function:
                return convert_to_function_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Import:
                return convert_to_import_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Keyframes:
                return convert_to_keyframes_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Layer:
                return convert_to_layer_rule<NestedDeclarationsRule>(at_rule, nested);
            case ValueParserFFI::FfiRuleKind::Margin:
                return convert_to_margin_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Media:
                return convert_to_media_rule<NestedDeclarationsRule>(at_rule, nested);
            case ValueParserFFI::FfiRuleKind::Namespace:
                return convert_to_namespace_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Page:
                return convert_to_page_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Property:
                return convert_to_property_rule(at_rule);
            case ValueParserFFI::FfiRuleKind::Scope:
                return convert_to_scope_rule<NestedDeclarationsRule>(at_rule, nested);
            case ValueParserFFI::FfiRuleKind::Supports:
                return convert_to_supports_rule<NestedDeclarationsRule>(at_rule, nested);
            }

            // FIXME: More at rules!
            ErrorReporter::the().report(UnknownRuleError { .rule_name = Utf16String::formatted("@{}", at_rule.name) });
            return {};
        },
        [this, nested](QualifiedRule const& qualified_rule) -> GC::Ptr<CSSRule> {
            return convert_to_style_rule(qualified_rule, nested);
        });
}

template<typename NestedDeclarationsRule>
GC::Ref<CSSRuleList> Parser::convert_child_rules(Vector<RuleOrListOfDeclarations> const& children, Nested nested)
{
    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto const& child : children) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule<NestedDeclarationsRule>(rule, nested))
                    child_rules.append(*child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(NestedDeclarationsRule::create(*this, declarations));
            });
    }
    return CSSRuleList::create(child_rules);
}

static StyleNestingParent parent_rule_for_style_nesting(Vector<RuleContext> rule_context)
{
    for (auto& context : rule_context.in_reverse()) {
        if (context == RuleContext::Style)
            return StyleNestingParent::Style;
        if (context == RuleContext::AtScope)
            return StyleNestingParent::Scope;
    }
    return StyleNestingParent::None;
}

GC::Ptr<CSSStyleRule> Parser::convert_to_style_rule(QualifiedRule const& qualified_rule, Nested nested)
{
    auto nesting_parent = parent_rule_for_style_nesting(m_rule_context);

    m_rule_context.append(RuleContext::Style);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::Style);
    };
    if (!qualified_rule.selectors.has_value()
        || selector_list_has_undeclared_namespace(*qualified_rule.selectors, m_declared_namespaces)) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "style"_utf16_fly_string,
            .prelude = qualified_rule.prelude_text.to_utf8(),
            .description = "Selectors invalid."_string,
        });
        return {};
    }

    if (qualified_rule.selectors->is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "style"_utf16_fly_string,
            .prelude = qualified_rule.prelude_text.to_utf8(),
            .description = "Empty selector."_string,
        });
        return {};
    }

    SelectorList selectors = *qualified_rule.selectors;
    if (nested == Nested::Yes)
        selectors = adapt_nested_relative_selector_list(selectors, nesting_parent);

    auto declaration = convert_to_style_declaration(qualified_rule.declarations);

    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto& child : qualified_rule.child_rules) {
        child.visit(
            [&](Rule const& rule) {
                // "In addition to nested style rules, this specification allows nested group rules inside of style rules:
                // any at-rule whose body contains style rules can be nested inside of a style rule as well."
                // https://drafts.csswg.org/css-nesting-1/#nested-group-rules
                if (auto converted_rule = convert_to_rule<CSSNestedDeclarations>(rule, Nested::Yes)) {
                    if (is<CSSGroupingRule>(*converted_rule)) {
                        child_rules.append(*converted_rule);
                    } else {
                        ErrorReporter::the().report(InvalidRuleLocationError {
                            .outer_rule_name = "style"_utf16_fly_string,
                            .inner_rule_name = Utf16FlyString::from_utf8(converted_rule->class_name()),
                        });
                    }
                }
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(CSSNestedDeclarations::create(*this, declarations));
            });
    }
    auto nested_rules = CSSRuleList::create(child_rules);
    auto style_rule = CSSStyleRule::create(move(selectors), *declaration, *nested_rules);
    style_rule->set_source_position(qualified_rule.source_position);
    return style_rule;
}

GC::Ptr<CSSImportRule> Parser::convert_to_import_rule(AtRule const& rule)
{
    if (rule.is_block_rule)
        return {};
    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Import)
        return {};

    VERIFY(!rule.parsed_prelude.items.is_empty());
    auto const& url_item = rule.parsed_prelude.items.first();
    auto url_kind = static_cast<ValueParserFFI::FfiImportPreludeItemKind>(url_item.kind);
    VERIFY(url_kind == ValueParserFFI::FfiImportPreludeItemKind::UrlFunction
        || url_kind == ValueParserFFI::FfiImportPreludeItemKind::UrlValue);
    VERIFY(url_item.style_value && url_item.style_value->is_url());
    auto url = url_item.style_value->as_url().url();

    Optional<Utf16FlyString> layer;
    bool has_scope = false;
    Optional<SelectorList> scope_start;
    Optional<SelectorList> scope_end;
    Optional<RustQueryHandle> supports;
    Vector<NonnullRefPtr<MediaQuery>> media_queries;
    for (auto const& item : rule.parsed_prelude.items.span().slice(1)) {
        switch (static_cast<ValueParserFFI::FfiImportPreludeItemKind>(item.kind)) {
        case ValueParserFFI::FfiImportPreludeItemKind::Layer:
            VERIFY(item.value.has_value());
            layer = *item.value;
            break;
        case ValueParserFFI::FfiImportPreludeItemKind::Scope:
            has_scope = true;
            break;
        case ValueParserFFI::FfiImportPreludeItemKind::ScopeStart:
            VERIFY(item.selectors.has_value());
            if (selector_list_has_undeclared_namespace(*item.selectors, m_declared_namespaces))
                return {};
            scope_start = *item.selectors;
            break;
        case ValueParserFFI::FfiImportPreludeItemKind::ScopeEnd:
            VERIFY(item.selectors.has_value());
            if (selector_list_has_undeclared_namespace(*item.selectors, m_declared_namespaces))
                return {};
            scope_end = *item.selectors;
            break;
        case ValueParserFFI::FfiImportPreludeItemKind::Supports:
            VERIFY(item.query.has_value());
            supports = RustQueryParser::reevaluate_supports_condition(*this, *item.query);
            break;
        case ValueParserFFI::FfiImportPreludeItemKind::Media:
            VERIFY(item.query.has_value());
            media_queries.append(MediaQuery::create(*item.query));
            break;
        case ValueParserFFI::FfiImportPreludeItemKind::UrlFunction:
        case ValueParserFFI::FfiImportPreludeItemKind::UrlValue:
            VERIFY_NOT_REACHED();
        }
    }

    Optional<CSSImportRule::ImportScope> scope;
    if (has_scope)
        scope = CSSImportRule::ImportScope { move(scope_start), move(scope_end) };
    return CSSImportRule::create(move(url), const_cast<DOM::Document*>(m_document.ptr()), move(layer), move(scope), move(supports), MediaList::create(move(media_queries)));
}

template<typename NestedDeclarationsRule>
GC::Ptr<CSSRule> Parser::convert_to_layer_rule(AtRule const& rule, Nested nested)
{
    m_rule_context.append(RuleContext::AtLayer);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtLayer);
    };

    // https://drafts.csswg.org/css-cascade-5/#at-layer
    if (rule.is_block_rule) {
        // CSSLayerBlockRule
        // @layer <layer-name>? {
        //   <rule-list>
        // }

        // First, the name
        if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Name || !rule.parsed_prelude.name.has_value()) {
            ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                .rule_name = "@layer"_utf16_fly_string,
                .prelude = rule.prelude_text.to_utf8(),
                .description = "Not a valid layer name."_string,
            });
            return {};
        }
        auto layer_name = rule.parsed_prelude.name.value();

        return CSSLayerBlockRule::create(layer_name, convert_child_rules<NestedDeclarationsRule>(rule.child_rules_and_lists_of_declarations, nested));
    }

    // CSSLayerStatementRule
    // @layer <layer-name>#;
    Vector<Utf16FlyString> layer_names;
    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Names) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@layer"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Contains invalid layer name."_string,
        });
        return {};
    }
    for (auto const& item : rule.parsed_prelude.items) {
        VERIFY(item.value.has_value());
        layer_names.append(item.value.value());
    }

    return CSSLayerStatementRule::create(move(layer_names));
}

GC::Ptr<CSSKeyframeRule> Parser::convert_to_keyframe_rule(QualifiedRule const& rule)
{
    if (!rule.child_rules.is_empty()) {
        for (auto const& child_rule : rule.child_rules) {
            ErrorReporter::the().report(InvalidRuleLocationError {
                .outer_rule_name = "@keyframes"_utf16_fly_string,
                .inner_rule_name = child_rule.visit(
                    [](Rule const& rule) {
                        return rule.visit(
                            [](AtRule const& at_rule) { return Utf16String::formatted("@{}", at_rule.name); },
                            [](QualifiedRule const&) { return Utf16String { "qualified-rule"_utf16_fly_string }; });
                    },
                    [](auto&) {
                        return Utf16String { "list-of-declarations"_utf16_fly_string };
                    }),
            });
        }
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::KeyframeSelectors)
        return nullptr;
    Vector<Percentage> selectors;
    selectors.ensure_capacity(rule.parsed_prelude.items.size());
    for (auto const& item : rule.parsed_prelude.items)
        selectors.unchecked_append(Percentage { item.number_value });

    PropertiesAndCustomProperties properties;
    rule.for_each_as_declaration_list("keyframe"_utf16_fly_string, [&](auto const& declaration) {
        // https://drafts.csswg.org/css-animations-1/#keyframes
        // None of the properties [in the <keyframe-block>'s <declaration-list>] interact with the cascade (so
        // using !important on them is invalid and will cause the property to be ignored).
        if (declaration.important == Important::Yes)
            return;
        extract_property(declaration, properties);
    });
    auto style = CSSStyleProperties::create(move(properties.properties), move(properties.custom_properties));

    return CSSKeyframeRule::create(move(selectors), *style);
}

GC::Ptr<CSSKeyframesRule> Parser::convert_to_keyframes_rule(AtRule const& rule)
{
    m_rule_context.append(RuleContext::AtKeyframes);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtKeyframes);
    };

    // https://drafts.csswg.org/css-animations/#keyframes
    // @keyframes = @keyframes <keyframes-name> { <qualified-rule-list> }
    // <keyframes-name> = <custom-ident> | <string>
    // <keyframe-block> = <keyframe-selector># { <declaration-list> }
    // <keyframe-selector> = from | to | <percentage [0,100]>
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Name || !rule.parsed_prelude.name.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Invalid keyframes name."_string,
        });
        return {};
    }

    auto name = rule.parsed_prelude.name.value();

    GC::RootVector<GC::Ref<CSSRule>> keyframes;
    rule.for_each_as_qualified_rule_list([&](auto& qualified_rule) {
        if (auto keyframe_rule = convert_to_keyframe_rule(qualified_rule))
            keyframes.append(*keyframe_rule);
    });

    return CSSKeyframesRule::create(name, CSSRuleList::create(keyframes));
}

GC::Ptr<CSSNamespaceRule> Parser::convert_to_namespace_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-namespaces/#syntax
    // @namespace <namespace-prefix>? [ <string> | <url> ] ;
    // <namespace-prefix> = <ident>
    if (rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@namespace"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a statement, not a block."_string,
        });
        return {};
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Namespace || !rule.parsed_prelude.secondary.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@namespace"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Invalid namespace prelude."_string,
        });
        return {};
    }

    return CSSNamespaceRule::create(rule.parsed_prelude.name, rule.parsed_prelude.secondary.value());
}

template<typename NestedDeclarationsRule>
GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule(AtRule const& rule, Nested nested)
{
    m_rule_context.append(RuleContext::AtMedia);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtMedia);
    };

    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@media"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Expected a block."_string,
        });
        return nullptr;
    }

    VERIFY(rule.parsed_prelude.kind == ParsedRulePreludeKind::MediaQueries);
    Vector<NonnullRefPtr<MediaQuery>> media_queries;
    media_queries.ensure_capacity(rule.parsed_prelude.items.size());
    for (auto const& item : rule.parsed_prelude.items) {
        VERIFY(item.query.has_value());
        media_queries.unchecked_append(MediaQuery::create(*item.query));
    }
    auto media_list = MediaList::create(move(media_queries));
    return CSSMediaRule::create(media_list, convert_child_rules<NestedDeclarationsRule>(rule.child_rules_and_lists_of_declarations, nested));
}

template<typename NestedDeclarationsRule>
GC::Ptr<CSSSupportsRule> Parser::convert_to_supports_rule(AtRule const& rule, Nested nested)
{
    m_rule_context.append(RuleContext::AtSupports);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtSupports);
    };

    // https://drafts.csswg.org/css-conditional-3/#at-supports
    // @supports <supports-condition> {
    //   <rule-list>
    // }
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return {};
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::SupportsCondition
        || rule.parsed_prelude.items.size() != 1
        || !rule.parsed_prelude.items.first().query.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Supports clause invalid."_string,
        });
        return {};
    }
    auto supports = RustQueryParser::reevaluate_supports_condition(*this, *rule.parsed_prelude.items.first().query);

    return CSSSupportsRule::create(move(supports), convert_child_rules<NestedDeclarationsRule>(rule.child_rules_and_lists_of_declarations, nested));
}

GC::Ptr<CSSPropertyRule> Parser::convert_to_property_rule(AtRule const& rule)
{
    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Property)
        return {};
    VERIFY(rule.parsed_prelude.name.has_value());
    VERIFY(rule.parsed_prelude.secondary.has_value());
    VERIFY(rule.parsed_prelude.syntax.has_value());
    VERIFY(rule.parsed_prelude.items.size() == 1);
    auto& item = rule.parsed_prelude.items.first();
    auto inherits = static_cast<ValueParserFFI::FfiPropertyPreludeItemKind>(item.kind) == ValueParserFFI::FfiPropertyPreludeItemKind::InheritsTrue;
    return CSSPropertyRule::create(*rule.parsed_prelude.name, *rule.parsed_prelude.secondary, rule.parsed_prelude.syntax.value(), inherits, item.style_value);
}

// https://drafts.csswg.org/css-cascade-6/#scope-atrule
template<typename NestedDeclarationsRule>
GC::Ptr<CSSScopeRule> Parser::convert_to_scope_rule(AtRule const& rule, Nested nested)
{
    auto nesting_parent = parent_rule_for_style_nesting(m_rule_context);
    m_rule_context.append(RuleContext::AtScope);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtScope);
    };
    if (!rule.is_block_rule)
        return nullptr;
    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Scope)
        return nullptr;
    Optional<SelectorList> start;
    Optional<SelectorList> end;
    for (auto const& item : rule.parsed_prelude.items) {
        VERIFY(item.selectors.has_value());
        bool is_end = static_cast<ValueParserFFI::FfiScopePreludeItemKind>(item.kind) == ValueParserFFI::FfiScopePreludeItemKind::End;
        auto const& selectors = *item.selectors;
        if (selectors.is_empty() || selector_list_contains_pseudo_element(selectors)
            || selector_list_has_undeclared_namespace(selectors, m_declared_namespaces))
            return nullptr;
        if (is_end)
            end = selectors;
        else
            start = selectors;
    }
    if (nested == Nested::Yes && start.has_value())
        start = adapt_nested_relative_selector_list(*start, nesting_parent);
    return CSSScopeRule::create(move(start), move(end), convert_child_rules<NestedDeclarationsRule>(rule.child_rules_and_lists_of_declarations, Nested::Yes));
}

// https://drafts.csswg.org/css-conditional-5/#container-rule
template<typename NestedDeclarationsRule>
GC::Ptr<CSSContainerRule> Parser::convert_to_container_rule(AtRule const& rule, Nested nested)
{
    m_rule_context.append(RuleContext::AtContainer);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtContainer);
    };

    // @container <container-condition># {
    //   <rule-list>
    // }
    // <container-condition> = [ <container-name>? <container-query>? ]!
    // <container-name> = <custom-ident>

    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@container"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::ContainerConditions) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@container"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Invalid container condition list."_string,
        });
        return nullptr;
    }

    Vector<CSSContainerRule::Condition> conditions;
    conditions.ensure_capacity(rule.parsed_prelude.items.size());
    for (auto const& item : rule.parsed_prelude.items) {
        RefPtr<ContainerQuery> query;
        if (item.query.has_value())
            query = ContainerQuery::create(*item.query);
        conditions.unchecked_empend(item.value, move(query));
    }

    return CSSContainerRule::create(move(conditions), convert_child_rules<NestedDeclarationsRule>(rule.child_rules_and_lists_of_declarations, nested));
}

GC::Ptr<CSSCounterStyleRule> Parser::convert_to_counter_style_rule(AtRule const& rule)
{
    m_rule_context.append(RuleContext::AtCounterStyle);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtCounterStyle);
    };

    // https://drafts.csswg.org/css-counter-styles-3/#the-counter-style-rule
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Name || !rule.parsed_prelude.name.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Missing counter style name."_string,
        });
        return nullptr;
    }
    auto name = rule.parsed_prelude.name.value();

    auto descriptor_value = [&rule](DescriptorID id) -> RefPtr<StyleValue const> {
        auto descriptor = rule.descriptors.first_matching([id](auto const& descriptor) {
            return descriptor.descriptor_name_and_id.id() == id;
        });
        if (!descriptor.has_value())
            return nullptr;
        return descriptor->value;
    };

    return CSSCounterStyleRule::create(move(name), descriptor_value(DescriptorID::System), descriptor_value(DescriptorID::Negative), descriptor_value(DescriptorID::Prefix), descriptor_value(DescriptorID::Suffix), descriptor_value(DescriptorID::Range), descriptor_value(DescriptorID::Pad), descriptor_value(DescriptorID::Fallback), descriptor_value(DescriptorID::Symbols), descriptor_value(DescriptorID::AdditiveSymbols), descriptor_value(DescriptorID::SpeakAs));
}

GC::Ptr<CSSFontFaceRule> Parser::convert_to_font_face_rule(AtRule const& rule)
{
    m_rule_context.append(RuleContext::AtFontFace);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtFontFace);
    };

    // https://drafts.csswg.org/css-fonts/#font-face-rule
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@font-face"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Empty) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@font-face"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Prelude is not allowed."_string,
        });
        return {};
    }

    auto font_face_descriptors = CSSFontFaceDescriptors::create(copy_descriptors(rule.descriptors));
    return CSSFontFaceRule::create(font_face_descriptors);
}

GC::Ptr<CSSFontFeatureValuesRule> Parser::convert_to_font_feature_values_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-fonts-4/#font-feature-values-syntax
    // @font-feature-values = @font-feature-values <family-name># { <declaration-rule-list> }
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@font-feature-values"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::FontFamilyNames)
        return nullptr;
    Vector<Utf16FlyString> family_names;
    family_names.ensure_capacity(rule.parsed_prelude.items.size());
    for (auto const& item : rule.parsed_prelude.items) {
        VERIFY(item.value.has_value());
        family_names.unchecked_append(item.value.value());
    }

    auto font_feature_values_rule = CSSFontFeatureValuesRule::create(move(family_names));

    rule.for_each_as_declaration_rule_list(
        [&](AtRule const& at_rule) {
            // <font-feature-value-type> = <@stylistic> | <@historical-forms> | <@styleset> | <@character-variant> | <@swash> | <@ornaments> | <@annotation>
            // @stylistic = @stylistic { <declaration-list> }
            // @historical-forms = @historical-forms { <declaration-list> }
            // @styleset = @styleset { <declaration-list> }
            // @character-variant = @character-variant { <declaration-list> }
            // @swash = @swash { <declaration-list> }
            // @ornaments = @ornaments { <declaration-list> }
            // @annotation = @annotation { <declaration-list> }

            if (at_rule.parsed_prelude.kind != ParsedRulePreludeKind::FontFeatureValuesRule || at_rule.parsed_prelude.items.size() != 1)
                return;
            GC::Ref<CSSFontFeatureValuesMap> feature_values_map = [&] {
                switch (static_cast<ValueParserFFI::FfiFontFeatureValuesRuleKind>(at_rule.parsed_prelude.items.first().kind)) {
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::Annotation:
                    return font_feature_values_rule->annotation();
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::CharacterVariant:
                    return font_feature_values_rule->character_variant();
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::HistoricalForms:
                    return font_feature_values_rule->historical_forms();
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::Ornaments:
                    return font_feature_values_rule->ornaments();
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::Styleset:
                    return font_feature_values_rule->styleset();
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::Stylistic:
                    return font_feature_values_rule->stylistic();
                case ValueParserFFI::FfiFontFeatureValuesRuleKind::Swash:
                    return font_feature_values_rule->swash();
                }
                VERIFY_NOT_REACHED();
            }();

            at_rule.for_each_as_declaration_list([&](Declaration const& declaration) {
                if (!declaration.font_feature_values.has_value())
                    return;
                MUST(feature_values_map->set(declaration.name.to_utf16_string(), declaration.font_feature_values.value()));
            });
        },
        [&](Declaration const&) {
            // FIXME: Handle the `font-display` descriptor here, see
            //        https://drafts.csswg.org/css-fonts-4/#font-display-font-feature-values
        });

    return font_feature_values_rule;
}

GC::Ptr<CSSFunctionRule> Parser::convert_to_function_rule(AtRule const& function_rule)
{
    m_rule_context.append(RuleContext::AtFunction);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtFunction);
    };

    // https://drafts.csswg.org/css-mixins-1/#function-rule
    if (!function_rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@function"_utf16_fly_string,
            .prelude = function_rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (function_rule.parsed_prelude.kind != ParsedRulePreludeKind::Function)
        return nullptr;
    VERIFY(function_rule.parsed_prelude.name.has_value());
    VERIFY(function_rule.parsed_prelude.syntax.has_value());

    Vector<FunctionParameterInternal> parameters;
    parameters.ensure_capacity(function_rule.parsed_prelude.items.size());
    for (auto const& item : function_rule.parsed_prelude.items) {
        VERIFY(static_cast<ValueParserFFI::FfiFunctionParameterItemKind>(item.kind) == ValueParserFFI::FfiFunctionParameterItemKind::Parameter);
        VERIFY(item.value.has_value());
        VERIFY(item.syntax.has_value());
        parameters.append({ *item.value, item.syntax.value(), item.style_value });
    }

    // https://drafts.csswg.org/css-mixins-1/#function-body
    return CSSFunctionRule::create(convert_child_rules<CSSFunctionDeclarations>(function_rule.child_rules_and_lists_of_declarations, Nested::Yes), *function_rule.parsed_prelude.name, move(parameters), function_rule.parsed_prelude.syntax.value());
}

GC::Ptr<CSSPageRule> Parser::convert_to_page_rule(AtRule const& page_rule)
{
    m_rule_context.append(RuleContext::AtPage);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtPage);
    };

    // https://drafts.csswg.org/css-page-3/#syntax-page-selector
    // @page = @page <page-selector-list>? { <declaration-rule-list> }
    if (!page_rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@page"_utf16_fly_string,
            .prelude = page_rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (page_rule.parsed_prelude.kind != ParsedRulePreludeKind::PageSelectors)
        return nullptr;
    auto page_selectors = page_selector_list_from_parsed_prelude(page_rule.parsed_prelude);

    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    page_rule.for_each_as_declaration_rule_list(
        [&](auto& at_rule) {
            if (auto converted_rule = convert_to_rule<CSSNestedDeclarations>(at_rule, Nested::No)) {
                if (is<CSSMarginRule>(*converted_rule)) {
                    child_rules.append(*converted_rule);
                } else {
                    ErrorReporter::the().report(InvalidRuleLocationError {
                        .outer_rule_name = "@page"_utf16_fly_string,
                        .inner_rule_name = Utf16FlyString::from_utf8(converted_rule->class_name()),
                    });
                }
            }
        },
        [](auto&) {});

    auto rule_list = CSSRuleList::create(child_rules);
    return CSSPageRule::create(move(page_selectors), CSSPageDescriptors::create(copy_descriptors(page_rule.descriptors)), rule_list);
}

GC::Ptr<CSSMarginRule> Parser::convert_to_margin_rule(AtRule const& rule)
{
    m_rule_context.append(RuleContext::Margin);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::Margin);
    };

    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = Utf16String::formatted("@{}", rule.name),
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Empty) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = Utf16String::formatted("@{}", rule.name),
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Prelude is not allowed."_string,
        });
        return {};
    }

    // https://drafts.csswg.org/css-page-3/#syntax-page-selector
    // There are lots of these, but they're all in the format:
    // @foo = @foo { <declaration-list> };

    // FIXME: The declaration list should be a CSSMarginDescriptors, but that has no spec definition:
    //        https://github.com/w3c/csswg-drafts/issues/10106
    //        So, we just parse a CSSStyleProperties instead for now.
    PropertiesAndCustomProperties properties;
    rule.for_each_as_declaration_list([&](auto const& declaration) {
        extract_property(declaration, properties);
    });
    auto style = CSSStyleProperties::create(move(properties.properties), move(properties.custom_properties));
    return CSSMarginRule::create(rule.name, style);
}

template<typename Descriptors>
GC::Ref<Descriptors> Parser::convert_to_descriptors(AtRuleID, Vector<Declaration> const& declarations)
{
    Vector<Descriptor> descriptors;
    descriptors.ensure_capacity(declarations.size());
    for (auto const& declaration : declarations) {
        if (!declaration.descriptor_name_and_id.has_value() || !declaration.parsed_value)
            continue;
        descriptors.unchecked_append({ declaration.descriptor_name_and_id.value(), NonnullRefPtr { *declaration.parsed_value } });
    }
    return Descriptors::create(move(descriptors));
}

template GC::Ref<CSSFunctionDescriptors> Parser::convert_to_descriptors(AtRuleID at_rule_id, Vector<Declaration> const& declarations);

template GC::Ptr<CSSRule> Parser::convert_to_rule<CSSNestedDeclarations>(Rule const&, Nested);
template GC::Ptr<CSSRule> Parser::convert_to_rule<CSSFunctionDeclarations>(Rule const&, Nested);

template GC::Ptr<CSSContainerRule> Parser::convert_to_container_rule<CSSNestedDeclarations>(AtRule const&, Nested);
template GC::Ptr<CSSContainerRule> Parser::convert_to_container_rule<CSSFunctionDeclarations>(AtRule const&, Nested);
template GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule<CSSNestedDeclarations>(AtRule const&, Nested);
template GC::Ptr<CSSMediaRule> Parser::convert_to_media_rule<CSSFunctionDeclarations>(AtRule const&, Nested);
template GC::Ptr<CSSScopeRule> Parser::convert_to_scope_rule<CSSNestedDeclarations>(AtRule const&, Nested);
template GC::Ptr<CSSScopeRule> Parser::convert_to_scope_rule<CSSFunctionDeclarations>(AtRule const&, Nested);

template GC::Ptr<CSSRule> Parser::convert_to_layer_rule<CSSNestedDeclarations>(AtRule const& rule, Nested);

template GC::Ptr<CSSSupportsRule> Parser::convert_to_supports_rule<CSSNestedDeclarations>(AtRule const&, Nested);
template GC::Ptr<CSSSupportsRule> Parser::convert_to_supports_rule<CSSFunctionDeclarations>(AtRule const&, Nested);

}
