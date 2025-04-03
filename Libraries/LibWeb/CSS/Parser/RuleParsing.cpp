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
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
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

            if (at_rule.name.equals_ignoring_ascii_case("media"sv))
                return convert_to_media_rule(at_rule, nested);

            if (at_rule.name.equals_ignoring_ascii_case("namespace"sv))
                return convert_to_namespace_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("supports"sv))
                return convert_to_supports_rule(at_rule, nested);

            if (at_rule.name.equals_ignoring_ascii_case("property"sv))
                return convert_to_property_rule(at_rule);

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

    GC::RootVector<CSSRule*> child_rules { realm().heap() };
    for (auto& child : qualified_rule.child_rules) {
        child.visit(
            [&](Rule const& rule) {
                // "In addition to nested style rules, this specification allows nested group rules inside of style rules:
                // any at-rule whose body contains style rules can be nested inside of a style rule as well."
                // https://drafts.csswg.org/css-nesting-1/#nested-group-rules
                if (auto converted_rule = convert_to_rule(rule, Nested::Yes)) {
                    if (is<CSSGroupingRule>(*converted_rule)) {
                        child_rules.append(converted_rule);
                    } else {
                        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: nested {} is not allowed inside style rule; discarding.", converted_rule->class_name());
                    }
                }
            },
            [&](Vector<Declaration> const& declarations) {
                child_rules.append(CSSNestedDeclarations::create(realm(), *convert_to_style_declaration(declarations)));
            });
    }
    auto nested_rules = CSSRuleList::create(realm(), move(child_rules));
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

    Optional<URL::URL> url = parse_url_function(tokens);
    if (!url.has_value() && tokens.next_token().is(Token::Type::String))
        url = complete_url(tokens.consume_a_token().token().string());

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
            m_rule_context.append(ContextType::SupportsCondition);
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

    return CSSImportRule::create(url.value(), const_cast<DOM::Document&>(*document()), supports, move(media_query_list));
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
        GC::RootVector<CSSRule*> child_rules { realm().heap() };
        for (auto const& child : rule.child_rules_and_lists_of_declarations) {
            child.visit(
                [&](Rule const& rule) {
                    if (auto child_rule = convert_to_rule(rule, nested))
                        child_rules.append(child_rule);
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

    GC::RootVector<CSSRule*> keyframes(realm().heap());
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

    return CSSKeyframesRule::create(realm(), name, CSSRuleList::create(realm(), move(keyframes)));
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
        namespace_uri = url.value().to_string();
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

    GC::RootVector<CSSRule*> child_rules { realm().heap() };
    for (auto const& child : rule.child_rules_and_lists_of_declarations) {
        child.visit(
            [&](Rule const& rule) {
                if (auto child_rule = convert_to_rule(rule, nested))
                    child_rules.append(child_rule);
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
    Optional<String> initial_value_maybe;

    rule.for_each_as_declaration_list([&](auto& declaration) {
        if (declaration.name.equals_ignoring_ascii_case("syntax"sv)) {
            TokenStream token_stream { declaration.value };
            token_stream.discard_whitespace();

            auto const& syntax_token = token_stream.consume_a_token();
            if (syntax_token.is(Token::Type::String)) {
                token_stream.discard_whitespace();
                if (token_stream.has_next_token()) {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Unexpected trailing tokens in syntax");
                } else {
                    syntax_maybe = syntax_token.token().string();
                }
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Unexpected value for @property \"syntax\": {}; discarding.", declaration.to_string());
            }
            return;
        }
        if (declaration.name.equals_ignoring_ascii_case("inherits"sv)) {
            TokenStream token_stream { declaration.value };
            token_stream.discard_whitespace();

            auto const& inherits_token = token_stream.consume_a_token();
            if (inherits_token.is_ident("true"sv) || inherits_token.is_ident("false"sv)) {
                auto const& ident = inherits_token.token().ident();
                token_stream.discard_whitespace();
                if (token_stream.has_next_token()) {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Unexpected trailing tokens in inherits");
                } else {
                    inherits_maybe = (ident == "true");
                }
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Expected true/false for @property \"inherits\" value, got: {}; discarding.", inherits_token.to_debug_string());
            }
            return;
        }
        if (declaration.name.equals_ignoring_ascii_case("initial-value"sv)) {
            // FIXME: Ensure that the initial value matches the syntax, and parse the correct CSSValue out
            StringBuilder initial_value_sb;
            for (auto const& component : declaration.value) {
                initial_value_sb.append(component.to_string());
            }
            initial_value_maybe = MUST(initial_value_sb.to_string());
            return;
        }
    });

    if (syntax_maybe.has_value() && inherits_maybe.has_value()) {
        return CSSPropertyRule::create(realm(), name, syntax_maybe.value(), inherits_maybe.value(), std::move(initial_value_maybe));
    }
    return {};
}

template<typename T>
Vector<ParsedFontFace::Source> Parser::parse_font_face_src(TokenStream<T>& component_values)
{
    Vector<ParsedFontFace::Source> supported_sources;

    auto list_of_source_token_lists = parse_a_comma_separated_list_of_component_values(component_values);
    for (auto const& source_token_list : list_of_source_token_lists) {
        TokenStream source_tokens { source_token_list };
        source_tokens.discard_whitespace();

        // <url> [ format(<font-format>)]?
        // FIXME: Implement optional tech() function from CSS-Fonts-4.
        if (auto maybe_url = parse_url_function(source_tokens); maybe_url.has_value()) {
            auto url = maybe_url.release_value();
            if (!url.is_valid()) {
                continue;
            }

            Optional<FlyString> format;

            source_tokens.discard_whitespace();
            if (!source_tokens.has_next_token()) {
                supported_sources.empend(move(url), format);
                continue;
            }

            auto const& maybe_function = source_tokens.consume_a_token();
            if (!maybe_function.is_function()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (token after `url()` that isn't a function: {}); discarding.", maybe_function.to_debug_string());
                return {};
            }

            auto const& function = maybe_function.function();
            if (function.name.equals_ignoring_ascii_case("format"sv)) {
                auto context_guard = push_temporary_value_parsing_context(FunctionContext { function.name });

                TokenStream format_tokens { function.value };
                format_tokens.discard_whitespace();
                auto const& format_name_token = format_tokens.consume_a_token();
                FlyString format_name;
                if (format_name_token.is(Token::Type::Ident)) {
                    format_name = format_name_token.token().ident();
                } else if (format_name_token.is(Token::Type::String)) {
                    format_name = format_name_token.token().string();
                } else {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (`format()` parameter not an ident or string; is: {}); discarding.", format_name_token.to_debug_string());
                    return {};
                }

                if (!font_format_is_supported(format_name)) {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src format({}) not supported; skipping.", format_name);
                    continue;
                }

                format = move(format_name);
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (unrecognized function token `{}`); discarding.", function.name);
                return {};
            }

            source_tokens.discard_whitespace();
            if (source_tokens.has_next_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (extra token `{}`); discarding.", source_tokens.next_token().to_debug_string());
                return {};
            }

            supported_sources.empend(move(url), format);
            continue;
        }

        auto const& first = source_tokens.consume_a_token();
        if (first.is_function("local"sv)) {
            // local(<family-name>)
            if (first.function().value.is_empty()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (`local()` syntax is invalid: no arguments); discarding.");
                return {};
            }

            TokenStream function_tokens { first.function().value };
            function_tokens.discard_whitespace();
            auto font_family = parse_family_name_value(function_tokens);
            function_tokens.discard_whitespace();
            if (!font_family || function_tokens.has_next_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (`local()` syntax is invalid: `{}`); discarding.", first.function().original_source_text());
                return {};
            }

            if (font_family->is_string())
                supported_sources.empend(font_family->as_string().string_value(), Optional<FlyString> {});
            else if (font_family->is_custom_ident())
                supported_sources.empend(font_family->as_custom_ident().custom_ident(), Optional<FlyString> {});
            else
                VERIFY_NOT_REACHED();

            continue;
        }

        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (failed to parse url from: {}); discarding.", first.to_debug_string());
        return {};
    }

    return supported_sources;
}
template Vector<ParsedFontFace::Source> Parser::parse_font_face_src(TokenStream<Token>& component_values);
template Vector<ParsedFontFace::Source> Parser::parse_font_face_src(TokenStream<ComponentValue>& component_values);

GC::Ptr<CSSFontFaceRule> Parser::convert_to_font_face_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-fonts/#font-face-rule
    Vector<Descriptor> descriptors;
    HashTable<DescriptorID> seen_descriptor_ids;
    rule.for_each_as_declaration_list([&](auto& declaration) {
        if (auto descriptor = convert_to_descriptor(AtRuleID::FontFace, declaration); descriptor.has_value()) {
            if (seen_descriptor_ids.contains(descriptor->descriptor_id)) {
                descriptors.remove_first_matching([&descriptor](Descriptor const& existing) {
                    return existing.descriptor_id == descriptor->descriptor_id;
                });
            } else {
                seen_descriptor_ids.set(descriptor->descriptor_id);
            }
            descriptors.append(descriptor.release_value());
        }
    });

    return CSSFontFaceRule::create(realm(), CSSFontFaceDescriptors::create(realm(), move(descriptors)));
}

}
