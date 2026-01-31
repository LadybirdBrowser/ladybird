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

#include <LibWeb/CSS/CSSCounterStyleRule.h>
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
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
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
            // https://compat.spec.whatwg.org/#css-at-rules
            // @-webkit-keyframes must be supported as an alias of @keyframes.
            if (at_rule.name.equals_ignoring_ascii_case("keyframes"sv) || at_rule.name.equals_ignoring_ascii_case("-webkit-keyframes"sv))
                return convert_to_keyframes_rule(at_rule);

            if (has_ignored_vendor_prefix(at_rule.name))
                return {};

            if (at_rule.name.equals_ignoring_ascii_case("counter-style"sv))
                return convert_to_counter_style_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("font-face"sv))
                return convert_to_font_face_rule(at_rule);

            if (at_rule.name.equals_ignoring_ascii_case("import"sv))
                return convert_to_import_rule(at_rule);

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
            ErrorReporter::the().report(UnknownRuleError { .rule_name = MUST(String::formatted("@{}", at_rule.name)) });
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
            ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                .rule_name = "style"_fly_string,
                .prelude = prelude_stream.dump_string(),
                .description = "Selectors invalid."_string,
            });
        }
        return {};
    }

    if (maybe_selectors.value().is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "style"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Empty selector."_string,
        });
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
                        ErrorReporter::the().report(InvalidRuleLocationError {
                            .outer_rule_name = "style"_fly_string,
                            .inner_rule_name = MUST(FlyString::from_utf8(converted_rule->class_name())),
                        });
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
    TokenStream tokens { rule.prelude };

    if (rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@import"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Must be a statement, not a block."_string,
        });
        return {};
    }

    if (rule.prelude.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@import"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Empty prelude."_string,
        });
        return {};
    }

    tokens.discard_whitespace();

    Optional<URL> url = parse_url_function(tokens);
    if (!url.has_value() && tokens.next_token().is(Token::Type::String))
        url = URL { tokens.consume_a_token().token().string().to_string() };

    if (!url.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@import"_fly_string,
            .prelude = tokens.dump_string(),
            .description = MUST(String::formatted("Unable to parse `{}` as URL.", tokens.next_token().to_debug_string())),
        });
        return {};
    }

    tokens.discard_whitespace();
    Optional<FlyString> layer;
    // [ layer | layer(<layer-name>) ]?
    if (tokens.next_token().is_ident("layer"sv)) {
        tokens.discard_a_token(); // layer
        layer = FlyString {};
    } else if (tokens.next_token().is_function("layer"sv)) {
        auto layer_transaction = tokens.begin_transaction();
        auto& layer_function = tokens.consume_a_token().function();
        TokenStream layer_tokens { layer_function.value };
        auto name = parse_layer_name(layer_tokens, AllowBlankLayerName::No);
        layer_tokens.discard_whitespace();
        if (!name.has_value() || layer_tokens.has_next_token()) {
            ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                .rule_name = "@import"_fly_string,
                .prelude = tokens.dump_string(),
                .description = MUST(String::formatted("Unable to parse `{}` as a valid layer.", layer_function.original_source_text())),
            });
        } else {
            layer_transaction.commit();
            layer = name.release_value();
        }
    }

    // <import-conditions> = [ supports( [ <supports-condition> | <declaration> ] ) ]?
    //                      <media-query-list>?
    tokens.discard_whitespace();
    RefPtr<Supports> supports {};
    if (tokens.next_token().is_function("supports"sv)) {
        auto component_value = tokens.consume_a_token();
        TokenStream supports_tokens { component_value.function().value };
        supports = parse_a_supports(supports_tokens);
        if (!supports) {
            m_rule_context.append(RuleContext::SupportsCondition);
            auto supports_declaration = parse_supports_declaration(supports_tokens);
            m_rule_context.take_last();
            if (supports_declaration)
                supports = Supports::create(supports_declaration.release_nonnull<BooleanExpression>());
        }
    }

    auto media_query_list = parse_a_media_query_list(tokens);

    if (tokens.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@import"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Trailing tokens in prelude."_string,
        });
        return {};
    }

    return CSSImportRule::create(realm(), url.release_value(), const_cast<DOM::Document*>(m_document.ptr()), move(layer), move(supports), MediaList::create(realm(), move(media_query_list)));
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
    if (rule.is_block_rule) {
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
            ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                .rule_name = "@layer"_fly_string,
                .prelude = prelude_tokens.dump_string(),
                .description = "Not a valid layer name."_string,
            });
            return {};
        }

        prelude_tokens.discard_whitespace();
        if (prelude_tokens.has_next_token()) {
            ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                .rule_name = "@layer"_fly_string,
                .prelude = prelude_tokens.dump_string(),
                .description = "Trailing tokens after name in prelude."_string,
            });
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
    auto prelude_tokens = TokenStream { rule.prelude };
    prelude_tokens.discard_whitespace();
    Vector<FlyString> layer_names;
    while (prelude_tokens.has_next_token()) {
        // Comma
        if (!layer_names.is_empty()) {
            if (auto comma = prelude_tokens.consume_a_token(); !comma.is(Token::Type::Comma)) {
                ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                    .rule_name = "@layer"_fly_string,
                    .prelude = prelude_tokens.dump_string(),
                    .description = "Missing comma between layer names."_string,
                });
                return {};
            }
            prelude_tokens.discard_whitespace();
        }

        if (auto name = parse_layer_name(prelude_tokens, AllowBlankLayerName::No); name.has_value()) {
            layer_names.append(name.release_value());
        } else {
            ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                .rule_name = "@layer"_fly_string,
                .prelude = prelude_tokens.dump_string(),
                .description = "Contains invalid layer name."_string,
            });
            return {};
        }
        prelude_tokens.discard_whitespace();
    }

    if (layer_names.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@layer"_fly_string,
            .prelude = prelude_tokens.dump_string(),
            .description = "No layer names provided."_string,
        });
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
    auto prelude_stream = TokenStream { rule.prelude };
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.prelude.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Empty prelude."_string,
        });
        return {};
    }

    prelude_stream.discard_whitespace();
    auto& token = prelude_stream.consume_a_token();
    if (!token.is_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Name must be a <string> or <ident>."_string,
        });
        return {};
    }

    auto name_token = token.token();
    prelude_stream.discard_whitespace();

    if (prelude_stream.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Trailing tokens after name in prelude."_string,
        });
        return {};
    }

    if (name_token.is(Token::Type::Ident) && (is_css_wide_keyword(name_token.ident()) || name_token.ident().is_one_of_ignoring_ascii_case("none"sv, "default"sv))) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Invalid name."_string,
        });
        return {};
    }

    if (!name_token.is(Token::Type::String) && !name_token.is(Token::Type::Ident)) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@keyframes"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Name must be a <string> or <ident>."_string,
        });
        return {};
    }

    auto name = name_token.to_string();

    GC::RootVector<GC::Ref<CSSRule>> keyframes(realm().heap());
    rule.for_each_as_qualified_rule_list([&](auto& qualified_rule) {
        if (!qualified_rule.child_rules.is_empty()) {
            for (auto const& child_rule : qualified_rule.child_rules) {
                ErrorReporter::the().report(InvalidRuleLocationError {
                    .outer_rule_name = "@keyframes"_fly_string,
                    .inner_rule_name = child_rule.visit(
                        [](Rule const& rule) {
                            return rule.visit(
                                [](AtRule const& at_rule) { return MUST(String::formatted("@{}", at_rule.name)); },
                                [](QualifiedRule const&) { return "qualified-rule"_string; });
                        },
                        [](auto&) {
                            return "list-of-declarations"_string;
                        }),
                });
            }
        }

        auto selectors = Vector<CSS::Percentage> {};
        TokenStream child_tokens { qualified_rule.prelude };
        while (child_tokens.has_next_token()) {
            child_tokens.discard_whitespace();
            if (!child_tokens.has_next_token())
                break;
            auto tok = child_tokens.consume_a_token();
            if (!tok.is_token()) {
                ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
                    .rule_name = "keyframe"_fly_string,
                    .prelude = child_tokens.dump_string(),
                    .description = "Invalid selector."_string,
                });
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
        qualified_rule.for_each_as_declaration_list("keyframe"_fly_string, [&](auto const& declaration) {
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
    auto tokens = TokenStream { rule.prelude };
    if (rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@namespace"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Must be a statement, not a block."_string,
        });
        return {};
    }

    if (rule.prelude.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@namespace"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Empty prelude."_string,
        });
        return {};
    }

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
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@namespace"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Unable to parse <url>."_string,
        });
        return {};
    }

    tokens.discard_whitespace();
    if (tokens.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@namespace"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Trailing tokens after <url> in prelude."_string,
        });
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
    auto supports_tokens = TokenStream { rule.prelude };
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_fly_string,
            .prelude = supports_tokens.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return {};
    }

    if (rule.prelude.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_fly_string,
            .prelude = supports_tokens.dump_string(),
            .description = "Empty prelude."_string,
        });
        return {};
    }

    auto supports = parse_a_supports(supports_tokens);
    if (!supports) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@supports"_fly_string,
            .prelude = supports_tokens.dump_string(),
            .description = "Supports clause invalid."_string,
        });
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
    auto prelude_stream = TokenStream { rule.prelude };
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return {};
    }

    if (rule.prelude.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Empty prelude."_string,
        });
        return {};
    }

    prelude_stream.discard_whitespace();
    auto const& token = prelude_stream.consume_a_token();
    if (!token.is_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Name must be an ident."_string,
        });
        return {};
    }

    auto name_token = token.token();
    prelude_stream.discard_whitespace();

    if (prelude_stream.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Trailing tokens after name in prelude."_string,
        });
        return {};
    }

    if (!name_token.is(Token::Type::Ident) || !is_a_custom_property_name_string(name_token.ident())) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@property"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Name must be an ident starting with '--'."_string,
        });
        return {};
    }

    auto const& name = name_token.ident();

    Optional<FlyString> syntax_maybe;
    Optional<bool> inherits_maybe;
    RefPtr<StyleValue const> initial_value_maybe;

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

    // @property rules require a syntax and inherits descriptor; if either are missing, the entire rule is invalid and must be ignored.
    if (!syntax_maybe.has_value() || syntax_maybe->is_empty() || !inherits_maybe.has_value()) {
        return {};
    }

    CSS::Parser::ParsingParams parsing_params;
    if (document())
        parsing_params = CSS::Parser::ParsingParams { *document() };
    else
        parsing_params = CSS::Parser::ParsingParams { realm() };

    auto syntax_component_values = parse_component_values_list(parsing_params, syntax_maybe.value());
    auto maybe_syntax = parse_as_syntax(syntax_component_values);

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
        initial_value_maybe = Web::CSS::Parser::parse_with_a_syntax(parsing_params, initial_value_maybe->tokenize(), *maybe_syntax);
        // Otherwise, if the value of the syntax descriptor is not the universal syntax definition,
        // the following conditions must be met for the @property rule to be valid:
        //  - The initial-value descriptor must be present.
        //  - The initial-value descriptor’s value must parse successfully according to the grammar specified by the syntax definition.
        //  - FIXME: The initial-value must be computationally independent.

        if (!initial_value_maybe || initial_value_maybe->is_guaranteed_invalid()) {
            return {};
        }
    }

    return CSSPropertyRule::create(realm(), name, syntax_maybe.value(), inherits_maybe.value(), move(initial_value_maybe));
}

GC::Ptr<CSSCounterStyleRule> Parser::convert_to_counter_style_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-counter-styles-3/#the-counter-style-rule
    TokenStream prelude_stream { rule.prelude };
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    if (rule.prelude.is_empty()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Empty prelude."_string,
        });
        return nullptr;
    }

    auto name = parse_counter_style_name(prelude_stream);
    if (!name.has_value()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Missing counter style name."_string,
        });
        return nullptr;
    }

    prelude_stream.discard_whitespace();
    if (prelude_stream.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Trailing tokens after name in prelude."_string,
        });
        return nullptr;
    }

    // https://drafts.csswg.org/css-counter-styles-3/#typedef-counter-style-name
    // When used here, to define a counter style, it also cannot be any of the non-overridable counter-style names
    // FIXME: We should allow these in the UA stylesheet in order to initially define them.
    if (CSSCounterStyleRule::matches_non_overridable_counter_style_name(name.value())) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@counter-style"_fly_string,
            .prelude = prelude_stream.dump_string(),
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

        switch (descriptor->descriptor_id) {
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

    return CSSCounterStyleRule::create(realm(), name.release_value(), move(system), move(negative), move(prefix), move(suffix), move(range), move(pad), move(fallback), move(symbols), move(additive_symbols), move(speak_as));
}

GC::Ptr<CSSFontFaceRule> Parser::convert_to_font_face_rule(AtRule const& rule)
{
    // https://drafts.csswg.org/css-fonts/#font-face-rule
    TokenStream prelude_stream { rule.prelude };
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@font-face"_fly_string,
            .prelude = prelude_stream.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    prelude_stream.discard_whitespace();
    if (prelude_stream.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@font-face"_fly_string,
            .prelude = prelude_stream.dump_string(),
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

    return CSSFontFaceRule::create(realm(), CSSFontFaceDescriptors::create(realm(), descriptors.release_descriptors()));
}

GC::Ptr<CSSPageRule> Parser::convert_to_page_rule(AtRule const& page_rule)
{
    // https://drafts.csswg.org/css-page-3/#syntax-page-selector
    // @page = @page <page-selector-list>? { <declaration-rule-list> }
    TokenStream tokens { page_rule.prelude };
    if (!page_rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = "@page"_fly_string,
            .prelude = tokens.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

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
                    ErrorReporter::the().report(InvalidRuleLocationError {
                        .outer_rule_name = "@page"_fly_string,
                        .inner_rule_name = MUST(FlyString::from_utf8(converted_rule->class_name())),
                    });
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
    TokenStream prelude_stream { rule.prelude };
    if (!rule.is_block_rule) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = MUST(String::formatted("@{}", rule.name)),
            .prelude = prelude_stream.dump_string(),
            .description = "Must be a block, not a statement."_string,
        });
        return nullptr;
    }

    prelude_stream.discard_whitespace();
    if (prelude_stream.has_next_token()) {
        ErrorReporter::the().report(CSS::Parser::InvalidRuleError {
            .rule_name = MUST(String::formatted("@{}", rule.name)),
            .prelude = prelude_stream.dump_string(),
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
    auto style = CSSStyleProperties::create(realm(), move(properties.properties), move(properties.custom_properties));
    return CSSMarginRule::create(realm(), rule.name, style);
}

}
