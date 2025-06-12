/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tommy van der Vorst <tommy@pixelspark.nl>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSFontFaceRule.h>
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
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS::Parser {

// A helper that ensures only the last instance of each descriptor is included, while also handling shorthands.
class DescriptorList {
public:
    DescriptorList(AtRuleID at_rule)
        : m_at_rule(at_rule)
    {
    }

    void append(Descriptor&& descriptor)
    {
        if (is_shorthand(m_at_rule, descriptor.descriptor_id)) {
            for_each_expanded_longhand(m_at_rule, descriptor.descriptor_id, descriptor.value, [this](auto longhand_id, auto longhand_value) {
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
        if (m_seen_descriptor_ids.contains(descriptor.descriptor_id)) {
            m_descriptors.remove_first_matching([&descriptor](Descriptor const& existing) {
                return existing.descriptor_id == descriptor.descriptor_id;
            });
        } else {
            m_seen_descriptor_ids.set(descriptor.descriptor_id);
        }
        m_descriptors.append(move(descriptor));
    }

    AtRuleID m_at_rule;
    Vector<Descriptor> m_descriptors;
    HashTable<DescriptorID> m_seen_descriptor_ids;
};

GC::Ptr<CSSRule> Parser::convert_to_rule(Rule const& rule, Nested nested)
{
    return rule.visit(
        [this, nested](AtRule const& at_rule) -> GC::Ptr<CSSRule> {
            if (has_ignored_vendor_prefix(at_rule.name))
                return {};

            if (at_rule.name.equals_ignoring_ascii_case("font-face"sv))
                return convert_to_font_face_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("import"sv))
                return convert_to_import_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("keyframes"sv))
                return convert_to_keyframes_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("layer"sv))
                return convert_to_layer_rule(at_rule, nested);

            if (is_margin_rule_name(at_rule.name))
                return convert_to_margin_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("media"sv))
                return convert_to_media_rule(at_rule, nested);

            if (at_rule.name.equals_ignoring_ascii_case("namespace"sv))
                return convert_to_namespace_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("page"sv))
                return convert_to_page_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("property"sv))
                return convert_to_property_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("supports"sv))
                return convert_to_supports_rule(at_rule, nested);

            // FIXME: More at rules!
            dbgln_if(CSS_PARSER_DEBUG, "Unrecognized CSS at-rule: @{}", at_rule.name);
            return {};
        },
        [this, nested](QualifiedRule const& qualified_rule) -> GC::Ptr<CSSRule> {
            return convert_to_style_rule(qualified_rule, nested);
        });
}

GC::Ptr<CSSStyleRule> Parser::convert_to_style_rule(QualifiedRule const& qualified_rule, Nested nested)
{
    TokenStream prelude_stream { qualified_rule.prelude };

    auto maybe_selectors = parse_a_selector_list(prelude_stream,
        nested == Nested::Yes ? SelectorType::Relative : SelectorType::Standalone);

    if (maybe_selectors.is_error()) {
        if (maybe_selectors.error() == ParseError::SyntaxError) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: style rule selectors invalid; discarding.");
            if constexpr (CSS_PARSER_DEBUG) {
                prelude_stream.dump_all_tokens();
            }
        }
        return {};
    }

    if (maybe_selectors.value().is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: empty selector; discarding.");
        return {};
    }

    SelectorList selectors = maybe_selectors.release_value();
    if (nested == Nested::Yes)
        selectors = adapt_nested_relative_selector_list(selectors);

    auto declaration = convert_to_style_declaration(qualified_rule.declarations);

    GC::RootVector<GC::Ref<CSSRule>> child_rules { realm().heap() };
    for (auto& child : qualified_rule.child_rules) {
        child.visit(
            [&](Rule const& rule) {
                // "In addition to nested style rules, this specification allows nested group rules inside of style rules:
                // any at-rule whose body contains style rules can be nested inside of a style rule as well."
                // https://drafts.csswg.org/css-nesting-1/#nested-group-rules
                if (auto converted_rule = convert_to_rule(rule, Nested::Yes)) {
                    if (is<CSSGroupingRule>(*converted_rule)) {
                        child_rules.append(*converted_rule);
                    } else {
                        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: nested {} is not allowed inside style rule; discarding.", converted_rule->class_name());
                    }
                }
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(CSSNestedDeclarations::create(realm(), *convert_to_style_declaration(declarations)));
            });
    }
    auto nested_rules = CSSRuleList::create(realm(), child_rules);
    return CSSStyleRule::create(realm(), move(selectors), *declaration, *nested_rules);
}

GC::Ptr<CSSImportRule> Parser::convert_to_import_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-cascade-5/#at-import
    // @import [ <url> | <string> ]
    //         [ layer | layer(<layer-name>) ]?
    //         <import-conditions> ;
    //
    // <import-conditions> = [ supports( [ <supports-condition> | <declaration> ] ) ]?
    //                      <media-query-list>?

    if (rule.prelude.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @import rule: Empty prelude.");
        return {};
    }

    if (!rule.child_rules_and_lists_of_declarations.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @import rule: Block is not allowed.");
        return {};
    }

    TokenStream tokens { rule.prelude };
    tokens.discard_whitespace();

    Optional<URL> url = parse_url_function(tokens);
    if (!url.has_value() && tokens.next_token().is(Token::Type::String))
        url = URL { tokens.consume_a_token().token().string().to_string() };

    if (!url.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @import rule: Unable to parse `{}` as URL.", tokens.next_token().to_debug_string());
        return {};
    }

    tokens.discard_whitespace();
    // FIXME: Implement layer support.
    RefPtr<Supports> supports {};
    if (tokens.next_token().is_function("supports"sv)) {
        auto component_value = tokens.consume_a_token();
        TokenStream supports_tokens { component_value.function().value };
        if (supports_tokens.next_token().is_block()) {
            supports = parse_a_supports(supports_tokens);
        } else {
            m_rule_context.append(RuleContext::SupportsCondition);
            auto declaration = consume_a_declaration(supports_tokens);
            m_rule_context.take_last();
            if (declaration.has_value()) {
                auto supports_declaration = Supports::Declaration::create(declaration->to_string(), convert_to_style_property(*declaration).has_value());
                supports = Supports::create(supports_declaration.release_nonnull<BooleanExpression>());
            }
        }
    }

    auto media_query_list = parse_a_media_query_list(tokens);

    if (tokens.has_next_token()) {
        if constexpr (CSS_PARSER_DEBUG) {
            dbgln("Failed to parse @import rule:");
            tokens.dump_all_tokens();
        }
        return {};
    }

    return CSSImportRule::create(realm(), url.release_value(), const_cast<DOM::Document*>(m_document.ptr()), supports, move(media_query_list));
}

Optional<FlyString> Parser::parse_layer_name(TokenStream<ComponentValue>& tokens, AllowBlankLayerName allow_blank_layer_name)
{
    // https://drafts.csswg.org/css-cascade-5/#typedef-layer-name
    // <layer-name> = <ident> [ '.' <ident> ]*

    // "The CSS-wide keywords are reserved for future use, and cause the rule to be invalid at parse time if used as an <ident> in the <layer-name>."
    auto is_valid_layer_name_part = [](auto& token) {
        return token.is(Token::Type::Ident) && !is_css_wide_keyword(token.token().ident());
    };

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    if (!tokens.has_next_token() && allow_blank_layer_name == AllowBlankLayerName::Yes) {
        // No name present, just return a blank one
        return FlyString();
    }

    auto& first_name_token = tokens.consume_a_token();
    if (!is_valid_layer_name_part(first_name_token))
        return {};

    StringBuilder builder;
    builder.append(first_name_token.token().ident());

    while (tokens.has_next_token()) {
        // Repeatedly parse `'.' <ident>`
        if (!tokens.next_token().is_delim('.'))
            break;
        tokens.discard_a_token(); // '.'

        auto& name_token = tokens.consume_a_token();
        if (!is_valid_layer_name_part(name_token))
            return {};
        builder.appendff(".{}", name_token.token().ident());
    }

    transaction.commit();
    return builder.to_fly_string_without_validation();
}

GC::Ptr<CSSRule> Parser::convert_to_layer_rule(AtRule const& rule, Nested nested)
{
    // https://drafts.csswg.org/css-cascade-5/#at-layer
    if (!rule.child_rules_and_lists_of_declarations.is_empty()) {
        // CSSLayerBlockRule
        // @layer <layer-name>? {
        //   <rule-list>
        // }

        // First, the name
        FlyString layer_name = {};
        auto prelude_tokens = TokenStream { rule.prelude };
        if (auto maybe_name = parse_layer_name(prelude_tokens, AllowBlankLayerName::Yes); maybe_name.has_value()) {
            layer_name = maybe_name.release_value();
        } else {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @layer has invalid prelude, (not a valid layer name) prelude = {}; discarding.", rule.prelude);
            return {};
        }

        prelude_tokens.discard_whitespace();
        if (prelude_tokens.has_next_token()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @layer has invalid prelude, (tokens after layer name) prelude = {}; discarding.", rule.prelude);
            return {};
        }

        // Then the rules
        GC::RootVector<GC::Ref<CSSRule>> child_rules { realm().heap() };
        for (auto const& child : rule.child_rules_and_lists_of_declarations) {
            child.visit(
                [&](Rule const& rule) {
                    if (auto child_rule = convert_to_rule(rule, nested))
                        child_rules.append(*child_rule);
                },
                [&](Vector<Declaration> const& declarations) {
                    child_rules.append(CSSNestedDeclarations::create(realm(), *convert_to_style_declaration(declarations)));
                });
        }
        auto rule_list = CSSRuleList::create(realm(), child_rules);
        return CSSLayerBlockRule::create(realm(), layer_name, rule_list);
    }

    // CSSLayerStatementRule
    // @layer <layer-name>#;
    auto tokens = TokenStream { rule.prelude };
    tokens.discard_whitespace();
    Vector<FlyString> layer_names;
    while (tokens.has_next_token()) {
        // Comma
        if (!layer_names.is_empty()) {
            if (auto comma = tokens.consume_a_token(); !comma.is(Token::Type::Comma)) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @layer missing separating comma, ({}) prelude = {}; discarding.", comma.to_debug_string(), rule.prelude);
                return {};
            }
            tokens.discard_whitespace();
        }

        if (auto name = parse_layer_name(tokens, AllowBlankLayerName::No); name.has_value()) {
            layer_names.append(name.release_value());
        } else {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @layer contains invalid name, prelude = {}; discarding.", rule.prelude);
            return {};
        }
        tokens.discard_whitespace();
    }

    if (layer_names.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @layer statement has no layer names, prelude = {}; discarding.", rule.prelude);
        return {};
    }

    return CSSLayerStatementRule::create(realm(), move(layer_names));
}

GC::Ptr<CSSKeyframesRule> Parser::convert_to_keyframes_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-animations/#keyframes
    // @keyframes = @keyframes <keyframes-name> { <qualified-rule-list> }
    // <keyframes-name> = <custom-ident> | <string>
    // <keyframe-block> = <keyframe-selector># { <declaration-list> }
    // <keyframe-selector> = from | to | <percentage [0,100]>

    if (rule.prelude.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @keyframes rule: Empty prelude.");
        return {};
    }

    // FIXME: Is there some way of detecting if there is a block or not?

    auto prelude_stream = TokenStream { rule.prelude };
    prelude_stream.discard_whitespace();
    auto& token = prelude_stream.consume_a_token();
    if (!token.is_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes has invalid prelude, prelude = {}; discarding.", rule.prelude);
        return {};
    }

    auto name_token = token.token();
    prelude_stream.discard_whitespace();

    if (prelude_stream.has_next_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes has invalid prelude, prelude = {}; discarding.", rule.prelude);
        return {};
    }

    if (name_token.is(Token::Type::Ident) && (is_css_wide_keyword(name_token.ident()) || name_token.ident().equals_ignoring_ascii_case("none"sv))) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes rule name is invalid: {}; discarding.", name_token.ident());
        return {};
    }

    if (!name_token.is(Token::Type::String) && !name_token.is(Token::Type::Ident)) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes rule name is invalid: {}; discarding.", name_token.to_debug_string());
        return {};
    }

    auto name = name_token.to_string();

    GC::RootVector<GC::Ref<CSSRule>> keyframes(realm().heap());
    rule.for_each_as_qualified_rule_list([&](auto& qualified_rule) {
        if (!qualified_rule.child_rules.is_empty()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes keyframe rule contains at-rules; discarding them.");
        }

        auto selectors = Vector<CSS::Percentage> {};
        TokenStream child_tokens { qualified_rule.prelude };
        while (child_tokens.has_next_token()) {
            child_tokens.discard_whitespace();
            if (!child_tokens.has_next_token())
                break;
            auto tok = child_tokens.consume_a_token();
            if (!tok.is_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes rule has invalid selector: {}; discarding.", tok.to_debug_string());
                child_tokens.reconsume_current_input_token();
                break;
            }
            auto token = tok.token();
            auto read_a_selector = false;
            if (token.is(Token::Type::Ident)) {
                if (token.ident().equals_ignoring_ascii_case("from"sv)) {
                    selectors.append(CSS::Percentage(0));
                    read_a_selector = true;
                }
                if (token.ident().equals_ignoring_ascii_case("to"sv)) {
                    selectors.append(CSS::Percentage(100));
                    read_a_selector = true;
                }
            } else if (token.is(Token::Type::Percentage)) {
                selectors.append(CSS::Percentage(token.percentage()));
                read_a_selector = true;
            }

            if (read_a_selector) {
                child_tokens.discard_whitespace();
                if (child_tokens.consume_a_token().is(Token::Type::Comma))
                    continue;
            }

            child_tokens.reconsume_current_input_token();
            break;
        }

        PropertiesAndCustomProperties properties;
        qualified_rule.for_each_as_declaration_list([&](auto const& declaration) {
            extract_property(declaration, properties);
        });
        auto style = CSSStyleProperties::create(realm(), move(properties.properties), move(properties.custom_properties));
        for (auto& selector : selectors) {
            auto keyframe_rule = CSSKeyframeRule::create(realm(), selector, *style);
            keyframes.append(keyframe_rule);
        }
    });

    return CSSKeyframesRule::create(realm(), name, CSSRuleList::create(realm(), keyframes));
}

GC::Ptr<CSSNamespaceRule> Parser::convert_to_namespace_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-namespaces/#syntax
    // @namespace <namespace-prefix>? [ <string> | <url> ] ;
    // <namespace-prefix> = <ident>

    if (rule.prelude.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @namespace rule: Empty prelude.");
        return {};
    }

    if (!rule.child_rules_and_lists_of_declarations.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @namespace rule: Block is not allowed.");
        return {};
    }

    auto tokens = TokenStream { rule.prelude };
    tokens.discard_whitespace();

    Optional<FlyString> prefix = {};
    if (tokens.next_token().is(Token::Type::Ident)) {
        prefix = tokens.consume_a_token().token().ident();
        tokens.discard_whitespace();
    }

    FlyString namespace_uri;
    if (auto url = parse_url_function(tokens); url.has_value()) {
        // "A URI string parsed from the URI syntax must be treated as a literal string: as with the STRING syntax, no
        // URI-specific normalization is applied."
        // https://drafts.csswg.org/css-namespaces/#syntax
        namespace_uri = url->url();
    } else if (auto& url_token = tokens.consume_a_token(); url_token.is(Token::Type::String)) {
        namespace_uri = url_token.token().string();
    } else {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @namespace rule: Unable to parse `{}` as URL.", tokens.next_token().to_debug_string());
        return {};
    }

    tokens.discard_whitespace();
    if (tokens.has_next_token()) {
        if constexpr (CSS_PARSER_DEBUG) {
            dbgln("Failed to parse @namespace rule: Trailing tokens after URL.");
            tokens.dump_all_tokens();
        }
        return {};
    }

    return CSSNamespaceRule::create(realm(), prefix, namespace_uri);
}

GC::Ptr<CSSSupportsRule> Parser::convert_to_supports_rule(AtRule const& rule, Nested nested)
{
    // https://drafts.csswg.org/css-conditional-3/#at-supports
    // @supports <supports-condition> {
    //   <rule-list>
    // }

    if (rule.prelude.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @supports rule: Empty prelude.");
        return {};
    }

    auto supports_tokens = TokenStream { rule.prelude };
    auto supports = parse_a_supports(supports_tokens);
    if (!supports) {
        if constexpr (CSS_PARSER_DEBUG) {
            dbgln("Failed to parse @supports rule: supports clause invalid.");
            supports_tokens.dump_all_tokens();
        }
        return {};
    }

    GC::RootVector<GC::Ref<CSSRule>> child_rules { realm().heap() };
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule(rule, nested))
                    child_rules.append(*child_rule);
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(CSSNestedDeclarations::create(realm(), *convert_to_style_declaration(declarations)));
            });
    }

    auto rule_list = CSSRuleList::create(realm(), child_rules);
    return CSSSupportsRule::create(realm(), supports.release_nonnull(), rule_list);
}

GC::Ptr<CSSPropertyRule> Parser::convert_to_property_rule(AtRule const& rule)
{
    // https://drafts.css-houdini.org/css-properties-values-api-1/#at-ruledef-property
    // @property <custom-property-name> {
    // <declaration-list>
    // }

    if (rule.prelude.is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "Failed to parse @property rule: Empty prelude.");
        return {};
    }

    auto prelude_stream = TokenStream { rule.prelude };
    prelude_stream.discard_whitespace();
    auto const& token = prelude_stream.consume_a_token();
    if (!token.is_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @property has invalid prelude, prelude = {}; discarding.", rule.prelude);
        return {};
    }

    auto name_token = token.token();
    prelude_stream.discard_whitespace();

    if (prelude_stream.has_next_token()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @property has invalid prelude, prelude = {}; discarding.", rule.prelude);
        return {};
    }

    if (!name_token.is(Token::Type::Ident)) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @property name is invalid: {}; discarding.", name_token.to_debug_string());
        return {};
    }

    if (!is_a_custom_property_name_string(name_token.ident())) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @property name doesn't start with '--': {}; discarding.", name_token.ident());
        return {};
    }

    auto const& name = name_token.ident();

    Optional<FlyString> syntax_maybe;
    Optional<bool> inherits_maybe;
    RefPtr<CSSStyleValue const> initial_value_maybe;

    rule.for_each_as_declaration_list([&](auto& declaration) {
        if (auto descriptor = convert_to_descriptor(AtRuleID::Property, declaration); descriptor.has_value()) {
            if (descriptor->descriptor_id == DescriptorID::Syntax) {
                if (descriptor->value->is_string())
                    syntax_maybe = descriptor->value->as_string().string_value();
                return;
            }
            if (descriptor->descriptor_id == DescriptorID::Inherits) {
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
            if (descriptor->descriptor_id == DescriptorID::InitialValue) {
                initial_value_maybe = *descriptor->value;
                return;
            }
        }
    });

    // TODO: Parse the initial value using the syntax, if it's provided.

    if (syntax_maybe.has_value() && inherits_maybe.has_value()) {
        return CSSPropertyRule::create(realm(), name, syntax_maybe.value(), inherits_maybe.value(), move(initial_value_maybe));
    }
    return {};
}

GC::Ptr<CSSFontFaceRule> Parser::convert_to_font_face_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-fonts/#font-face-rule
    DescriptorList descriptors { AtRuleID::FontFace };
    rule.for_each_as_declaration_list([&](auto& declaration) {
        if (auto descriptor = convert_to_descriptor(AtRuleID::FontFace, declaration); descriptor.has_value()) {
            descriptors.append(descriptor.release_value());
        }
    });

    return CSSFontFaceRule::create(realm(), CSSFontFaceDescriptors::create(realm(), descriptors.release_descriptors()));
}

GC::Ptr<CSSPageRule> Parser::convert_to_page_rule(AtRule const& page_rule)
{
    // https://drafts.csswg.org/css-page-3/#syntax-page-selector
    // @page = @page <page-selector-list>? { <declaration-rule-list> }
    TokenStream tokens { page_rule.prelude };
    auto page_selectors = parse_a_page_selector_list(tokens);
    if (page_selectors.is_error())
        return nullptr;

    GC::RootVector<GC::Ref<CSSRule>> child_rules { realm().heap() };
    DescriptorList descriptors { AtRuleID::Page };
    page_rule.for_each_as_declaration_rule_list(
        [&](auto& at_rule) {
            if (auto converted_rule = convert_to_rule(at_rule, Nested::No)) {
                if (is<CSSMarginRule>(*converted_rule)) {
                    child_rules.append(*converted_rule);
                } else {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: nested {} is not allowed inside @page rule; discarding.", converted_rule->class_name());
                }
            }
        },
        [&](auto& declaration) {
            if (auto descriptor = convert_to_descriptor(AtRuleID::Page, declaration); descriptor.has_value()) {
                descriptors.append(descriptor.release_value());
            }
        });

    auto rule_list = CSSRuleList::create(realm(), child_rules);
    return CSSPageRule::create(realm(), page_selectors.release_value(), CSSPageDescriptors::create(realm(), descriptors.release_descriptors()), rule_list);
}

GC::Ptr<CSSMarginRule> Parser::convert_to_margin_rule(AtRule const& rule)
{
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
    auto style = CSSStyleProperties::create(realm(), move(properties.properties), move(properties.custom_properties));
    return CSSMarginRule::create(realm(), rule.name, style);
}

}
