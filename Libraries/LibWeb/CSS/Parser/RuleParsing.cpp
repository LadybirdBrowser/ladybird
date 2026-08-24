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
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

static bool selector_list_contains_pseudo_element(SelectorList const& selectors)
{
    return any_of(selectors, [](auto const& selector) { return selector->target_pseudo_element().has_value(); });
}

// A helper that ensures only the last instance of each descriptor is included, while also handling shorthands.
class DescriptorList {
public:
    DescriptorList(AtRuleID at_rule)
        : m_at_rule(at_rule)
    {
    }

    void append(Descriptor&& descriptor)
    {
        if (is_shorthand(m_at_rule, descriptor.descriptor_name_and_id)) {
            for_each_expanded_longhand(m_at_rule, descriptor.descriptor_name_and_id, descriptor.value, [this](auto longhand_id, auto longhand_value) {
                append_internal(Descriptor { longhand_id, longhand_value.release_nonnull() });
            });
            return;
        }

        append_internal(move(descriptor));
    }

    Vector<Descriptor> release_descriptors()
    {
        return move(m_descriptors);
    }

private:
    void append_internal(Descriptor&& descriptor)
    {
        if (m_seen_descriptor_ids.contains(descriptor.descriptor_name_and_id)) {
            m_descriptors.remove_first_matching([&descriptor](Descriptor const& existing) {
                return existing.descriptor_name_and_id == descriptor.descriptor_name_and_id;
            });
        } else {
            m_seen_descriptor_ids.set(descriptor.descriptor_name_and_id);
        }
        m_descriptors.append(move(descriptor));
    }

    AtRuleID m_at_rule;
    Vector<Descriptor> m_descriptors;
    HashTable<DescriptorNameAndID> m_seen_descriptor_ids;
};

template<typename NestedDeclarationsRule>
GC::Ptr<CSSRule> Parser::convert_to_rule(Rule const& rule, Nested nested)
{
    return rule.visit(
        [this, nested](AtRule const& at_rule) -> GC::Ptr<CSSRule> {
            // https://compat.spec.whatwg.org/#css-at-rules
            // @-webkit-keyframes must be supported as an alias of @keyframes.
            if (at_rule.name.equals_ignoring_ascii_case("keyframes"sv) || at_rule.name.equals_ignoring_ascii_case("-webkit-keyframes"sv))
                return convert_to_keyframes_rule(at_rule);

            if (has_ignored_vendor_prefix(at_rule.name))
                return {};

            if (at_rule.name.equals_ignoring_ascii_case("container"sv))
                return convert_to_container_rule<NestedDeclarationsRule>(at_rule, nested);

            if (at_rule.name.equals_ignoring_ascii_case("counter-style"sv))
                return convert_to_counter_style_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("font-face"sv))
                return convert_to_font_face_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("font-feature-values"sv))
                return convert_to_font_feature_values_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("function"sv))
                return convert_to_function_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("import"sv))
                return convert_to_import_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("layer"sv))
                return convert_to_layer_rule<NestedDeclarationsRule>(at_rule, nested);

            if (is_margin_rule_name(at_rule.name))
                return convert_to_margin_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("media"sv))
                return convert_to_media_rule<NestedDeclarationsRule>(at_rule, nested);

            if (at_rule.name.equals_ignoring_ascii_case("namespace"sv))
                return convert_to_namespace_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("page"sv))
                return convert_to_page_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("property"sv))
                return convert_to_property_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("scope"sv))
                return convert_to_scope_rule<NestedDeclarationsRule>(at_rule, nested);

            if (at_rule.name.equals_ignoring_ascii_case("supports"sv))
                return convert_to_supports_rule<NestedDeclarationsRule>(at_rule, nested);

            // FIXME: More at rules!
            ErrorReporter::the().report(UnknownRuleError { .rule_name = Utf16String::formatted("@{}", at_rule.name) });
            return {};
        },
        [this, nested](QualifiedRule const& qualified_rule) -> GC::Ptr<CSSRule> {
            return convert_to_style_rule(qualified_rule, nested);
        });
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
    auto maybe_selectors = parse_selector_list_in_rust(qualified_rule.prelude_text, m_declared_namespaces,
        nested == Nested::Yes, false);

    if (!maybe_selectors.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "style"_utf16_fly_string,
            .prelude = qualified_rule.prelude_text.to_utf8(),
            .description = "Selectors invalid."_string,
        });
        return {};
    }

    if (maybe_selectors.value().is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "style"_utf16_fly_string,
            .prelude = qualified_rule.prelude_text.to_utf8(),
            .description = "Empty selector."_string,
        });
        return {};
    }

    SelectorList selectors = maybe_selectors.release_value();
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
    auto prelude = parse_import_prelude(rule);
    if (!prelude.has_value())
        return {};
    Optional<CSSImportRule::ImportScope> scope;
    if (prelude->has_scope)
        scope = CSSImportRule::ImportScope { move(prelude->scope_start), move(prelude->scope_end) };
    return CSSImportRule::create(move(prelude->url), const_cast<DOM::Document*>(m_document.ptr()), move(prelude->layer), move(scope), move(prelude->supports), MediaList::create(move(prelude->media_queries)));
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

        // Then the rules
        GC::RootVector<GC::Ref<CSSRule>> child_rules;
        for (auto const& child : rule.child_rules_and_lists_of_declarations) {
            child.visit(
                [&](Rule const& rule) {
                    if (auto child_rule = convert_to_rule<NestedDeclarationsRule>(rule, nested))
                        child_rules.append(*child_rule);
                },
                [&](Vector<Declaration> const& declarations) {
                    child_rules.append(NestedDeclarationsRule::create(*this, declarations));
                });
        }
        auto rule_list = CSSRuleList::create(child_rules);
        return CSSLayerBlockRule::create(layer_name, rule_list);
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

    auto media_list = MediaList::create(RustQueryParser::parse_media_query_list(*this, rule.prelude_text));
    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule<NestedDeclarationsRule>(rule, nested))
                    child_rules.append(*child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(NestedDeclarationsRule::create(*this, declarations));
            });
    }
    return CSSMediaRule::create(media_list, CSSRuleList::create(child_rules));
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

    if (rule.prelude_text.trim_ascii_whitespace().is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Empty prelude."_string,
        });
        return {};
    }

    auto supports = RustQueryParser::parse_supports(*this, rule.prelude_text);
    if (!supports) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Supports clause invalid."_string,
        });
        return {};
    }

    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule<NestedDeclarationsRule>(rule, nested))
                    child_rules.append(*child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(NestedDeclarationsRule::create(*this, declarations));
            });
    }

    auto rule_list = CSSRuleList::create(child_rules);
    return CSSSupportsRule::create(supports.release_nonnull(), rule_list);
}

GC::Ptr<CSSPropertyRule> Parser::convert_to_property_rule(AtRule const& rule)
{
    m_rule_context.append(RuleContext::AtProperty);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtProperty);
    };

    // https://drafts.css-houdini.org/css-properties-values-api-1/#at-ruledef-property
    // @property <custom-property-name> {
    // <declaration-list>
    // }
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Must be a block, not a statement."_string,
        });
        return {};
    }

    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Name || !rule.parsed_prelude.name.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Name must be an ident starting with '--'."_string,
        });
        return {};
    }

    auto name = rule.parsed_prelude.name.value();

    Optional<Utf16FlyString> syntax_maybe;
    Optional<bool> inherits_maybe;
    RefPtr<StyleValue const> initial_value_maybe;

    rule.for_each_as_declaration_list([&](auto& declaration) {
        if (auto descriptor = convert_to_descriptor(AtRuleID::Property, declaration); descriptor.has_value()) {
            if (descriptor->descriptor_name_and_id.id() == DescriptorID::Syntax) {
                if (descriptor->value->is_string())
                    syntax_maybe = descriptor->value->as_string().string_value();
                return;
            }
            if (descriptor->descriptor_name_and_id.id() == DescriptorID::Inherits) {
                switch (descriptor->value->to_keyword()) {
                case Keyword::True:
                    inherits_maybe = true;
                    break;
                case Keyword::False:
                    inherits_maybe = false;
                    break;
                default:
                    break;
                }
                return;
            }
            if (descriptor->descriptor_name_and_id.id() == DescriptorID::InitialValue) {
                initial_value_maybe = *descriptor->value;
                return;
            }
        }
    });

    // @property rules require a syntax and inherits descriptor; if either are missing, the entire rule is invalid and must be ignored.
    if (!syntax_maybe.has_value() || syntax_maybe->is_empty() || !inherits_maybe.has_value()) {
        return {};
    }

    CSS::Parser::ParsingParams parsing_params;
    if (document())
        parsing_params = CSS::Parser::ParsingParams { *document() };
    else
        parsing_params = CSS::Parser::ParsingParams {};

    auto maybe_syntax = parse_as_syntax(syntax_maybe.value(), LimitSingleComponentIdentToCustomIdent::Yes);

    // If the provided string is not a valid syntax string (if it returns failure when consume
    // a syntax definition is called on it), the descriptor is invalid and must be ignored.
    if (!maybe_syntax) {
        return {};
    }
    // The initial-value descriptor is optional only if the syntax is the universal syntax definition,
    // otherwise the descriptor is required; if it’s missing, the entire rule is invalid and must be ignored.
    if (!initial_value_maybe && maybe_syntax->type() != CSS::Parser::SyntaxNode::NodeType::Universal) {
        return {};
    }

    if (initial_value_maybe) {
        initial_value_maybe = Web::CSS::Parser::parse_with_a_syntax(parsing_params, initial_value_maybe->is_unresolved() ? initial_value_maybe->as_unresolved().token_source() : initial_value_maybe->to_utf16_string(SerializationMode::ResolvedValueForReparse),
            *maybe_syntax);

        // Otherwise, if the value of the syntax descriptor is not the universal syntax definition,
        // the following conditions must be met for the @property rule to be valid:
        if (maybe_syntax->type() != CSS::Parser::SyntaxNode::NodeType::Universal) {
            //  - The initial-value descriptor must be present.
            //  - The initial-value descriptor’s value must parse successfully according to the grammar specified by the syntax definition.
            //  - The initial-value must be computationally independent.
            if (!initial_value_maybe || initial_value_maybe->is_guaranteed_invalid() || !initial_value_maybe->is_computationally_independent())
                return {};
        }
    }

    return CSSPropertyRule::create(move(name), syntax_maybe.value(), maybe_syntax.release_nonnull(), inherits_maybe.value(), move(initial_value_maybe));
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
        VERIFY(item.value.has_value());
        bool is_end = item.flags == 1;
        auto selectors = parse_selector_list_in_rust(*item.value, m_declared_namespaces,
            is_end || nested == Nested::Yes, false);
        if (!selectors.has_value() || selectors->is_empty() || selector_list_contains_pseudo_element(*selectors))
            return nullptr;
        if (is_end)
            end = selectors.release_value();
        else
            start = selectors.release_value();
    }
    if (nested == Nested::Yes && start.has_value())
        start = adapt_nested_relative_selector_list(*start, nesting_parent);
    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& child_rule) {
                if (auto converted_rule = convert_to_rule<NestedDeclarationsRule>(child_rule, Nested::Yes))
                    child_rules.append(*converted_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(NestedDeclarationsRule::create(*this, declarations));
            });
    }
    return CSSScopeRule::create(move(start), move(end), CSSRuleList::create(child_rules));
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

    auto rust_conditions = RustQueryParser::parse_container_condition_list(*this, rule.prelude_text);
    if (!rust_conditions.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@container"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Invalid container condition list."_string,
        });
        return nullptr;
    }

    Vector<CSSContainerRule::Condition> conditions;
    conditions.ensure_capacity(rust_conditions->size());
    for (auto& condition : *rust_conditions)
        conditions.unchecked_empend(move(condition.name), move(condition.query));

    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& child_rule) {
                if (auto converted_rule = convert_to_rule<NestedDeclarationsRule>(child_rule, nested))
                    child_rules.append(*converted_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(NestedDeclarationsRule::create(*this, declarations));
            });
    }

    auto rule_list = CSSRuleList::create(child_rules);
    return CSSContainerRule::create(move(conditions), rule_list);
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

    // https://drafts.csswg.org/css-counter-styles-3/#typedef-counter-style-name
    // When used here, to define a counter style, it also cannot be any of the non-overridable counter-style names
    // FIXME: We should allow these in the UA stylesheet in order to initially define them.
    if (CSSCounterStyleRule::matches_non_overridable_counter_style_name(name) && m_is_ua_style_sheet != IsUAStyleSheet::Yes) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_utf16_fly_string,
            .prelude = rule.prelude_text.to_utf8(),
            .description = "Non-overridable counter style name."_string,
        });
        return nullptr;
    }

    RefPtr<StyleValue const> system;
    RefPtr<StyleValue const> negative;
    RefPtr<StyleValue const> prefix;
    RefPtr<StyleValue const> suffix;
    RefPtr<StyleValue const> range;
    RefPtr<StyleValue const> pad;
    RefPtr<StyleValue const> fallback;
    RefPtr<StyleValue const> symbols;
    RefPtr<StyleValue const> additive_symbols;
    RefPtr<StyleValue const> speak_as;

    rule.for_each_as_declaration_list([&](auto& declaration) {
        auto const& descriptor = convert_to_descriptor(AtRuleID::CounterStyle, declaration);
        if (!descriptor.has_value())
            return;

        switch (descriptor->descriptor_name_and_id.id()) {
        case DescriptorID::System:
            system = descriptor->value;
            break;
        case DescriptorID::Negative:
            negative = descriptor->value;
            break;
        case DescriptorID::Prefix:
            prefix = descriptor->value;
            break;
        case DescriptorID::Suffix:
            suffix = descriptor->value;
            break;
        case DescriptorID::Range:
            range = descriptor->value;
            break;
        case DescriptorID::Pad:
            pad = descriptor->value;
            break;
        case DescriptorID::Fallback:
            fallback = descriptor->value;
            break;
        case DescriptorID::Symbols:
            symbols = descriptor->value;
            break;
        case DescriptorID::AdditiveSymbols:
            additive_symbols = descriptor->value;
            break;
        case DescriptorID::SpeakAs:
            speak_as = descriptor->value;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    });

    return CSSCounterStyleRule::create(move(name), move(system), move(negative), move(prefix), move(suffix), move(range), move(pad), move(fallback), move(symbols), move(additive_symbols), move(speak_as));
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

    DescriptorList descriptors { AtRuleID::FontFace };
    rule.for_each_as_declaration_list([&](auto& declaration) {
        if (auto descriptor = convert_to_descriptor(AtRuleID::FontFace, declaration); descriptor.has_value()) {
            descriptors.append(descriptor.release_value());
        }
    });

    return CSSFontFaceRule::create(CSSFontFaceDescriptors::create(descriptors.release_descriptors()));
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

            GC::Ptr<CSSFontFeatureValuesMap> feature_values_map;
            size_t max_value_count = 1;

            if (at_rule.name.equals_ignoring_ascii_case("stylistic"sv)) {
                feature_values_map = font_feature_values_rule->stylistic();
            } else if (at_rule.name.equals_ignoring_ascii_case("historical-forms"sv)) {
                feature_values_map = font_feature_values_rule->historical_forms();
            } else if (at_rule.name.equals_ignoring_ascii_case("styleset"sv)) {
                feature_values_map = font_feature_values_rule->styleset();
                max_value_count = NumericLimits<size_t>::max();
            } else if (at_rule.name.equals_ignoring_ascii_case("character-variant"sv)) {
                feature_values_map = font_feature_values_rule->character_variant();
                max_value_count = 2;
            } else if (at_rule.name.equals_ignoring_ascii_case("swash"sv)) {
                feature_values_map = font_feature_values_rule->swash();
            } else if (at_rule.name.equals_ignoring_ascii_case("ornaments"sv)) {
                feature_values_map = font_feature_values_rule->ornaments();
            } else if (at_rule.name.equals_ignoring_ascii_case("annotation"sv)) {
                feature_values_map = font_feature_values_rule->annotation();
            } else {
                // NB: Other at-rules are disallowed in this context and should have already been dropped
                VERIFY_NOT_REACHED();
            }

            at_rule.for_each_as_declaration_list([&](Declaration const& declaration) {
                auto values = parse_font_feature_values(declaration, max_value_count);
                if (!values.has_value())
                    return;
                MUST(feature_values_map->set(declaration.name.to_utf16_string(), values.release_value()));
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

    auto prelude = parse_function_prelude(function_rule);

    if (!prelude.has_value())
        return nullptr;

    Vector<GC::Ref<CSSRule>> child_rules {};

    // https://drafts.csswg.org/css-mixins-1/#function-body
    for (auto const& child : function_rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule<CSSFunctionDeclarations>(rule, Nested::Yes))
                    child_rules.append(*child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(CSSFunctionDeclarations::create(*this, declarations));
            });
    }

    return CSSFunctionRule::create(CSSRuleList::create(child_rules), move(prelude->name), move(prelude->parameters), move(prelude->return_type));
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
    PageSelectorList page_selectors;
    Optional<Utf16FlyString> page_name;
    Vector<PagePseudoClass> pseudo_classes;
    for (auto const& item : page_rule.parsed_prelude.items) {
        if (item.flags != 0x80) {
            VERIFY(item.flags <= to_underlying(PagePseudoClass::Blank));
            pseudo_classes.append(static_cast<PagePseudoClass>(item.flags));
            continue;
        }
        if (page_name.has_value() || !pseudo_classes.is_empty())
            page_selectors.empend(move(page_name), move(pseudo_classes));
        page_name = item.value;
    }
    if (page_name.has_value() || !pseudo_classes.is_empty())
        page_selectors.empend(move(page_name), move(pseudo_classes));

    GC::RootVector<GC::Ref<CSSRule>> child_rules;
    DescriptorList descriptors { AtRuleID::Page };
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
        [&](auto& declaration) {
            if (auto descriptor = convert_to_descriptor(AtRuleID::Page, declaration); descriptor.has_value()) {
                descriptors.append(descriptor.release_value());
            }
        });

    auto rule_list = CSSRuleList::create(child_rules);
    return CSSPageRule::create(move(page_selectors), CSSPageDescriptors::create(descriptors.release_descriptors()), rule_list);
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
GC::Ref<Descriptors> Parser::convert_to_descriptors(AtRuleID at_rule_id, Vector<Declaration> const& declarations)
{
    DescriptorList descriptor_list { at_rule_id };

    for (auto const& declaration : declarations) {
        if (auto descriptor = convert_to_descriptor(at_rule_id, declaration); descriptor.has_value())
            descriptor_list.append(descriptor.release_value());
    }

    return Descriptors::create(descriptor_list.release_descriptors());
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

Optional<Parser::ImportPrelude> Parser::parse_import_prelude(AtRule const& rule)
{
    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Import)
        return {};
    if (rule.parsed_prelude.items.is_empty())
        return {};
    auto const& url_item = rule.parsed_prelude.items[0];
    if (!url_item.value.has_value())
        return {};
    Optional<URL> url;
    if (url_item.flags == 7) {
        url = URL { MUST(url_item.value->view().to_utf8()) };
    } else {
        auto value = parse_primitive_value_from_source(ValueType::Url, *url_item.value);
        if (value && value->is_url())
            url = value->as_url().url();
    }
    if (!url.has_value())
        return {};
    Optional<Utf16FlyString> layer;
    bool has_scope = false;
    Optional<SelectorList> scope_start;
    Optional<SelectorList> scope_end;
    RefPtr<Supports> supports;
    Vector<NonnullRefPtr<MediaQuery>> media_queries;
    auto parse_scope_selector_list = [&](Utf16View source, SelectorType selector_type) -> Optional<SelectorList> {
        auto selectors = parse_selector_list_in_rust(source, m_declared_namespaces, selector_type == SelectorType::Relative, false);
        if (!selectors.has_value() || selectors->is_empty())
            return {};
        if (selector_list_contains_pseudo_element(*selectors))
            return {};
        return selectors;
    };
    for (auto const& item : rule.parsed_prelude.items.span().slice(1)) {
        switch (item.flags) {
        case 1:
            if (!item.value.has_value())
                return {};
            layer = *item.value;
            break;
        case 2:
            has_scope = true;
            break;
        case 3:
            if (!item.value.has_value())
                return {};
            scope_start = parse_scope_selector_list(*item.value, SelectorType::Standalone);
            if (!scope_start.has_value())
                return {};
            break;
        case 4:
            if (!item.value.has_value())
                return {};
            scope_end = parse_scope_selector_list(*item.value, SelectorType::Relative);
            if (!scope_end.has_value())
                return {};
            break;
        case 5: {
            if (!item.value.has_value())
                return {};
            supports = RustQueryParser::parse_supports(*this, *item.value);
            if (!supports) {
                auto declaration = RustQueryParser::parse_supports_declaration(*this, *item.value);
                if (declaration)
                    supports = Supports::create(declaration.release_nonnull<BooleanExpression>());
            }
            if (!supports)
                return {};
            break;
        }
        case 6:
            if (!item.value.has_value())
                return {};
            media_queries = RustQueryParser::parse_media_query_list(*this, *item.value);
            break;
        default:
            return {};
        }
    }
    return ImportPrelude { url.release_value(), move(layer), has_scope, move(scope_start), move(scope_end), move(supports), move(media_queries) };
}

Optional<Vector<u32>> Parser::parse_font_feature_values(Declaration const& declaration, size_t max_value_count)
{
    if (declaration.important == Important::Yes)
        return {};
    auto source = declaration.value_text.utf16_view();
    ValueParserFFI::FfiUtf16View ffi_source {
        .ascii = source.has_ascii_storage() ? reinterpret_cast<u8 const*>(source.ascii_span().data()) : nullptr,
        .utf16 = source.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(source.utf16_span().data()),
        .length = source.length_in_code_units(),
    };
    auto count = ValueParserFFI::rust_parse_font_feature_values(ffi_source, max_value_count, nullptr, 0);
    if (count == NumericLimits<size_t>::max())
        return {};
    Vector<u32> values;
    values.resize(count);
    if (ValueParserFFI::rust_parse_font_feature_values(ffi_source, max_value_count, values.data(), values.size()) != count)
        return {};
    return values;
}

Optional<Parser::FunctionPrelude> Parser::parse_function_prelude(AtRule const& rule)
{
    if (rule.parsed_prelude.kind != ParsedRulePreludeKind::Function || !rule.parsed_prelude.name.has_value())
        return {};
    Vector<FunctionParameterInternal> parameters;
    size_t position = 0;
    while (position < rule.parsed_prelude.items.size()) {
        auto const& name_item = rule.parsed_prelude.items[position++];
        if (name_item.flags != 0 || !name_item.value.has_value())
            return {};
        NonnullRefPtr<SyntaxNode> type = UniversalSyntaxNode::create();
        if (position < rule.parsed_prelude.items.size() && rule.parsed_prelude.items[position].flags == 1) {
            auto const& type_item = rule.parsed_prelude.items[position++];
            if (!type_item.value.has_value())
                return {};
            auto parsed_type = parse_as_syntax(*type_item.value);
            if (!parsed_type)
                return {};
            type = parsed_type.release_nonnull();
        }
        RefPtr<StyleValue const> default_value;
        if (position < rule.parsed_prelude.items.size() && rule.parsed_prelude.items[position].flags == 2) {
            auto const& default_item = rule.parsed_prelude.items[position++];
            if (!default_item.value.has_value())
                return {};
            auto parsed_default = parse_css_value_from_source(PropertyID::Custom, *default_item.value);
            if (parsed_default.is_error())
                return {};
            auto unparsed_default = parsed_default.release_value();
            if (unparsed_default->is_css_wide_keyword() || unparsed_default->as_unresolved().contains_arbitrary_substitution_function()) {
                default_value = move(unparsed_default);
            } else {
                auto parsed = parse_with_a_syntax(unparsed_default->as_unresolved().token_source(), *type);
                if (parsed->is_guaranteed_invalid())
                    return {};
                default_value = move(parsed);
            }
        }
        parameters.append({ *name_item.value, move(type), move(default_value) });
    }
    NonnullRefPtr<SyntaxNode> return_type = UniversalSyntaxNode::create();
    if (rule.parsed_prelude.secondary.has_value()) {
        auto parsed = parse_as_syntax(*rule.parsed_prelude.secondary);
        if (!parsed)
            return {};
        return_type = parsed.release_nonnull();
    }
    return FunctionPrelude { *rule.parsed_prelude.name, move(parameters), move(return_type) };
}

}
