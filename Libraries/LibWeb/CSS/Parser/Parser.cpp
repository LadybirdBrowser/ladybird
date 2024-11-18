/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tommy van der Vorst <tommy@pixelspark.nl>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/NonnullRawPtr.h>
#include <AK/SourceLocation.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/EdgeRect.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColor.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSHSL.h>
#include <LibWeb/CSS/StyleValues/CSSHWB.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CSSLCHLike.h>
#include <LibWeb/CSS/StyleValues/CSSLabLike.h>
#include <LibWeb/CSS/StyleValues/CSSRGB.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RotationStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>

static void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(CSS_PARSER_DEBUG, "Parse error (CSS) {}", location);
}

namespace Web::CSS::Parser {

Parser Parser::create(ParsingContext const& context, StringView input, StringView encoding)
{
    auto tokens = Tokenizer::tokenize(input, encoding);
    return Parser { context, move(tokens) };
}

Parser::Parser(ParsingContext const& context, Vector<Token> tokens)
    : m_context(context)
    , m_tokens(move(tokens))
    , m_token_stream(m_tokens)
{
}

Parser::Parser(Parser&& other)
    : m_context(other.m_context)
    , m_tokens(move(other.m_tokens))
    , m_token_stream(m_tokens)
{
    // Moving the TokenStream directly from `other` would break it, because TokenStream holds
    // a reference to the Vector<Token>, so it would be pointing at the old Parser's tokens.
    // So instead, we create a new TokenStream from this Parser's tokens, and then tell it to
    // copy the other TokenStream's state. This is quite hacky.
    m_token_stream.copy_state({}, other.m_token_stream);
}

// https://drafts.csswg.org/css-syntax/#parse-stylesheet
template<typename T>
Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<T>& input, Optional<URL::URL> location)
{
    // To parse a stylesheet from an input given an optional url location:

    // 1. If input is a byte stream for a stylesheet, decode bytes from input, and set input to the result.
    // 2. Normalize input, and set input to the result.
    // NOTE: These are done automatically when creating the Parser.

    // 3. Create a new stylesheet, with its location set to location (or null, if location was not passed).
    ParsedStyleSheet style_sheet;
    style_sheet.location = move(location);

    // 4. Consume a stylesheet’s contents from input, and set the stylesheet’s rules to the result.
    style_sheet.rules = consume_a_stylesheets_contents(input);

    // 5. Return the stylesheet.
    return style_sheet;
}

// https://drafts.csswg.org/css-syntax/#parse-a-stylesheets-contents
template<typename T>
Vector<Rule> Parser::parse_a_stylesheets_contents(TokenStream<T>& input)
{
    // To parse a stylesheet’s contents from input:

    // 1. Normalize input, and set input to the result.
    // NOTE: This is done automatically when creating the Parser.

    // 2. Consume a stylesheet’s contents from input, and return the result.
    return consume_a_stylesheets_contents(input);
}

// https://drafts.csswg.org/css-syntax/#parse-a-css-stylesheet
CSSStyleSheet* Parser::parse_as_css_stylesheet(Optional<URL::URL> location)
{
    // To parse a CSS stylesheet, first parse a stylesheet.
    auto const& style_sheet = parse_a_stylesheet(m_token_stream, {});

    // Interpret all of the resulting top-level qualified rules as style rules, defined below.
    GC::MarkedVector<CSSRule*> rules(m_context.realm().heap());
    for (auto const& raw_rule : style_sheet.rules) {
        auto rule = convert_to_rule(raw_rule, Nested::No);
        // If any style rule is invalid, or any at-rule is not recognized or is invalid according to its grammar or context, it’s a parse error.
        // Discard that rule.
        if (!rule) {
            log_parse_error();
            continue;
        }
        rules.append(rule);
    }

    auto rule_list = CSSRuleList::create(m_context.realm(), rules);
    auto media_list = MediaList::create(m_context.realm(), {});
    return CSSStyleSheet::create(m_context.realm(), rule_list, media_list, move(location));
}

RefPtr<Supports> Parser::parse_as_supports()
{
    return parse_a_supports(m_token_stream);
}

template<typename T>
RefPtr<Supports> Parser::parse_a_supports(TokenStream<T>& tokens)
{
    auto component_values = parse_a_list_of_component_values(tokens);
    TokenStream<ComponentValue> token_stream { component_values };
    m_rule_context.append(ContextType::SupportsCondition);
    auto maybe_condition = parse_supports_condition(token_stream);
    m_rule_context.take_last();
    token_stream.discard_whitespace();
    if (maybe_condition && !token_stream.has_next_token())
        return Supports::create(m_context.realm(), maybe_condition.release_nonnull());

    return {};
}
template RefPtr<Supports> Parser::parse_a_supports(TokenStream<ComponentValue>&);
template RefPtr<Supports> Parser::parse_a_supports(TokenStream<Token>&);

OwnPtr<Supports::Condition> Parser::parse_supports_condition(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto const& peeked_token = tokens.next_token();
    // `not <supports-in-parens>`
    if (peeked_token.is_ident("not"sv)) {
        tokens.discard_a_token();
        tokens.discard_whitespace();
        auto child = parse_supports_in_parens(tokens);
        if (!child.has_value())
            return {};

        transaction.commit();
        auto condition = make<Supports::Condition>();
        condition->type = Supports::Condition::Type::Not;
        condition->children.append(child.release_value());
        return condition;
    }

    // `  <supports-in-parens> [ and <supports-in-parens> ]*
    //  | <supports-in-parens> [ or <supports-in-parens> ]*`
    Vector<Supports::InParens> children;
    Optional<Supports::Condition::Type> condition_type {};
    auto as_condition_type = [](auto& token) -> Optional<Supports::Condition::Type> {
        if (!token.is(Token::Type::Ident))
            return {};
        auto ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("and"sv))
            return Supports::Condition::Type::And;
        if (ident.equals_ignoring_ascii_case("or"sv))
            return Supports::Condition::Type::Or;
        return {};
    };

    while (tokens.has_next_token()) {
        if (!children.is_empty()) {
            // Expect `and` or `or` here
            auto maybe_combination = as_condition_type(tokens.consume_a_token());
            if (!maybe_combination.has_value())
                return {};
            if (!condition_type.has_value()) {
                condition_type = maybe_combination.value();
            } else if (maybe_combination != condition_type) {
                return {};
            }
        }

        tokens.discard_whitespace();

        if (auto in_parens = parse_supports_in_parens(tokens); in_parens.has_value()) {
            children.append(in_parens.release_value());
        } else {
            return {};
        }

        tokens.discard_whitespace();
    }

    if (children.is_empty())
        return {};

    transaction.commit();
    auto condition = make<Supports::Condition>();
    condition->type = condition_type.value_or(Supports::Condition::Type::Or);
    condition->children = move(children);
    return condition;
}

Optional<Supports::InParens> Parser::parse_supports_in_parens(TokenStream<ComponentValue>& tokens)
{
    // `( <supports-condition> )`
    auto const& first_token = tokens.next_token();
    if (first_token.is_block() && first_token.block().is_paren()) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_a_token();
        tokens.discard_whitespace();

        TokenStream child_tokens { first_token.block().value };
        if (auto condition = parse_supports_condition(child_tokens)) {
            if (child_tokens.has_next_token())
                return {};
            transaction.commit();
            return Supports::InParens {
                .value = { condition.release_nonnull() }
            };
        }
    }

    // `<supports-feature>`
    if (auto feature = parse_supports_feature(tokens); feature.has_value()) {
        return Supports::InParens {
            .value = { feature.release_value() }
        };
    }

    // `<general-enclosed>`
    if (auto general_enclosed = parse_general_enclosed(tokens); general_enclosed.has_value()) {
        return Supports::InParens {
            .value = general_enclosed.release_value()
        };
    }

    return {};
}

Optional<Supports::Feature> Parser::parse_supports_feature(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();

    // `<supports-decl>`
    if (first_token.is_block() && first_token.block().is_paren()) {
        TokenStream block_tokens { first_token.block().value };
        // FIXME: Parsing and then converting back to a string is weird.
        if (auto declaration = consume_a_declaration(block_tokens); declaration.has_value()) {
            transaction.commit();
            return Supports::Feature {
                Supports::Declaration { declaration->to_string() }
            };
        }
    }

    // `<supports-selector-fn>`
    if (first_token.is_function("selector"sv)) {
        // FIXME: Parsing and then converting back to a string is weird.
        StringBuilder builder;
        for (auto const& item : first_token.function().value)
            builder.append(item.to_string());
        transaction.commit();
        return Supports::Feature {
            Supports::Selector { builder.to_string().release_value_but_fixme_should_propagate_errors() }
        };
    }

    return {};
}

// https://www.w3.org/TR/mediaqueries-4/#typedef-general-enclosed
Optional<GeneralEnclosed> Parser::parse_general_enclosed(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();

    // `[ <function-token> <any-value>? ) ]`
    if (first_token.is_function()) {
        transaction.commit();
        return GeneralEnclosed { first_token.to_string() };
    }

    // `( <any-value>? )`
    if (first_token.is_block() && first_token.block().is_paren()) {
        transaction.commit();
        return GeneralEnclosed { first_token.to_string() };
    }

    return {};
}

// https://drafts.csswg.org/css-syntax/#consume-stylesheet-contents
template<typename T>
Vector<Rule> Parser::consume_a_stylesheets_contents(TokenStream<T>& input)
{
    // To consume a stylesheet’s contents from a token stream input:

    // Let rules be an initially empty list of rules.
    Vector<Rule> rules;

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <whitespace-token>
        if (token.is(Token::Type::Whitespace)) {
            // Discard a token from input.
            input.discard_a_token();
            continue;
        }

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // Return rules.
            return rules;
        }

        // <CDO-token>
        // <CDC-token>
        if (token.is(Token::Type::CDO) || token.is(Token::Type::CDC)) {
            // Discard a token from input.
            input.discard_a_token();
            continue;
        }

        // <at-keyword-token>
        if (token.is(Token::Type::AtKeyword)) {
            // Consume an at-rule from input. If anything is returned, append it to rules.
            if (auto maybe_at_rule = consume_an_at_rule(input); maybe_at_rule.has_value())
                rules.append(*maybe_at_rule);
            continue;
        }

        // anything else
        {
            // Consume a qualified rule from input. If a rule is returned, append it to rules.
            consume_a_qualified_rule(input).visit(
                [&](QualifiedRule qualified_rule) { rules.append(move(qualified_rule)); },
                [](auto&) {});
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-at-rule
template<typename T>
Optional<AtRule> Parser::consume_an_at_rule(TokenStream<T>& input, Nested nested)
{
    // To consume an at-rule from a token stream input, given an optional bool nested (default false):

    // Assert: The next token is an <at-keyword-token>.
    VERIFY(input.next_token().is(Token::Type::AtKeyword));

    // Consume a token from input, and let rule be a new at-rule with its name set to the returned token’s value,
    // its prelude initially set to an empty list, and no declarations or child rules.
    AtRule rule {
        .name = ((Token)input.consume_a_token()).at_keyword(),
        .prelude = {},
        .child_rules_and_lists_of_declarations = {},
    };

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <semicolon-token>
        // <EOF-token>
        if (token.is(Token::Type::Semicolon) || token.is(Token::Type::EndOfFile)) {
            // Discard a token from input. If rule is valid in the current context, return it; otherwise return nothing.
            input.discard_a_token();
            if (is_valid_in_the_current_context(rule))
                return rule;
            return {};
        }

        // <}-token>
        if (token.is(Token::Type::CloseCurly)) {
            // If nested is true:
            if (nested == Nested::Yes) {
                // If rule is valid in the current context, return it.
                if (is_valid_in_the_current_context(rule))
                    return rule;
                // Otherwise, return nothing.
                return {};
            }
            // Otherwise, consume a token and append the result to rule’s prelude.
            else {
                rule.prelude.append(input.consume_a_token());
            }
            continue;
        }

        // <{-token>
        if (token.is(Token::Type::OpenCurly)) {
            // Consume a block from input, and assign the result to rule’s child rules.
            m_rule_context.append(context_type_for_at_rule(rule.name));
            rule.child_rules_and_lists_of_declarations = consume_a_block(input);
            m_rule_context.take_last();

            // If rule is valid in the current context, return it. Otherwise, return nothing.
            if (is_valid_in_the_current_context(rule))
                return rule;
            return {};
        }

        // anything else
        {
            // Consume a component value from input and append the returned value to rule’s prelude.
            rule.prelude.append(consume_a_component_value(input));
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-qualified-rule
template<typename T>
Variant<Empty, QualifiedRule, Parser::InvalidRuleError> Parser::consume_a_qualified_rule(TokenStream<T>& input, Optional<Token::Type> stop_token, Nested nested)
{
    // To consume a qualified rule, from a token stream input, given an optional token stop token and an optional bool nested (default false):

    // Let rule be a new qualified rule with its prelude, declarations, and child rules all initially set to empty lists.
    QualifiedRule rule {
        .prelude = {},
        .declarations = {},
        .child_rules = {},
    };

    // NOTE: Qualified rules inside @keyframes are a keyframe rule.
    //       We'll assume all others are style rules.
    auto type_of_qualified_rule = (!m_rule_context.is_empty() && m_rule_context.last() == ContextType::AtKeyframes)
        ? ContextType::Keyframe
        : ContextType::Style;

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <EOF-token>
        // stop token (if passed)
        if (token.is(Token::Type::EndOfFile) || (stop_token.has_value() && token.is(*stop_token))) {
            // This is a parse error. Return nothing.
            log_parse_error();
            return {};
        }

        // <}-token>
        if (token.is(Token::Type::CloseCurly)) {
            // This is a parse error. If nested is true, return nothing. Otherwise, consume a token and append the result to rule’s prelude.
            log_parse_error();
            if (nested == Nested::Yes)
                return {};
            rule.prelude.append(input.consume_a_token());
            continue;
        }

        // <{-token>
        if (token.is(Token::Type::OpenCurly)) {
            // If the first two non-<whitespace-token> values of rule’s prelude are an <ident-token> whose value starts with "--"
            // followed by a <colon-token>, then:
            TokenStream prelude_tokens { rule.prelude };
            prelude_tokens.discard_whitespace();
            auto& first_non_whitespace = prelude_tokens.consume_a_token();
            prelude_tokens.discard_whitespace();
            auto& second_non_whitespace = prelude_tokens.consume_a_token();
            if (first_non_whitespace.is(Token::Type::Ident) && first_non_whitespace.token().ident().starts_with_bytes("--"sv)
                && second_non_whitespace.is(Token::Type::Colon)) {
                // If nested is true, consume the remnants of a bad declaration from input, with nested set to true, and return nothing.
                if (nested == Nested::Yes) {
                    consume_the_remnants_of_a_bad_declaration(input, Nested::Yes);
                    return {};
                }

                // If nested is false, consume a block from input, and return nothing.
                (void)consume_a_block(input);
                return {};
            }

            // Otherwise, consume a block from input, and let child rules be the result.
            m_rule_context.append(type_of_qualified_rule);
            rule.child_rules = consume_a_block(input);
            m_rule_context.take_last();

            // If the first item of child rules is a list of declarations, remove it from child rules and assign it to rule’s declarations.
            if (!rule.child_rules.is_empty() && rule.child_rules.first().has<Vector<Declaration>>()) {
                auto first = rule.child_rules.take_first();
                rule.declarations = move(first.get<Vector<Declaration>>());
            }

            // If any remaining items of child rules are lists of declarations, replace them with nested declarations rules
            // containing the list as its sole child. Assign child rules to rule’s child rules.
            // NOTE: We do this later, when converting the QualifiedRule to a CSSRule type.

            // If rule is valid in the current context, return it; otherwise return an invalid rule error.
            if (is_valid_in_the_current_context(rule))
                return rule;
            return InvalidRuleError {};
        }

        // anything else
        {
            // Consume a component value from input and append the result to rule’s prelude.
            rule.prelude.append(consume_a_component_value(input));
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-block
template<typename T>
Vector<RuleOrListOfDeclarations> Parser::consume_a_block(TokenStream<T>& input)
{
    // To consume a block, from a token stream input:

    // Assert: The next token is a <{-token>.
    VERIFY(input.next_token().is(Token::Type::OpenCurly));

    // Discard a token from input.
    input.discard_a_token();
    // Consume a block’s contents from input and let rules be the result.
    auto rules = consume_a_blocks_contents(input);
    // Discard a token from input.
    input.discard_a_token();

    // Return rules.
    return rules;
}

// https://drafts.csswg.org/css-syntax/#consume-block-contents
template<typename T>
Vector<RuleOrListOfDeclarations> Parser::consume_a_blocks_contents(TokenStream<T>& input)
{
    // To consume a block’s contents from a token stream input:

    // Let rules be an empty list, containing either rules or lists of declarations.
    Vector<RuleOrListOfDeclarations> rules;

    // Let decls be an empty list of declarations.
    Vector<Declaration> declarations;

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <whitespace-token>
        // <semicolon-token>
        if (token.is(Token::Type::Whitespace) || token.is(Token::Type::Semicolon)) {
            // Discard a token from input.
            input.discard_a_token();
            continue;
        }

        // <EOF-token>
        // <}-token>
        if (token.is(Token::Type::EndOfFile) || token.is(Token::Type::CloseCurly)) {
            // AD-HOC: If decls is not empty, append it to rules.
            // Spec issue: https://github.com/w3c/csswg-drafts/issues/11017
            if (!declarations.is_empty())
                rules.append(move(declarations));
            // Return rules.
            return rules;
        }

        // <at-keyword-token>
        if (token.is(Token::Type::AtKeyword)) {
            // If decls is not empty, append it to rules, and set decls to a fresh empty list of declarations.
            if (!declarations.is_empty()) {
                rules.append(move(declarations));
                declarations = {};
            }

            // Consume an at-rule from input, with nested set to true.
            // If a rule was returned, append it to rules.
            if (auto at_rule = consume_an_at_rule(input, Nested::Yes); at_rule.has_value())
                rules.append({ at_rule.release_value() });

            continue;
        }

        // anything else
        {
            // Mark input.
            input.mark();

            // Consume a declaration from input, with nested set to true.
            // If a declaration was returned, append it to decls, and discard a mark from input.
            if (auto declaration = consume_a_declaration(input, Nested::Yes); declaration.has_value()) {
                declarations.append(declaration.release_value());
                input.discard_a_mark();
            }

            // Otherwise, restore a mark from input, then consume a qualified rule from input,
            // with nested set to true, and <semicolon-token> as the stop token.
            else {
                input.restore_a_mark();
                consume_a_qualified_rule(input, Token::Type::Semicolon, Nested::Yes).visit(
                    // -> If nothing was returned
                    [](Empty&) {
                        // Do nothing
                    },
                    // -> If an invalid rule error was returned
                    [&](InvalidRuleError&) {
                        // If decls is not empty, append decls to rules, and set decls to a fresh empty list of declarations. (Otherwise, do nothing.)
                        if (!declarations.is_empty()) {
                            rules.append(move(declarations));
                            declarations = {};
                        }
                    },
                    // -> If a rule was returned
                    [&](QualifiedRule rule) {
                        // If decls is not empty, append decls to rules, and set decls to a fresh empty list of declarations.
                        if (!declarations.is_empty()) {
                            rules.append(move(declarations));
                            declarations = {};
                        }
                        // Append the rule to rules.
                        rules.append({ move(rule) });
                    });
            }
        }
    }
}

template<>
ComponentValue Parser::consume_a_component_value<ComponentValue>(TokenStream<ComponentValue>& tokens)
{
    // Note: This overload is called once tokens have already been converted into component values,
    //       so we do not need to do the work in the more general overload.
    return tokens.consume_a_token();
}

// 5.4.7. Consume a component value
// https://drafts.csswg.org/css-syntax/#consume-component-value
template<typename T>
ComponentValue Parser::consume_a_component_value(TokenStream<T>& input)
{
    // To consume a component value from a token stream input:

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <{-token>
        // <[-token>
        // <(-token>
        if (token.is(Token::Type::OpenCurly) || token.is(Token::Type::OpenSquare) || token.is(Token::Type::OpenParen)) {
            // Consume a simple block from input and return the result.
            return ComponentValue { consume_a_simple_block(input) };
        }

        // <function-token>
        if (token.is(Token::Type::Function)) {
            // Consume a function from input and return the result.
            return ComponentValue { consume_a_function(input) };
        }

        // anything else
        {
            // Consume a token from input and return the result.
            return ComponentValue { input.consume_a_token() };
        }
    }
}

template<typename T>
Vector<ComponentValue> Parser::consume_a_list_of_component_values(TokenStream<T>& input, Optional<Token::Type> stop_token, Nested nested)
{
    // To consume a list of component values from a token stream input, given an optional token stop token
    // and an optional boolean nested (default false):

    // Let values be an empty list of component values.
    Vector<ComponentValue> values;

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <eof-token>
        // stop token (if passed)
        if (token.is(Token::Type::EndOfFile) || (stop_token.has_value() && token.is(*stop_token))) {
            // Return values.
            return values;
        }

        // <}-token>
        if (token.is(Token::Type::CloseCurly)) {
            // If nested is true, return values.
            if (nested == Nested::Yes) {
                return values;
            }
            // Otherwise, this is a parse error. Consume a token from input and append the result to values.
            else {
                log_parse_error();
                values.append(input.consume_a_token());
            }
        }

        // anything else
        {
            // Consume a component value from input, and append the result to values.
            values.append(consume_a_component_value(input));
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-simple-block
template<typename T>
SimpleBlock Parser::consume_a_simple_block(TokenStream<T>& input)
{
    // To consume a simple block from a token stream input:

    // Assert: the next token of input is <{-token>, <[-token>, or <(-token>.
    auto& next = input.next_token();
    VERIFY(next.is(Token::Type::OpenCurly) || next.is(Token::Type::OpenSquare) || next.is(Token::Type::OpenParen));

    // Let ending token be the mirror variant of the next token. (E.g. if it was called with <[-token>, the ending token is <]-token>.)
    auto ending_token = ((Token)input.next_token()).mirror_variant();

    // Let block be a new simple block with its associated token set to the next token and with its value initially set to an empty list.
    SimpleBlock block {
        .token = input.next_token(),
        .value = {},
    };

    // Discard a token from input.
    input.discard_a_token();

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <eof-token>
        // ending token
        if (token.is(Token::Type::EndOfFile) || token.is(ending_token)) {
            // Discard a token from input. Return block.
            // AD-HOC: Store the token instead as the "end token"
            block.end_token = input.consume_a_token();
            return block;
        }

        // anything else
        {
            // Consume a component value from input and append the result to block’s value.
            block.value.empend(move(consume_a_component_value(input)));
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-function
template<typename T>
Function Parser::consume_a_function(TokenStream<T>& input)
{
    // To consume a function from a token stream input:

    // Assert: The next token is a <function-token>.
    VERIFY(input.next_token().is(Token::Type::Function));

    // Consume a token from input, and let function be a new function with its name equal the returned token’s value,
    // and a value set to an empty list.
    auto name_token = ((Token)input.consume_a_token());
    Function function {
        .name = name_token.function(),
        .value = {},
        .name_token = name_token,
    };

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <eof-token>
        // <)-token>
        if (token.is(Token::Type::EndOfFile) || token.is(Token::Type::CloseParen)) {
            // Discard a token from input. Return function.
            // AD-HOC: Store the token instead as the "end token"
            function.end_token = input.consume_a_token();
            return function;
        }

        // anything else
        {
            // Consume a component value from input and append the result to function’s value.
            function.value.append(consume_a_component_value(input));
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-declaration
template<typename T>
Optional<Declaration> Parser::consume_a_declaration(TokenStream<T>& input, Nested nested)
{
    // To consume a declaration from a token stream input, given an optional bool nested (default false):

    // TODO: As noted in the "Implementation note" below https://drafts.csswg.org/css-syntax/#consume-block-contents
    //       there are ways we can optimise this by early-exiting.

    // Let decl be a new declaration, with an initially empty name and a value set to an empty list.
    Declaration declaration {
        .name {},
        .value {},
    };

    // 1. If the next token is an <ident-token>, consume a token from input and set decl’s name to the token’s value.
    if (input.next_token().is(Token::Type::Ident)) {
        declaration.name = ((Token)input.consume_a_token()).ident();
    }
    //    Otherwise, consume the remnants of a bad declaration from input, with nested, and return nothing.
    else {
        consume_the_remnants_of_a_bad_declaration(input, nested);
        return {};
    }

    // 2. Discard whitespace from input.
    input.discard_whitespace();

    // 3. If the next token is a <colon-token>, discard a token from input.
    if (input.next_token().is(Token::Type::Colon)) {
        input.discard_a_token();
    }
    //    Otherwise, consume the remnants of a bad declaration from input, with nested, and return nothing.
    else {
        consume_the_remnants_of_a_bad_declaration(input, nested);
        return {};
    }

    // 4. Discard whitespace from input.
    input.discard_whitespace();

    // 5. Consume a list of component values from input, with nested, and with <semicolon-token> as the stop token,
    //    and set decl’s value to the result.
    declaration.value = consume_a_list_of_component_values(input, Token::Type::Semicolon, nested);

    // 6. If the last two non-<whitespace-token>s in decl’s value are a <delim-token> with the value "!"
    //    followed by an <ident-token> with a value that is an ASCII case-insensitive match for "important",
    //    remove them from decl’s value and set decl’s important flag.
    if (declaration.value.size() >= 2) {
        // NOTE: Walk backwards from the end until we find "important"
        Optional<size_t> important_index;
        for (size_t i = declaration.value.size() - 1; i > 0; i--) {
            auto const& value = declaration.value[i];
            if (value.is_ident("important"sv)) {
                important_index = i;
                break;
            }
            if (!value.is(Token::Type::Whitespace))
                break;
        }

        // NOTE: Walk backwards from important until we find "!"
        if (important_index.has_value()) {
            Optional<size_t> bang_index;
            for (size_t i = important_index.value() - 1; i > 0; i--) {
                auto const& value = declaration.value[i];
                if (value.is_delim('!')) {
                    bang_index = i;
                    break;
                }
                if (value.is(Token::Type::Whitespace))
                    continue;
                break;
            }

            if (bang_index.has_value()) {
                declaration.value.remove(important_index.value());
                declaration.value.remove(bang_index.value());
                declaration.important = Important::Yes;
            }
        }
    }

    // 7. While the last item in decl’s value is a <whitespace-token>, remove that token.
    while (!declaration.value.is_empty() && declaration.value.last().is(Token::Type::Whitespace)) {
        declaration.value.take_last();
    }

    // See second clause of step 8.
    auto contains_a_curly_block_and_non_whitespace = [](Vector<ComponentValue> const& declaration_value) {
        bool contains_curly_block = false;
        bool contains_non_whitespace = false;
        for (auto const& value : declaration_value) {
            if (value.is_block() && value.block().is_curly()) {
                if (contains_non_whitespace)
                    return true;
                contains_curly_block = true;
                continue;
            }

            if (!value.is(Token::Type::Whitespace)) {
                if (contains_curly_block)
                    return true;
                contains_non_whitespace = true;
                continue;
            }
        }
        return false;
    };

    // 8. If decl’s name is a custom property name string, then set decl’s original text to the segment
    //    of the original source text string corresponding to the tokens of decl’s value.
    if (is_a_custom_property_name_string(declaration.name)) {
        // TODO: If we could reach inside the source string that the TokenStream uses, we could grab this as
        //       a single substring instead of having to reconstruct it.
        StringBuilder original_text;
        for (auto const& value : declaration.value) {
            original_text.append(value.original_source_text());
        }
        declaration.original_text = original_text.to_string_without_validation();
    }
    //    Otherwise, if decl’s value contains a top-level simple block with an associated token of <{-token>,
    //    and also contains any other non-<whitespace-token> value, return nothing.
    //    (That is, a top-level {}-block is only allowed as the entire value of a non-custom property.)
    else if (contains_a_curly_block_and_non_whitespace(declaration.value)) {
        return {};
    }
    //    Otherwise, if decl’s name is an ASCII case-insensitive match for "unicode-range", consume the value of
    //    a unicode-range descriptor from the segment of the original source text string corresponding to the
    //    tokens returned by the consume a list of component values call, and replace decl’s value with the result.
    else if (declaration.name.equals_ignoring_ascii_case("unicode-range"sv)) {
        // FIXME: Special unicode-range handling
    }

    // 9. If decl is valid in the current context, return it; otherwise return nothing.
    if (is_valid_in_the_current_context(declaration))
        return declaration;
    return {};
}

// https://drafts.csswg.org/css-syntax/#consume-the-remnants-of-a-bad-declaration
template<typename T>
void Parser::consume_the_remnants_of_a_bad_declaration(TokenStream<T>& input, Nested nested)
{
    // To consume the remnants of a bad declaration from a token stream input, given a bool nested:

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <eof-token>
        // <semicolon-token>
        if (token.is(Token::Type::EndOfFile) || token.is(Token::Type::Semicolon)) {
            // Discard a token from input, and return nothing.
            input.discard_a_token();
            return;
        }

        // <}-token>
        if (token.is(Token::Type::CloseCurly)) {
            // If nested is true, return nothing. Otherwise, discard a token.
            if (nested == Nested::Yes)
                return;
            input.discard_a_token();
            continue;
        }

        // anything else
        {
            // Consume a component value from input, and do nothing.
            (void)consume_a_component_value(input);
            continue;
        }
    }
}

CSSRule* Parser::parse_as_css_rule()
{
    if (auto maybe_rule = parse_a_rule(m_token_stream); maybe_rule.has_value())
        return convert_to_rule(maybe_rule.value(), Nested::No);
    return {};
}

// https://drafts.csswg.org/css-syntax/#parse-rule
template<typename T>
Optional<Rule> Parser::parse_a_rule(TokenStream<T>& input)
{
    // To parse a rule from input:
    Optional<Rule> rule;

    // 1. Normalize input, and set input to the result.
    // NOTE: This is done when initializing the Parser.

    // 2. Discard whitespace from input.
    input.discard_whitespace();

    // 3. If the next token from input is an <EOF-token>, return a syntax error.
    if (input.next_token().is(Token::Type::EndOfFile)) {
        return {};
    }
    //    Otherwise, if the next token from input is an <at-keyword-token>,
    //    consume an at-rule from input, and let rule be the return value.
    else if (input.next_token().is(Token::Type::AtKeyword)) {
        rule = consume_an_at_rule(m_token_stream).map([](auto& it) { return Rule { it }; });
    }
    //    Otherwise, consume a qualified rule from input and let rule be the return value.
    //    If nothing or an invalid rule error was returned, return a syntax error.
    else {
        consume_a_qualified_rule(input).visit(
            [&](QualifiedRule qualified_rule) { rule = move(qualified_rule); },
            [](auto&) {});

        if (!rule.has_value())
            return {};
    }

    // 4. Discard whitespace from input.
    input.discard_whitespace();

    // 5. If the next token from input is an <EOF-token>, return rule. Otherwise, return a syntax error.
    if (input.next_token().is(Token::Type::EndOfFile))
        return rule;
    return {};
}

// https://drafts.csswg.org/css-syntax/#parse-block-contents
template<typename T>
Vector<RuleOrListOfDeclarations> Parser::parse_a_blocks_contents(TokenStream<T>& input)
{
    // To parse a block’s contents from input:

    // 1. Normalize input, and set input to the result.
    // NOTE: Done by constructing the Parser.

    // 2. Consume a block’s contents from input, and return the result.
    return consume_a_blocks_contents(input);
}

Optional<StyleProperty> Parser::parse_as_supports_condition()
{
    m_rule_context.append(ContextType::SupportsCondition);
    auto maybe_declaration = parse_a_declaration(m_token_stream);
    m_rule_context.take_last();
    if (maybe_declaration.has_value())
        return convert_to_style_property(maybe_declaration.release_value());
    return {};
}

// https://drafts.csswg.org/css-syntax/#parse-declaration
template<typename T>
Optional<Declaration> Parser::parse_a_declaration(TokenStream<T>& input)
{
    // To parse a declaration from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Discard whitespace from input.
    input.discard_whitespace();

    // 3. Consume a declaration from input. If anything was returned, return it. Otherwise, return a syntax error.
    if (auto declaration = consume_a_declaration(input); declaration.has_value())
        return declaration.release_value();
    // FIXME: Syntax error
    return {};
}

Optional<ComponentValue> Parser::parse_as_component_value()
{
    return parse_a_component_value(m_token_stream);
}

// https://drafts.csswg.org/css-syntax/#parse-component-value
template<typename T>
Optional<ComponentValue> Parser::parse_a_component_value(TokenStream<T>& input)
{
    // To parse a component value from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Discard whitespace from input.
    input.discard_whitespace();

    // 3. If input is empty, return a syntax error.
    // FIXME: Syntax error
    if (input.is_empty())
        return {};

    // 4. Consume a component value from input and let value be the return value.
    auto value = consume_a_component_value(input);

    // 5. Discard whitespace from input.
    input.discard_whitespace();

    // 6. If input is empty, return value. Otherwise, return a syntax error.
    if (input.is_empty())
        return move(value);
    // FIXME: Syntax error
    return {};
}

// https://drafts.csswg.org/css-syntax/#parse-list-of-component-values
template<typename T>
Vector<ComponentValue> Parser::parse_a_list_of_component_values(TokenStream<T>& input)
{
    // To parse a list of component values from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Consume a list of component values from input, and return the result.
    return consume_a_list_of_component_values(input);
}

// https://drafts.csswg.org/css-syntax/#parse-comma-separated-list-of-component-values
template<typename T>
Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<T>& input)
{
    // To parse a comma-separated list of component values from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Let groups be an empty list.
    Vector<Vector<ComponentValue>> groups;

    // 3. While input is not empty:
    while (!input.is_empty()) {

        // 1. Consume a list of component values from input, with <comma-token> as the stop token, and append the result to groups.
        groups.append(consume_a_list_of_component_values(input, Token::Type::Comma));

        // 2. Discard a token from input.
        input.discard_a_token();
    }

    // 4. Return groups.
    return groups;
}
template Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<ComponentValue>&);
template Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<Token>&);

ElementInlineCSSStyleDeclaration* Parser::parse_as_style_attribute(DOM::Element& element)
{
    auto expand_shorthands = [&](Vector<StyleProperty>& properties) -> Vector<StyleProperty> {
        Vector<StyleProperty> expanded_properties;
        for (auto& property : properties) {
            if (property_is_shorthand(property.property_id)) {
                StyleComputer::for_each_property_expanding_shorthands(property.property_id, *property.value, StyleComputer::AllowUnresolved::Yes, [&](PropertyID longhand_property_id, CSSStyleValue const& longhand_value) {
                    expanded_properties.append(CSS::StyleProperty {
                        .important = property.important,
                        .property_id = longhand_property_id,
                        .value = longhand_value,
                    });
                });
            } else {
                expanded_properties.append(property);
            }
        }
        return expanded_properties;
    };

    m_rule_context.append(ContextType::Style);
    auto declarations_and_at_rules = parse_a_blocks_contents(m_token_stream);
    m_rule_context.take_last();

    auto [properties, custom_properties] = extract_properties(declarations_and_at_rules);
    auto expanded_properties = expand_shorthands(properties);
    return ElementInlineCSSStyleDeclaration::create(element, move(expanded_properties), move(custom_properties));
}

Optional<URL::URL> Parser::parse_url_function(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();

    auto convert_string_to_url = [&](StringView url_string) -> Optional<URL::URL> {
        auto url = m_context.complete_url(url_string);
        if (url.is_valid()) {
            transaction.commit();
            return url;
        }
        return {};
    };

    if (component_value.is(Token::Type::Url)) {
        auto url_string = component_value.token().url();
        return convert_string_to_url(url_string);
    }
    if (component_value.is_function("url"sv)) {
        auto const& function_values = component_value.function().value;
        // FIXME: Handle url-modifiers. https://www.w3.org/TR/css-values-4/#url-modifiers
        for (size_t i = 0; i < function_values.size(); ++i) {
            auto const& value = function_values[i];
            if (value.is(Token::Type::Whitespace))
                continue;
            if (value.is(Token::Type::String)) {
                auto url_string = value.token().string();
                return convert_string_to_url(url_string);
            }
            break;
        }
    }

    return {};
}

RefPtr<CSSStyleValue> Parser::parse_url_value(TokenStream<ComponentValue>& tokens)
{
    auto url = parse_url_function(tokens);
    if (!url.has_value())
        return nullptr;
    return URLStyleValue::create(*url);
}

// https://www.w3.org/TR/css-shapes-1/#typedef-shape-radius
Optional<ShapeRadius> Parser::parse_shape_radius(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto maybe_radius = parse_length_percentage(tokens);
    if (maybe_radius.has_value()) {
        // Negative radius is invalid.
        auto radius = maybe_radius.value();
        if ((radius.is_length() && radius.length().raw_value() < 0) || (radius.is_percentage() && radius.percentage().value() < 0))
            return {};

        transaction.commit();
        return radius;
    }

    if (tokens.next_token().is_ident("closest-side"sv)) {
        tokens.discard_a_token();
        transaction.commit();
        return FitSide::ClosestSide;
    }

    if (tokens.next_token().is_ident("farthest-side"sv)) {
        tokens.discard_a_token();
        transaction.commit();
        return FitSide::FarthestSide;
    }

    return {};
}

RefPtr<CSSStyleValue> Parser::parse_basic_shape_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();
    if (!component_value.is_function())
        return nullptr;

    auto function_name = component_value.function().name.bytes_as_string_view();

    // FIXME: Implement path(). See: https://www.w3.org/TR/css-shapes-1/#basic-shape-functions
    if (function_name.equals_ignoring_ascii_case("inset"sv)) {
        // inset() = inset( <length-percentage>{1,4} [ round <'border-radius'> ]? )
        // FIXME: Parse the border-radius.
        auto arguments_tokens = TokenStream { component_value.function().value };

        // If less than four <length-percentage> values are provided,
        // the omitted values default in the same way as the margin shorthand:
        // an omitted second or third value defaults to the first, and an omitted fourth value defaults to the second.

        // The four <length-percentage>s define the position of the top, right, bottom, and left edges of a rectangle.

        arguments_tokens.discard_whitespace();
        auto top = parse_length_percentage(arguments_tokens);
        if (!top.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto right = parse_length_percentage(arguments_tokens);
        if (!right.has_value())
            right = top;

        arguments_tokens.discard_whitespace();
        auto bottom = parse_length_percentage(arguments_tokens);
        if (!bottom.has_value())
            bottom = top;

        arguments_tokens.discard_whitespace();
        auto left = parse_length_percentage(arguments_tokens);
        if (!left.has_value())
            left = right;

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Inset { LengthBox(top.value(), right.value(), bottom.value(), left.value()) });
    }

    if (function_name.equals_ignoring_ascii_case("xywh"sv)) {
        // xywh() = xywh( <length-percentage>{2} <length-percentage [0,∞]>{2} [ round <'border-radius'> ]? )
        // FIXME: Parse the border-radius.
        auto arguments_tokens = TokenStream { component_value.function().value };

        arguments_tokens.discard_whitespace();
        auto x = parse_length_percentage(arguments_tokens);
        if (!x.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto y = parse_length_percentage(arguments_tokens);
        if (!y.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto width = parse_length_percentage(arguments_tokens);
        if (!width.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        auto height = parse_length_percentage(arguments_tokens);
        if (!height.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        // Negative width or height is invalid.
        if ((width->is_length() && width->length().raw_value() < 0)
            || (width->is_percentage() && width->percentage().value() < 0)
            || (height->is_length() && height->length().raw_value() < 0)
            || (height->is_percentage() && height->percentage().value() < 0))
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Xywh { x.value(), y.value(), width.value(), height.value() });
    }

    if (function_name.equals_ignoring_ascii_case("rect"sv)) {
        // rect() = rect( [ <length-percentage> | auto ]{4} [ round <'border-radius'> ]? )
        // FIXME: Parse the border-radius.
        auto arguments_tokens = TokenStream { component_value.function().value };

        auto parse_length_percentage_or_auto = [this](TokenStream<ComponentValue>& tokens) -> Optional<LengthPercentage> {
            tokens.discard_whitespace();
            auto value = parse_length_percentage(tokens);
            if (!value.has_value()) {
                if (tokens.consume_a_token().is_ident("auto"sv)) {
                    value = Length::make_auto();
                }
            }
            return value;
        };

        auto top = parse_length_percentage_or_auto(arguments_tokens);
        auto right = parse_length_percentage_or_auto(arguments_tokens);
        auto bottom = parse_length_percentage_or_auto(arguments_tokens);
        auto left = parse_length_percentage_or_auto(arguments_tokens);

        if (!top.has_value() || !right.has_value() || !bottom.has_value() || !left.has_value())
            return nullptr;

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Rect { LengthBox(top.value(), right.value(), bottom.value(), left.value()) });
    }

    if (function_name.equals_ignoring_ascii_case("circle"sv)) {
        // circle() = circle( <shape-radius>? [ at <position> ]? )
        auto arguments_tokens = TokenStream { component_value.function().value };

        auto radius = parse_shape_radius(arguments_tokens).value_or(FitSide::ClosestSide);

        auto position = PositionStyleValue::create_center();
        arguments_tokens.discard_whitespace();
        if (arguments_tokens.next_token().is_ident("at"sv)) {
            arguments_tokens.discard_a_token();
            arguments_tokens.discard_whitespace();
            auto maybe_position = parse_position_value(arguments_tokens);
            if (maybe_position.is_null())
                return nullptr;

            position = maybe_position.release_nonnull();
        }

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Circle { radius, position });
    }

    if (function_name.equals_ignoring_ascii_case("ellipse"sv)) {
        // ellipse() = ellipse( [ <shape-radius>{2} ]? [ at <position> ]? )
        auto arguments_tokens = TokenStream { component_value.function().value };

        Optional<ShapeRadius> radius_x = parse_shape_radius(arguments_tokens);
        Optional<ShapeRadius> radius_y = parse_shape_radius(arguments_tokens);

        if (radius_x.has_value() && !radius_y.has_value())
            return nullptr;

        if (!radius_x.has_value()) {
            radius_x = FitSide::ClosestSide;
            radius_y = FitSide::ClosestSide;
        }

        auto position = PositionStyleValue::create_center();
        arguments_tokens.discard_whitespace();
        if (arguments_tokens.next_token().is_ident("at"sv)) {
            arguments_tokens.discard_a_token();
            arguments_tokens.discard_whitespace();
            auto maybe_position = parse_position_value(arguments_tokens);
            if (maybe_position.is_null())
                return nullptr;

            position = maybe_position.release_nonnull();
        }

        arguments_tokens.discard_whitespace();
        if (arguments_tokens.has_next_token())
            return nullptr;

        transaction.commit();
        return BasicShapeStyleValue::create(Ellipse { radius_x.value(), radius_y.value(), position });
    }

    if (function_name.equals_ignoring_ascii_case("polygon"sv)) {
        // polygon() = polygon( <'fill-rule'>? , [<length-percentage> <length-percentage>]# )
        auto arguments_tokens = TokenStream { component_value.function().value };
        auto arguments = parse_a_comma_separated_list_of_component_values(arguments_tokens);

        if (arguments.size() < 1)
            return nullptr;

        Optional<Gfx::WindingRule> fill_rule;
        auto const& first_argument = arguments[0];
        TokenStream first_argument_tokens { first_argument };

        first_argument_tokens.discard_whitespace();
        if (first_argument_tokens.next_token().is_ident("nonzero"sv)) {
            fill_rule = Gfx::WindingRule::Nonzero;
        } else if (first_argument_tokens.next_token().is_ident("evenodd"sv)) {
            fill_rule = Gfx::WindingRule::EvenOdd;
        }

        if (fill_rule.has_value()) {
            first_argument_tokens.discard_a_token();
            if (first_argument_tokens.has_next_token())
                return nullptr;

            arguments.remove(0);
        } else {
            fill_rule = Gfx::WindingRule::Nonzero;
        }

        if (arguments.size() < 1)
            return nullptr;

        Vector<Polygon::Point> points;
        for (auto& argument : arguments) {
            TokenStream argument_tokens { argument };

            argument_tokens.discard_whitespace();
            auto x_pos = parse_length_percentage(argument_tokens);
            if (!x_pos.has_value())
                return nullptr;

            argument_tokens.discard_whitespace();
            auto y_pos = parse_length_percentage(argument_tokens);
            if (!y_pos.has_value())
                return nullptr;

            argument_tokens.discard_whitespace();
            if (argument_tokens.has_next_token())
                return nullptr;

            points.append(Polygon::Point { *x_pos, *y_pos });
        }

        transaction.commit();
        return BasicShapeStyleValue::create(Polygon { fill_rule.value(), move(points) });
    }

    return nullptr;
}

bool Parser::is_valid_in_the_current_context(Declaration const&) const
{
    // TODO: Determine if this *particular* declaration is valid here, not just declarations in general.

    // Declarations can't appear at the top level
    if (m_rule_context.is_empty())
        return false;

    switch (m_rule_context.last()) {
    case ContextType::Unknown:
        // If the context is an unknown type, we don't accept anything.
        return false;

    case ContextType::Style:
    case ContextType::Keyframe:
        // Style and keyframe rules contain property declarations
        return true;

    case ContextType::AtLayer:
    case ContextType::AtMedia:
    case ContextType::AtSupports:
        // Grouping rules can contain declarations if they are themselves inside a style rule
        return m_rule_context.contains_slow(ContextType::Style);

    case ContextType::AtFontFace:
    case ContextType::AtProperty:
        // @font-face and @property have descriptor declarations
        return true;

    case ContextType::AtKeyframes:
        // @keyframes can only contain keyframe rules
        return false;

    case ContextType::SupportsCondition:
        // @supports conditions accept all declarations
        return true;
    }

    VERIFY_NOT_REACHED();
}

bool Parser::is_valid_in_the_current_context(AtRule const& at_rule) const
{
    // All at-rules can appear at the top level
    if (m_rule_context.is_empty())
        return true;

    switch (m_rule_context.last()) {
    case ContextType::Unknown:
        // If the context is an unknown type, we don't accept anything.
        return false;

    case ContextType::Style:
        // Style rules can contain grouping rules
        return first_is_one_of(at_rule.name, "layer", "media", "supports");

    case ContextType::AtLayer:
    case ContextType::AtMedia:
    case ContextType::AtSupports:
        // Grouping rules can contain anything except @import or @namespace
        return !first_is_one_of(at_rule.name, "import", "namespace");

    case ContextType::SupportsCondition:
        // @supports cannot check for at-rules
        return false;

    case ContextType::AtFontFace:
    case ContextType::AtKeyframes:
    case ContextType::Keyframe:
    case ContextType::AtProperty:
        // These can't contain any at-rules
        return false;
    }

    VERIFY_NOT_REACHED();
}

bool Parser::is_valid_in_the_current_context(QualifiedRule const&) const
{
    // TODO: Different places accept different kinds of qualified rules. How do we tell them apart? Can we?

    // Top level can contain style rules
    if (m_rule_context.is_empty())
        return true;

    switch (m_rule_context.last()) {
    case ContextType::Unknown:
        // If the context is an unknown type, we don't accept anything.
        return false;

    case ContextType::Style:
        // Style rules can contain style rules
        return true;

    case ContextType::AtLayer:
    case ContextType::AtMedia:
    case ContextType::AtSupports:
        // Grouping rules can contain style rules
        return true;

    case ContextType::AtKeyframes:
        // @keyframes can contain keyframe rules
        return true;

    case ContextType::SupportsCondition:
        // @supports cannot check qualified rules
        return false;

    case ContextType::AtFontFace:
    case ContextType::AtProperty:
    case ContextType::Keyframe:
        // These can't contain qualified rules
        return false;
    }

    VERIFY_NOT_REACHED();
}

Parser::PropertiesAndCustomProperties Parser::extract_properties(Vector<RuleOrListOfDeclarations> const& rules_and_lists_of_declarations)
{
    PropertiesAndCustomProperties result;
    for (auto const& rule_or_list : rules_and_lists_of_declarations) {
        if (rule_or_list.has<Rule>())
            continue;

        auto& declarations = rule_or_list.get<Vector<Declaration>>();
        PropertiesAndCustomProperties& dest = result;
        for (auto const& declaration : declarations) {
            extract_property(declaration, dest);
        }
    }
    return result;
}

void Parser::extract_property(Declaration const& declaration, PropertiesAndCustomProperties& dest)
{
    if (auto maybe_property = convert_to_style_property(declaration); maybe_property.has_value()) {
        auto property = maybe_property.release_value();
        if (property.property_id == PropertyID::Custom) {
            dest.custom_properties.set(property.custom_name, property);
        } else {
            dest.properties.append(move(property));
        }
    }
}

PropertyOwningCSSStyleDeclaration* Parser::convert_to_style_declaration(Vector<Declaration> const& declarations)
{
    PropertiesAndCustomProperties properties;
    PropertiesAndCustomProperties& dest = properties;
    for (auto const& declaration : declarations) {
        extract_property(declaration, dest);
    }
    return PropertyOwningCSSStyleDeclaration::create(m_context.realm(), move(properties.properties), move(properties.custom_properties));
}

Optional<StyleProperty> Parser::convert_to_style_property(Declaration const& declaration)
{
    auto const& property_name = declaration.name;
    auto property_id = property_id_from_string(property_name);

    if (!property_id.has_value()) {
        if (property_name.bytes_as_string_view().starts_with("--"sv)) {
            property_id = PropertyID::Custom;
        } else if (has_ignored_vendor_prefix(property_name)) {
            return {};
        } else if (!property_name.bytes_as_string_view().starts_with('-')) {
            dbgln_if(CSS_PARSER_DEBUG, "Unrecognized CSS property '{}'", property_name);
            return {};
        }
    }

    auto value_token_stream = TokenStream(declaration.value);
    auto value = parse_css_value(property_id.value(), value_token_stream, declaration.original_text);
    if (value.is_error()) {
        if (value.error() == ParseError::SyntaxError) {
            dbgln_if(CSS_PARSER_DEBUG, "Unable to parse value for CSS property '{}'.", property_name);
            if constexpr (CSS_PARSER_DEBUG) {
                value_token_stream.dump_all_tokens();
            }
        }
        return {};
    }

    if (property_id.value() == PropertyID::Custom)
        return StyleProperty { declaration.important, property_id.value(), value.release_value(), declaration.name };

    return StyleProperty { declaration.important, property_id.value(), value.release_value(), {} };
}

RefPtr<CSSStyleValue> Parser::parse_builtin_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& component_value = tokens.consume_a_token();
    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();
        if (ident.equals_ignoring_ascii_case("inherit"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Inherit);
        }
        if (ident.equals_ignoring_ascii_case("initial"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Initial);
        }
        if (ident.equals_ignoring_ascii_case("unset"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Unset);
        }
        if (ident.equals_ignoring_ascii_case("revert"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::Revert);
        }
        if (ident.equals_ignoring_ascii_case("revert-layer"sv)) {
            transaction.commit();
            return CSSKeywordValue::create(Keyword::RevertLayer);
        }
    }

    return nullptr;
}

// https://www.w3.org/TR/css-values-4/#custom-idents
RefPtr<CustomIdentStyleValue> Parser::parse_custom_ident_value(TokenStream<ComponentValue>& tokens, std::initializer_list<StringView> blacklist)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto const& token = tokens.consume_a_token();
    if (!token.is(Token::Type::Ident))
        return nullptr;
    auto custom_ident = token.token().ident();

    // The CSS-wide keywords are not valid <custom-ident>s.
    if (is_css_wide_keyword(custom_ident))
        return nullptr;

    // The default keyword is reserved and is also not a valid <custom-ident>.
    if (custom_ident.equals_ignoring_ascii_case("default"sv))
        return nullptr;

    // Specifications using <custom-ident> must specify clearly what other keywords are excluded from <custom-ident>,
    // if any—for example by saying that any pre-defined keywords in that property’s value definition are excluded.
    // Excluded keywords are excluded in all ASCII case permutations.
    for (auto& value : blacklist) {
        if (custom_ident.equals_ignoring_ascii_case(value))
            return nullptr;
    }

    transaction.commit();
    return CustomIdentStyleValue::create(custom_ident);
}

RefPtr<CSSMathValue> Parser::parse_calculated_value(ComponentValue const& component_value)
{
    if (!component_value.is_function())
        return nullptr;

    auto const& function = component_value.function();

    auto function_node = parse_a_calc_function_node(function);
    if (!function_node)
        return nullptr;

    auto function_type = function_node->determine_type(m_context.current_property_id());
    if (!function_type.has_value())
        return nullptr;

    return CSSMathValue::create(function_node.release_nonnull(), function_type.release_value());
}

OwnPtr<CalculationNode> Parser::parse_a_calc_function_node(Function const& function)
{
    if (function.name.equals_ignoring_ascii_case("calc"sv))
        return parse_a_calculation(function.value);

    if (auto maybe_function = parse_math_function(m_context.current_property_id(), function))
        return maybe_function;

    return nullptr;
}

Optional<Dimension> Parser::parse_dimension(ComponentValue const& component_value)
{
    if (component_value.is(Token::Type::Dimension)) {
        auto numeric_value = component_value.token().dimension_value();
        auto unit_string = component_value.token().dimension_unit();

        if (auto length_type = Length::unit_from_name(unit_string); length_type.has_value())
            return Length { numeric_value, length_type.release_value() };

        if (auto angle_type = Angle::unit_from_name(unit_string); angle_type.has_value())
            return Angle { numeric_value, angle_type.release_value() };

        if (auto flex_type = Flex::unit_from_name(unit_string); flex_type.has_value())
            return Flex { numeric_value, flex_type.release_value() };

        if (auto frequency_type = Frequency::unit_from_name(unit_string); frequency_type.has_value())
            return Frequency { numeric_value, frequency_type.release_value() };

        if (auto resolution_type = Resolution::unit_from_name(unit_string); resolution_type.has_value())
            return Resolution { numeric_value, resolution_type.release_value() };

        if (auto time_type = Time::unit_from_name(unit_string); time_type.has_value())
            return Time { numeric_value, time_type.release_value() };
    }

    if (component_value.is(Token::Type::Percentage))
        return Percentage { component_value.token().percentage() };

    if (component_value.is(Token::Type::Number)) {
        auto numeric_value = component_value.token().number_value();
        if (numeric_value == 0)
            return Length::make_px(0);
        if (m_context.in_quirks_mode() && property_has_quirk(m_context.current_property_id(), Quirk::UnitlessLength)) {
            // https://quirks.spec.whatwg.org/#quirky-length-value
            // FIXME: Disallow quirk when inside a CSS sub-expression (like `calc()`)
            // "The <quirky-length> value must not be supported in arguments to CSS expressions other than the rect()
            // expression, and must not be supported in the supports() static method of the CSS interface."
            return Length::make_px(CSSPixels::nearest_value_for(numeric_value));
        }
    }

    return {};
}

Optional<AngleOrCalculated> Parser::parse_angle(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_angle()) {
            transaction.commit();
            return dimension->angle();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_angle()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<AnglePercentage> Parser::parse_angle_percentage(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_angle_percentage()) {
            transaction.commit();
            return dimension->angle_percentage();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_angle_percentage()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<FlexOrCalculated> Parser::parse_flex(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_flex()) {
            transaction.commit();
            return dimension->flex();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_flex()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<FrequencyOrCalculated> Parser::parse_frequency(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_frequency()) {
            transaction.commit();
            return dimension->frequency();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_frequency()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<FrequencyPercentage> Parser::parse_frequency_percentage(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_frequency_percentage()) {
            transaction.commit();
            return dimension->frequency_percentage();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_frequency_percentage()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<IntegerOrCalculated> Parser::parse_integer(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (token.is(Token::Type::Number) && token.token().number().is_integer()) {
        transaction.commit();
        return token.token().to_integer();
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_number()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<LengthOrCalculated> Parser::parse_length(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_length()) {
            transaction.commit();
            return dimension->length();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_length()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<LengthPercentage> Parser::parse_length_percentage(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_length_percentage()) {
            transaction.commit();
            return dimension->length_percentage();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_length_percentage()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<NumberOrCalculated> Parser::parse_number(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (token.is(Token::Type::Number)) {
        transaction.commit();
        return token.token().number_value();
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_number()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<NumberPercentage> Parser::parse_number_percentage(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (token.is(Token::Type::Number)) {
        transaction.commit();
        return token.token().number();
    }

    if (token.is(Token::Type::Percentage)) {
        transaction.commit();
        return Percentage(token.token().percentage());
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_number_percentage()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<ResolutionOrCalculated> Parser::parse_resolution(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_resolution()) {
            transaction.commit();
            return dimension->resolution();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_resolution()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<TimeOrCalculated> Parser::parse_time(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_time()) {
            transaction.commit();
            return dimension->time();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_time()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<TimePercentage> Parser::parse_time_percentage(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.consume_a_token();

    if (auto dimension = parse_dimension(token); dimension.has_value()) {
        if (dimension->is_time_percentage()) {
            transaction.commit();
            return dimension->time_percentage();
        }
        return {};
    }

    if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_time_percentage()) {
        transaction.commit();
        return calc.release_nonnull();
    }

    return {};
}

Optional<LengthOrCalculated> Parser::parse_source_size_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is_ident("auto"sv)) {
        tokens.discard_a_token(); // auto
        return LengthOrCalculated { Length::make_auto() };
    }

    return parse_length(tokens);
}

Optional<Ratio> Parser::parse_ratio(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto read_number_value = [this](ComponentValue const& component_value) -> Optional<double> {
        if (component_value.is(Token::Type::Number)) {
            return component_value.token().number_value();
        } else if (component_value.is_function()) {
            auto maybe_calc = parse_calculated_value(component_value);
            if (!maybe_calc || !maybe_calc->resolves_to_number())
                return {};
            if (auto resolved_number = maybe_calc->resolve_number(); resolved_number.has_value() && resolved_number.value() >= 0) {
                return resolved_number.value();
            }
        }
        return {};
    };

    // `<ratio> = <number [0,∞]> [ / <number [0,∞]> ]?`
    auto maybe_numerator = read_number_value(tokens.consume_a_token());
    if (!maybe_numerator.has_value() || maybe_numerator.value() < 0)
        return {};
    auto numerator = maybe_numerator.value();

    {
        auto two_value_transaction = tokens.begin_transaction();
        tokens.discard_whitespace();
        auto const& solidus = tokens.consume_a_token();
        tokens.discard_whitespace();
        auto maybe_denominator = read_number_value(tokens.consume_a_token());

        if (solidus.is_delim('/') && maybe_denominator.has_value() && maybe_denominator.value() >= 0) {
            auto denominator = maybe_denominator.value();
            // Two-value ratio
            two_value_transaction.commit();
            transaction.commit();
            return Ratio { numerator, denominator };
        }
    }

    // Single-value ratio
    transaction.commit();
    return Ratio { numerator };
}

// https://www.w3.org/TR/css-syntax-3/#urange-syntax
Optional<Gfx::UnicodeRange> Parser::parse_unicode_range(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // <urange> =
    //  u '+' <ident-token> '?'* |
    //  u <dimension-token> '?'* |
    //  u <number-token> '?'* |
    //  u <number-token> <dimension-token> |
    //  u <number-token> <number-token> |
    //  u '+' '?'+
    // (All with no whitespace in between tokens.)

    // NOTE: Parsing this is different from usual. We take these steps:
    // 1. Match the grammar above against the tokens, concatenating them into a string using their original representation.
    // 2. Then, parse that string according to the spec algorithm.
    // Step 2 is performed by calling the other parse_unicode_range() overload.

    auto is_ending_token = [](ComponentValue const& component_value) {
        return component_value.is(Token::Type::EndOfFile)
            || component_value.is(Token::Type::Comma)
            || component_value.is(Token::Type::Semicolon)
            || component_value.is(Token::Type::Whitespace);
    };

    auto create_unicode_range = [&](StringView text, auto& local_transaction) -> Optional<Gfx::UnicodeRange> {
        auto maybe_unicode_range = parse_unicode_range(text);
        if (maybe_unicode_range.has_value()) {
            local_transaction.commit();
            transaction.commit();
        }
        return maybe_unicode_range;
    };

    // All options start with 'u'/'U'.
    auto const& u = tokens.consume_a_token();
    if (!u.is_ident("u"sv)) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> does not start with 'u'");
        return {};
    }

    auto const& second_token = tokens.consume_a_token();

    //  u '+' <ident-token> '?'* |
    //  u '+' '?'+
    if (second_token.is_delim('+')) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().original_source_text());

        auto const& third_token = tokens.consume_a_token();
        if (third_token.is(Token::Type::Ident) || third_token.is_delim('?')) {
            string_builder.append(third_token.token().original_source_text());
            while (tokens.next_token().is_delim('?'))
                string_builder.append(tokens.consume_a_token().token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        }
    }

    //  u <dimension-token> '?'*
    if (second_token.is(Token::Type::Dimension)) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().original_source_text());
        while (tokens.next_token().is_delim('?'))
            string_builder.append(tokens.consume_a_token().token().original_source_text());
        if (is_ending_token(tokens.next_token()))
            return create_unicode_range(string_builder.string_view(), local_transaction);
    }

    //  u <number-token> '?'* |
    //  u <number-token> <dimension-token> |
    //  u <number-token> <number-token>
    if (second_token.is(Token::Type::Number)) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().original_source_text());

        if (is_ending_token(tokens.next_token()))
            return create_unicode_range(string_builder.string_view(), local_transaction);

        auto const& third_token = tokens.consume_a_token();
        if (third_token.is_delim('?')) {
            string_builder.append(third_token.token().original_source_text());
            while (tokens.next_token().is_delim('?'))
                string_builder.append(tokens.consume_a_token().token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        } else if (third_token.is(Token::Type::Dimension)) {
            string_builder.append(third_token.token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        } else if (third_token.is(Token::Type::Number)) {
            string_builder.append(third_token.token().original_source_text());
            if (is_ending_token(tokens.next_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        }
    }

    if constexpr (CSS_PARSER_DEBUG) {
        dbgln("CSSParser: Tokens did not match <urange> grammar.");
        tokens.dump_all_tokens();
    }
    return {};
}

Optional<Gfx::UnicodeRange> Parser::parse_unicode_range(StringView text)
{
    auto make_valid_unicode_range = [&](u32 start_value, u32 end_value) -> Optional<Gfx::UnicodeRange> {
        // https://www.w3.org/TR/css-syntax-3/#maximum-allowed-code-point
        constexpr u32 maximum_allowed_code_point = 0x10FFFF;

        // To determine what codepoints the <urange> represents:
        // 1. If end value is greater than the maximum allowed code point,
        //    the <urange> is invalid and a syntax error.
        if (end_value > maximum_allowed_code_point) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Invalid <urange>: end_value ({}) > maximum ({})", end_value, maximum_allowed_code_point);
            return {};
        }

        // 2. If start value is greater than end value, the <urange> is invalid and a syntax error.
        if (start_value > end_value) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Invalid <urange>: start_value ({}) > end_value ({})", start_value, end_value);
            return {};
        }

        // 3. Otherwise, the <urange> represents a contiguous range of codepoints from start value to end value, inclusive.
        return Gfx::UnicodeRange { start_value, end_value };
    };

    // 1. Skipping the first u token, concatenate the representations of all the tokens in the production together.
    //    Let this be text.
    // NOTE: The concatenation is already done by the caller.
    GenericLexer lexer { text };

    // 2. If the first character of text is U+002B PLUS SIGN, consume it.
    //    Otherwise, this is an invalid <urange>, and this algorithm must exit.
    if (lexer.next_is('+')) {
        lexer.consume();
    } else {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Second character of <urange> was not '+'; got: '{}'", lexer.consume());
        return {};
    }

    // 3. Consume as many hex digits from text as possible.
    //    then consume as many U+003F QUESTION MARK (?) code points as possible.
    auto start_position = lexer.tell();
    auto hex_digits = lexer.consume_while(is_ascii_hex_digit);
    auto question_marks = lexer.consume_while([](auto it) { return it == '?'; });
    //    If zero code points were consumed, or more than six code points were consumed,
    //    this is an invalid <urange>, and this algorithm must exit.
    size_t consumed_code_points = hex_digits.length() + question_marks.length();
    if (consumed_code_points == 0 || consumed_code_points > 6) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start value had {} digits/?s, expected between 1 and 6.", consumed_code_points);
        return {};
    }
    StringView start_value_code_points = text.substring_view(start_position, consumed_code_points);

    //    If any U+003F QUESTION MARK (?) code points were consumed, then:
    if (question_marks.length() > 0) {
        // 1. If there are any code points left in text, this is an invalid <urange>,
        //    and this algorithm must exit.
        if (lexer.tell_remaining() != 0) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> invalid; had {} code points left over.", lexer.tell_remaining());
            return {};
        }

        // 2. Interpret the consumed code points as a hexadecimal number,
        //    with the U+003F QUESTION MARK (?) code points replaced by U+0030 DIGIT ZERO (0) code points.
        //    This is the start value.
        auto start_value_string = start_value_code_points.replace("?"sv, "0"sv, ReplaceMode::All);
        auto maybe_start_value = AK::StringUtils::convert_to_uint_from_hex<u32>(start_value_string);
        if (!maybe_start_value.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> ?-converted start value did not parse as hex number.");
            return {};
        }
        u32 start_value = maybe_start_value.release_value();

        // 3. Interpret the consumed code points as a hexadecimal number again,
        //    with the U+003F QUESTION MARK (?) code points replaced by U+0046 LATIN CAPITAL LETTER F (F) code points.
        //    This is the end value.
        auto end_value_string = start_value_code_points.replace("?"sv, "F"sv, ReplaceMode::All);
        auto maybe_end_value = AK::StringUtils::convert_to_uint_from_hex<u32>(end_value_string);
        if (!maybe_end_value.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> ?-converted end value did not parse as hex number.");
            return {};
        }
        u32 end_value = maybe_end_value.release_value();

        // 4. Exit this algorithm.
        return make_valid_unicode_range(start_value, end_value);
    }
    //   Otherwise, interpret the consumed code points as a hexadecimal number. This is the start value.
    auto maybe_start_value = AK::StringUtils::convert_to_uint_from_hex<u32>(start_value_code_points);
    if (!maybe_start_value.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start value did not parse as hex number.");
        return {};
    }
    u32 start_value = maybe_start_value.release_value();

    // 4. If there are no code points left in text, The end value is the same as the start value.
    //    Exit this algorithm.
    if (lexer.tell_remaining() == 0)
        return make_valid_unicode_range(start_value, start_value);

    // 5. If the next code point in text is U+002D HYPHEN-MINUS (-), consume it.
    if (lexer.next_is('-')) {
        lexer.consume();
    }
    //    Otherwise, this is an invalid <urange>, and this algorithm must exit.
    else {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start and end values not separated by '-'.");
        return {};
    }

    // 6. Consume as many hex digits as possible from text.
    auto end_hex_digits = lexer.consume_while(is_ascii_hex_digit);

    //   If zero hex digits were consumed, or more than 6 hex digits were consumed,
    //   this is an invalid <urange>, and this algorithm must exit.
    if (end_hex_digits.length() == 0 || end_hex_digits.length() > 6) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> end value had {} digits, expected between 1 and 6.", end_hex_digits.length());
        return {};
    }

    //   If there are any code points left in text, this is an invalid <urange>, and this algorithm must exit.
    if (lexer.tell_remaining() != 0) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> invalid; had {} code points left over.", lexer.tell_remaining());
        return {};
    }

    // 7. Interpret the consumed code points as a hexadecimal number. This is the end value.
    auto maybe_end_value = AK::StringUtils::convert_to_uint_from_hex<u32>(end_hex_digits);
    if (!maybe_end_value.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> end value did not parse as hex number.");
        return {};
    }
    u32 end_value = maybe_end_value.release_value();

    return make_valid_unicode_range(start_value, end_value);
}

Vector<Gfx::UnicodeRange> Parser::parse_unicode_ranges(TokenStream<ComponentValue>& tokens)
{
    Vector<Gfx::UnicodeRange> unicode_ranges;
    auto range_token_lists = parse_a_comma_separated_list_of_component_values(tokens);
    for (auto& range_tokens : range_token_lists) {
        TokenStream range_token_stream { range_tokens };
        auto maybe_unicode_range = parse_unicode_range(range_token_stream);
        if (!maybe_unicode_range.has_value()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: unicode-range format invalid; discarding.");
            return {};
        }
        unicode_ranges.append(maybe_unicode_range.release_value());
    }
    return unicode_ranges;
}

RefPtr<CSSStyleValue> Parser::parse_dimension_value(TokenStream<ComponentValue>& tokens)
{
    if (auto dimension = parse_dimension(tokens.next_token()); dimension.has_value()) {
        tokens.discard_a_token(); // dimension

        if (dimension->is_angle())
            return AngleStyleValue::create(dimension->angle());
        if (dimension->is_frequency())
            return FrequencyStyleValue::create(dimension->frequency());
        if (dimension->is_length())
            return LengthStyleValue::create(dimension->length());
        if (dimension->is_percentage())
            return PercentageStyleValue::create(dimension->percentage());
        if (dimension->is_resolution())
            return ResolutionStyleValue::create(dimension->resolution());
        if (dimension->is_time())
            return TimeStyleValue::create(dimension->time());
        VERIFY_NOT_REACHED();
    }

    if (auto calc = parse_calculated_value(tokens.next_token()); calc && calc->resolves_to_dimension()) {
        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_integer_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number) && peek_token.token().number().is_integer()) {
        tokens.discard_a_token(); // integer
        return IntegerStyleValue::create(peek_token.token().number().integer_value());
    }
    if (auto calc = parse_calculated_value(peek_token); calc && calc->resolves_to_number()) {
        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_number_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number)) {
        tokens.discard_a_token(); // number
        return NumberStyleValue::create(peek_token.token().number().value());
    }
    if (auto calc = parse_calculated_value(peek_token); calc && calc->resolves_to_number()) {
        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_number_percentage_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<percentage> | <number>] (which is equivalent to [<alpha-value>])
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number)) {
        tokens.discard_a_token(); // number
        return NumberStyleValue::create(peek_token.token().number().value());
    }
    if (peek_token.is(Token::Type::Percentage)) {
        tokens.discard_a_token(); // percentage
        return PercentageStyleValue::create(Percentage(peek_token.token().percentage()));
    }
    if (auto calc = parse_calculated_value(peek_token); calc && calc->resolves_to_number_percentage()) {
        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_number_percentage_none_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<percentage> | <number> | none] (which is equivalent to [<alpha-value> | none])
    auto peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number)) {
        tokens.discard_a_token(); // number
        return NumberStyleValue::create(peek_token.token().number().value());
    }
    if (peek_token.is(Token::Type::Percentage)) {
        tokens.discard_a_token(); // percentage
        return PercentageStyleValue::create(Percentage(peek_token.token().percentage()));
    }
    if (auto calc = parse_calculated_value(peek_token); calc && calc->resolves_to_number_percentage()) {
        tokens.discard_a_token(); // calc
        return calc;
    }
    if (peek_token.is(Token::Type::Ident)) {
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (keyword.has_value() && keyword.value() == Keyword::None) {
            tokens.discard_a_token(); // keyword none
            return CSSKeywordValue::create(keyword.value());
        }
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Percentage)) {
        tokens.discard_a_token(); // percentage
        return PercentageStyleValue::create(Percentage(peek_token.token().percentage()));
    }
    if (auto calc = parse_calculated_value(peek_token); calc && calc->resolves_to_percentage()) {
        tokens.discard_a_token(); // calc
        return calc;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_angle_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_angle()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_angle())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_angle_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_angle() || dimension_value->is_percentage()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_angle_percentage())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_flex_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_flex()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_flex())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_frequency_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_frequency()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_frequency())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_frequency_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_frequency() || dimension_value->is_percentage()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_frequency_percentage())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_length_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_length()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_length())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_length_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_length() || dimension_value->is_percentage()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_length_percentage())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_resolution_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_resolution()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_resolution())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_time_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_time()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_time())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_time_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    if (auto dimension_value = parse_dimension_value(tokens)) {
        if (dimension_value->is_time() || dimension_value->is_percentage()
            || (dimension_value->is_math() && dimension_value->as_math().resolves_to_time_percentage())) {
            transaction.commit();
            return dimension_value;
        }
    }
    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_keyword_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Ident)) {
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (keyword.has_value()) {
            tokens.discard_a_token(); // ident
            return CSSKeywordValue::create(keyword.value());
        }
    }

    return nullptr;
}

// https://www.w3.org/TR/CSS2/visufx.html#value-def-shape
RefPtr<CSSStyleValue> Parser::parse_rect_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto const& function_token = tokens.consume_a_token();
    if (!function_token.is_function("rect"sv))
        return nullptr;

    Vector<Length, 4> params;
    auto argument_tokens = TokenStream { function_token.function().value };

    enum class CommaRequirement {
        Unknown,
        RequiresCommas,
        RequiresNoCommas
    };

    enum class Side {
        Top = 0,
        Right = 1,
        Bottom = 2,
        Left = 3
    };

    auto comma_requirement = CommaRequirement::Unknown;

    // In CSS 2.1, the only valid <shape> value is: rect(<top>, <right>, <bottom>, <left>) where
    // <top> and <bottom> specify offsets from the top border edge of the box, and <right>, and
    //  <left> specify offsets from the left border edge of the box.
    for (size_t side = 0; side < 4; side++) {
        argument_tokens.discard_whitespace();

        // <top>, <right>, <bottom>, and <left> may either have a <length> value or 'auto'.
        // Negative lengths are permitted.
        if (argument_tokens.next_token().is_ident("auto"sv)) {
            (void)argument_tokens.consume_a_token(); // `auto`
            params.append(Length::make_auto());
        } else {
            auto maybe_length = parse_length(argument_tokens);
            if (!maybe_length.has_value())
                return nullptr;
            if (maybe_length.value().is_calculated()) {
                dbgln("FIXME: Support calculated lengths in rect(): {}", maybe_length.value().calculated()->to_string());
                return nullptr;
            }
            params.append(maybe_length.value().value());
        }
        argument_tokens.discard_whitespace();

        // The last side, should be no more tokens following it.
        if (static_cast<Side>(side) == Side::Left) {
            if (argument_tokens.has_next_token())
                return nullptr;
            break;
        }

        bool next_is_comma = argument_tokens.next_token().is(Token::Type::Comma);

        // Authors should separate offset values with commas. User agents must support separation
        // with commas, but may also support separation without commas (but not a combination),
        // because a previous revision of this specification was ambiguous in this respect.
        if (comma_requirement == CommaRequirement::Unknown)
            comma_requirement = next_is_comma ? CommaRequirement::RequiresCommas : CommaRequirement::RequiresNoCommas;

        if (comma_requirement == CommaRequirement::RequiresCommas) {
            if (next_is_comma)
                argument_tokens.discard_a_token();
            else
                return nullptr;
        } else if (comma_requirement == CommaRequirement::RequiresNoCommas) {
            if (next_is_comma)
                return nullptr;
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    transaction.commit();
    return RectStyleValue::create(EdgeRect { params[0], params[1], params[2], params[3] });
}

// https://www.w3.org/TR/css-color-4/#typedef-hue
RefPtr<CSSStyleValue> Parser::parse_hue_none_value(TokenStream<ComponentValue>& tokens)
{
    // Parses [<hue> | none]
    //   <hue> = <number> | <angle>

    auto peek_token = tokens.next_token();
    if (peek_token.is(Token::Type::Number)) {
        tokens.discard_a_token(); // number
        return NumberStyleValue::create(peek_token.token().number().value());
    }
    if (auto calc = parse_calculated_value(peek_token); calc && (calc->resolves_to_number() || calc->resolves_to_angle())) {
        tokens.discard_a_token(); // calc number/angle
        return calc;
    }
    if (auto dimension = parse_dimension(peek_token); dimension.has_value() && dimension->is_angle()) {
        tokens.discard_a_token(); // angle
        return AngleStyleValue::create(dimension->angle());
    }
    if (peek_token.is(Token::Type::Ident)) {
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (keyword.has_value() && keyword.value() == Keyword::None) {
            tokens.discard_a_token(); // keyword none
            return CSSKeywordValue::create(keyword.value());
        }
    }

    return nullptr;
}

// https://www.w3.org/TR/css-color-4/#typedef-color-alpha-value
RefPtr<CSSStyleValue> Parser::parse_solidus_and_alpha_value(TokenStream<ComponentValue>& tokens)
{
    // [ / [<alpha-value> | none] ]?
    // <alpha-value> = <number> | <percentage>
    // Common to the modern-syntax color functions.

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    if (!tokens.consume_a_token().is_delim('/'))
        return {};
    tokens.discard_whitespace();
    auto alpha = parse_number_percentage_none_value(tokens);
    if (!alpha)
        return {};
    tokens.discard_whitespace();

    transaction.commit();
    return alpha;
}

// https://www.w3.org/TR/css-color-4/#funcdef-rgb
RefPtr<CSSStyleValue> Parser::parse_rgb_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // rgb() = [ <legacy-rgb-syntax> | <modern-rgb-syntax> ]
    // rgba() = [ <legacy-rgba-syntax> | <modern-rgba-syntax> ]
    // <legacy-rgb-syntax> = rgb( <percentage>#{3} , <alpha-value>? ) |
    //                       rgb( <number>#{3} , <alpha-value>? )
    // <legacy-rgba-syntax> = rgba( <percentage>#{3} , <alpha-value>? ) |
    //                        rgba( <number>#{3} , <alpha-value>? )
    // <modern-rgb-syntax> = rgb(
    //     [ <number> | <percentage> | none]{3}
    //     [ / [<alpha-value> | none] ]?  )
    // <modern-rgba-syntax> = rgba(
    //     [ <number> | <percentage> | none]{3}
    //     [ / [<alpha-value> | none] ]?  )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("rgb"sv) && !function_token.is_function("rgba"sv))
        return {};

    RefPtr<CSSStyleValue> red;
    RefPtr<CSSStyleValue> green;
    RefPtr<CSSStyleValue> blue;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    red = parse_number_percentage_none_value(inner_tokens);
    if (!red)
        return {};

    inner_tokens.discard_whitespace();
    bool legacy_syntax = inner_tokens.next_token().is(Token::Type::Comma);
    if (legacy_syntax) {
        // Legacy syntax
        //   <percentage>#{3} , <alpha-value>?
        //   | <number>#{3} , <alpha-value>?
        // So, r/g/b can be numbers or percentages, as long as they're all the same type.

        // We accepted the 'none' keyword when parsing the red value, but it's not allowed in the legacy syntax.
        if (red->is_keyword())
            return {};

        inner_tokens.discard_a_token(); // comma
        inner_tokens.discard_whitespace();

        green = parse_number_percentage_value(inner_tokens);
        if (!green)
            return {};
        inner_tokens.discard_whitespace();

        if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
            return {};
        inner_tokens.discard_whitespace();

        blue = parse_number_percentage_value(inner_tokens);
        if (!blue)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            // Try and read comma and alpha
            if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
                return {};
            inner_tokens.discard_whitespace();

            alpha = parse_number_percentage_value(inner_tokens);

            if (!alpha)
                return {};

            inner_tokens.discard_whitespace();

            if (inner_tokens.has_next_token())
                return {};
        }

        // Verify we're all percentages or all numbers
        auto is_percentage = [](CSSStyleValue const& style_value) {
            return style_value.is_percentage()
                || (style_value.is_math() && style_value.as_math().resolves_to_percentage());
        };
        bool red_is_percentage = is_percentage(*red);
        bool green_is_percentage = is_percentage(*green);
        bool blue_is_percentage = is_percentage(*blue);
        if (red_is_percentage != green_is_percentage || red_is_percentage != blue_is_percentage)
            return {};

    } else {
        // Modern syntax
        //   [ <number> | <percentage> | none]{3}  [ / [<alpha-value> | none] ]?

        green = parse_number_percentage_none_value(inner_tokens);
        if (!green)
            return {};
        inner_tokens.discard_whitespace();

        blue = parse_number_percentage_none_value(inner_tokens);
        if (!blue)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            alpha = parse_solidus_and_alpha_value(inner_tokens);
            if (!alpha || inner_tokens.has_next_token())
                return {};
        }
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSRGB::create(red.release_nonnull(), green.release_nonnull(), blue.release_nonnull(), alpha.release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-hsl
RefPtr<CSSStyleValue> Parser::parse_hsl_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // hsl() = [ <legacy-hsl-syntax> | <modern-hsl-syntax> ]
    // hsla() = [ <legacy-hsla-syntax> | <modern-hsla-syntax> ]
    // <modern-hsl-syntax> = hsl(
    //     [<hue> | none]
    //     [<percentage> | <number> | none]
    //     [<percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )
    // <modern-hsla-syntax> = hsla(
    //     [<hue> | none]
    //     [<percentage> | <number> | none]
    //     [<percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )
    // <legacy-hsl-syntax> = hsl( <hue>, <percentage>, <percentage>, <alpha-value>? )
    // <legacy-hsla-syntax> = hsla( <hue>, <percentage>, <percentage>, <alpha-value>? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("hsl"sv) && !function_token.is_function("hsla"sv))
        return {};

    RefPtr<CSSStyleValue> h;
    RefPtr<CSSStyleValue> s;
    RefPtr<CSSStyleValue> l;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    h = parse_hue_none_value(inner_tokens);
    if (!h)
        return {};

    inner_tokens.discard_whitespace();
    bool legacy_syntax = inner_tokens.next_token().is(Token::Type::Comma);
    if (legacy_syntax) {
        // Legacy syntax
        //   <hue>, <percentage>, <percentage>, <alpha-value>?

        // We accepted the 'none' keyword when parsing the h value, but it's not allowed in the legacy syntax.
        if (h->is_keyword())
            return {};

        (void)inner_tokens.consume_a_token(); // comma
        inner_tokens.discard_whitespace();

        s = parse_percentage_value(inner_tokens);
        if (!s)
            return {};
        inner_tokens.discard_whitespace();

        if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
            return {};
        inner_tokens.discard_whitespace();

        l = parse_percentage_value(inner_tokens);
        if (!l)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            // Try and read comma and alpha
            if (!inner_tokens.consume_a_token().is(Token::Type::Comma))
                return {};
            inner_tokens.discard_whitespace();

            alpha = parse_number_percentage_value(inner_tokens);
            // The parser has consumed a comma, so the alpha value is now required
            if (!alpha)
                return {};
            inner_tokens.discard_whitespace();

            if (inner_tokens.has_next_token())
                return {};
        }
    } else {
        // Modern syntax
        //   [<hue> | none]
        //   [<percentage> | <number> | none]
        //   [<percentage> | <number> | none]
        //   [ / [<alpha-value> | none] ]?

        s = parse_number_percentage_none_value(inner_tokens);
        if (!s)
            return {};
        inner_tokens.discard_whitespace();

        l = parse_number_percentage_none_value(inner_tokens);
        if (!l)
            return {};
        inner_tokens.discard_whitespace();

        if (inner_tokens.has_next_token()) {
            alpha = parse_solidus_and_alpha_value(inner_tokens);
            if (!alpha || inner_tokens.has_next_token())
                return {};
        }
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSHSL::create(h.release_nonnull(), s.release_nonnull(), l.release_nonnull(), alpha.release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-hwb
RefPtr<CSSStyleValue> Parser::parse_hwb_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // hwb() = hwb(
    //     [<hue> | none]
    //     [<percentage> | <number> | none]
    //     [<percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("hwb"sv))
        return {};

    RefPtr<CSSStyleValue> h;
    RefPtr<CSSStyleValue> w;
    RefPtr<CSSStyleValue> b;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    h = parse_hue_none_value(inner_tokens);
    if (!h)
        return {};
    inner_tokens.discard_whitespace();

    w = parse_number_percentage_none_value(inner_tokens);
    if (!w)
        return {};
    inner_tokens.discard_whitespace();

    b = parse_number_percentage_none_value(inner_tokens);
    if (!b)
        return {};
    inner_tokens.discard_whitespace();

    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSHWB::create(h.release_nonnull(), w.release_nonnull(), b.release_nonnull(), alpha.release_nonnull());
}

Optional<Array<RefPtr<CSSStyleValue>, 4>> Parser::parse_lab_like_color_value(TokenStream<ComponentValue>& outer_tokens, StringView function_name)
{
    // This helper is designed to be compatible with lab and oklab and parses a function with a form like:
    // f() = f( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function(function_name))
        return OptionalNone {};

    RefPtr<CSSStyleValue> l;
    RefPtr<CSSStyleValue> a;
    RefPtr<CSSStyleValue> b;
    RefPtr<CSSStyleValue> alpha;

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    l = parse_number_percentage_none_value(inner_tokens);
    if (!l)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    a = parse_number_percentage_none_value(inner_tokens);
    if (!a)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    b = parse_number_percentage_none_value(inner_tokens);
    if (!b)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return OptionalNone {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();

    return Array { move(l), move(a), move(b), move(alpha) };
}

// https://www.w3.org/TR/css-color-4/#funcdef-lab
RefPtr<CSSStyleValue> Parser::parse_lab_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // lab() = lab( [<percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lab_like_color_value(outer_tokens, "lab"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLabLike::create<CSSLab>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-oklab
RefPtr<CSSStyleValue> Parser::parse_oklab_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // oklab() = oklab( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lab_like_color_value(outer_tokens, "oklab"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLabLike::create<CSSOKLab>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

Optional<Array<RefPtr<CSSStyleValue>, 4>> Parser::parse_lch_like_color_value(TokenStream<ComponentValue>& outer_tokens, StringView function_name)
{
    // This helper is designed to be compatible with lch and oklch and parses a function with a form like:
    // f() = f( [<percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <hue> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto const& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function(function_name))
        return OptionalNone {};

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    auto l = parse_number_percentage_none_value(inner_tokens);
    if (!l)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    auto c = parse_number_percentage_none_value(inner_tokens);
    if (!c)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    auto h = parse_hue_none_value(inner_tokens);
    if (!h)
        return OptionalNone {};
    inner_tokens.discard_whitespace();

    RefPtr<CSSStyleValue> alpha;
    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return OptionalNone {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();

    return Array { move(l), move(c), move(h), move(alpha) };
}

// https://www.w3.org/TR/css-color-4/#funcdef-lch
RefPtr<CSSStyleValue> Parser::parse_lch_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // lch() = lch( [<percentage> | <number> | none]
    //      [ <percentage> | <number> | none]
    //      [ <hue> | none]
    //      [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lch_like_color_value(outer_tokens, "lch"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLCHLike::create<CSSLCH>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-oklch
RefPtr<CSSStyleValue> Parser::parse_oklch_color_value(TokenStream<ComponentValue>& outer_tokens)
{
    // oklch() = oklch( [ <percentage> | <number> | none]
    //     [ <percentage> | <number> | none]
    //     [ <hue> | none]
    //     [ / [<alpha-value> | none] ]? )

    auto maybe_color_values = parse_lch_like_color_value(outer_tokens, "oklch"sv);
    if (!maybe_color_values.has_value())
        return {};

    auto& color_values = *maybe_color_values;

    return CSSLCHLike::create<CSSOKLCH>(color_values[0].release_nonnull(),
        color_values[1].release_nonnull(),
        color_values[2].release_nonnull(),
        color_values[3].release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#funcdef-color
RefPtr<CSSStyleValue> Parser::parse_color_function(TokenStream<ComponentValue>& outer_tokens)
{
    // color() = color( <colorspace-params> [ / [ <alpha-value> | none ] ]? )
    //     <colorspace-params> = [ <predefined-rgb-params> | <xyz-params>]
    //     <predefined-rgb-params> = <predefined-rgb> [ <number> | <percentage> | none ]{3}
    //     <predefined-rgb> = srgb | srgb-linear | display-p3 | a98-rgb | prophoto-rgb | rec2020
    //     <xyz-params> = <xyz-space> [ <number> | <percentage> | none ]{3}
    //     <xyz-space> = xyz | xyz-d50 | xyz-d65

    auto transaction = outer_tokens.begin_transaction();
    outer_tokens.discard_whitespace();

    auto const& function_token = outer_tokens.consume_a_token();
    if (!function_token.is_function("color"sv))
        return {};

    auto inner_tokens = TokenStream { function_token.function().value };
    inner_tokens.discard_whitespace();

    auto const& maybe_color_space = inner_tokens.consume_a_token();
    inner_tokens.discard_whitespace();
    if (!any_of(CSSColor::s_supported_color_space, [&](auto supported) { return maybe_color_space.is_ident(supported); }))
        return {};

    auto const& color_space = maybe_color_space.token().ident();

    auto c1 = parse_number_percentage_value(inner_tokens);
    if (!c1)
        return {};
    inner_tokens.discard_whitespace();

    auto c2 = parse_number_percentage_value(inner_tokens);
    if (!c2)
        return {};
    inner_tokens.discard_whitespace();

    auto c3 = parse_number_percentage_value(inner_tokens);
    if (!c3)
        return {};
    inner_tokens.discard_whitespace();

    RefPtr<CSSStyleValue> alpha;
    if (inner_tokens.has_next_token()) {
        alpha = parse_solidus_and_alpha_value(inner_tokens);
        if (!alpha || inner_tokens.has_next_token())
            return {};
    }

    if (!alpha)
        alpha = NumberStyleValue::create(1);

    transaction.commit();
    return CSSColor::create(color_space.to_ascii_lowercase(),
        c1.release_nonnull(),
        c2.release_nonnull(),
        c3.release_nonnull(),
        alpha.release_nonnull());
}

// https://www.w3.org/TR/css-color-4/#color-syntax
RefPtr<CSSStyleValue> Parser::parse_color_value(TokenStream<ComponentValue>& tokens)
{

    // Keywords: <system-color> | <deprecated-color> | currentColor
    {
        auto transaction = tokens.begin_transaction();
        if (auto keyword = parse_keyword_value(tokens); keyword && keyword->has_color()) {
            transaction.commit();
            return keyword;
        }
    }

    // Functions
    if (auto color = parse_color_function(tokens))
        return color;

    if (auto rgb = parse_rgb_color_value(tokens))
        return rgb;
    if (auto hsl = parse_hsl_color_value(tokens))
        return hsl;
    if (auto hwb = parse_hwb_color_value(tokens))
        return hwb;
    if (auto lab = parse_lab_color_value(tokens))
        return lab;
    if (auto lch = parse_lch_color_value(tokens))
        return lch;
    if (auto oklab = parse_oklab_color_value(tokens))
        return oklab;
    if (auto oklch = parse_oklch_color_value(tokens))
        return oklch;

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& component_value = tokens.consume_a_token();

    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();

        auto color = Color::from_string(ident);
        if (color.has_value()) {
            transaction.commit();
            return CSSColorValue::create_from_color(color.release_value());
        }
        // Otherwise, fall through to the hashless-hex-color case
    }

    if (component_value.is(Token::Type::Hash)) {
        auto color = Color::from_string(MUST(String::formatted("#{}", component_value.token().hash_value())));
        if (color.has_value()) {
            transaction.commit();
            return CSSColorValue::create_from_color(color.release_value());
        }
        return {};
    }

    // https://quirks.spec.whatwg.org/#the-hashless-hex-color-quirk
    if (m_context.in_quirks_mode() && property_has_quirk(m_context.current_property_id(), Quirk::HashlessHexColor)) {
        // The value of a quirky color is obtained from the possible component values using the following algorithm,
        // aborting on the first step that returns a value:

        // 1. Let cv be the component value.
        auto const& cv = component_value;
        String serialization;
        // 2. If cv is a <number-token> or a <dimension-token>, follow these substeps:
        if (cv.is(Token::Type::Number) || cv.is(Token::Type::Dimension)) {
            // 1. If cv’s type flag is not "integer", return an error.
            //    This means that values that happen to use scientific notation, e.g., 5e5e5e, will fail to parse.
            if (!cv.token().number().is_integer())
                return {};

            // 2. If cv’s value is less than zero, return an error.
            auto value = cv.is(Token::Type::Number) ? cv.token().to_integer() : cv.token().dimension_value_int();
            if (value < 0)
                return {};

            // 3. Let serialization be the serialization of cv’s value, as a base-ten integer using digits 0-9 (U+0030 to U+0039) in the shortest form possible.
            StringBuilder serialization_builder;
            serialization_builder.appendff("{}", value);

            // 4. If cv is a <dimension-token>, append the unit to serialization.
            if (cv.is(Token::Type::Dimension))
                serialization_builder.append(cv.token().dimension_unit());

            // 5. If serialization consists of fewer than six characters, prepend zeros (U+0030) so that it becomes six characters.
            serialization = MUST(serialization_builder.to_string());
            if (serialization_builder.length() < 6) {
                StringBuilder builder;
                for (size_t i = 0; i < (6 - serialization_builder.length()); i++)
                    builder.append('0');
                builder.append(serialization_builder.string_view());
                serialization = MUST(builder.to_string());
            }
        }
        // 3. Otherwise, cv is an <ident-token>; let serialization be cv’s value.
        else {
            if (!cv.is(Token::Type::Ident))
                return {};
            serialization = cv.token().ident().to_string();
        }

        // 4. If serialization does not consist of three or six characters, return an error.
        if (serialization.bytes().size() != 3 && serialization.bytes().size() != 6)
            return {};

        // 5. If serialization contains any characters not in the range [0-9A-Fa-f] (U+0030 to U+0039, U+0041 to U+0046, U+0061 to U+0066), return an error.
        for (auto c : serialization.bytes_as_string_view()) {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
                return {};
        }

        // 6. Return the concatenation of "#" (U+0023) and serialization.
        auto color = Color::from_string(MUST(String::formatted("#{}", serialization)));
        if (color.has_value()) {
            transaction.commit();
            return CSSColorValue::create_from_color(color.release_value());
        }
    }

    return {};
}

// https://drafts.csswg.org/css-lists-3/#counter-functions
RefPtr<CSSStyleValue> Parser::parse_counter_value(TokenStream<ComponentValue>& tokens)
{
    auto parse_counter_name = [this](TokenStream<ComponentValue>& tokens) -> Optional<FlyString> {
        // https://drafts.csswg.org/css-lists-3/#typedef-counter-name
        // Counters are referred to in CSS syntax using the <counter-name> type, which represents
        // their name as a <custom-ident>. A <counter-name> name cannot match the keyword none;
        // such an identifier is invalid as a <counter-name>.
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto counter_name = parse_custom_ident_value(tokens, { "none"sv });
        if (!counter_name)
            return {};

        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return {};

        transaction.commit();
        return counter_name->custom_ident();
    };

    auto parse_counter_style = [this](TokenStream<ComponentValue>& tokens) -> RefPtr<CSSStyleValue> {
        // https://drafts.csswg.org/css-counter-styles-3/#typedef-counter-style
        // <counter-style> = <counter-style-name> | <symbols()>
        // For now we just support <counter-style-name>, found here:
        // https://drafts.csswg.org/css-counter-styles-3/#typedef-counter-style-name
        // <counter-style-name> is a <custom-ident> that is not an ASCII case-insensitive match for none.
        auto transaction = tokens.begin_transaction();
        tokens.discard_whitespace();

        auto counter_style_name = parse_custom_ident_value(tokens, { "none"sv });
        if (!counter_style_name)
            return {};

        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return {};

        transaction.commit();
        return counter_style_name.release_nonnull();
    };

    auto transaction = tokens.begin_transaction();
    auto const& token = tokens.consume_a_token();
    if (token.is_function("counter"sv)) {
        // counter() = counter( <counter-name>, <counter-style>? )
        auto& function = token.function();
        TokenStream function_tokens { function.value };
        auto function_values = parse_a_comma_separated_list_of_component_values(function_tokens);
        if (function_values.is_empty() || function_values.size() > 2)
            return nullptr;

        TokenStream name_tokens { function_values[0] };
        auto counter_name = parse_counter_name(name_tokens);
        if (!counter_name.has_value())
            return nullptr;

        RefPtr<CSSStyleValue> counter_style;
        if (function_values.size() > 1) {
            TokenStream counter_style_tokens { function_values[1] };
            counter_style = parse_counter_style(counter_style_tokens);
            if (!counter_style)
                return nullptr;
        } else {
            // In both cases, if the <counter-style> argument is omitted it defaults to `decimal`.
            counter_style = CustomIdentStyleValue::create("decimal"_fly_string);
        }

        transaction.commit();
        return CounterStyleValue::create_counter(counter_name.release_value(), counter_style.release_nonnull());
    }

    if (token.is_function("counters"sv)) {
        // counters() = counters( <counter-name>, <string>, <counter-style>? )
        auto& function = token.function();
        TokenStream function_tokens { function.value };
        auto function_values = parse_a_comma_separated_list_of_component_values(function_tokens);
        if (function_values.size() < 2 || function_values.size() > 3)
            return nullptr;

        TokenStream name_tokens { function_values[0] };
        auto counter_name = parse_counter_name(name_tokens);
        if (!counter_name.has_value())
            return nullptr;

        TokenStream string_tokens { function_values[1] };
        string_tokens.discard_whitespace();
        auto join_string = parse_string_value(string_tokens);
        string_tokens.discard_whitespace();
        if (!join_string || string_tokens.has_next_token())
            return nullptr;

        RefPtr<CSSStyleValue> counter_style;
        if (function_values.size() > 2) {
            TokenStream counter_style_tokens { function_values[2] };
            counter_style = parse_counter_style(counter_style_tokens);
            if (!counter_style)
                return nullptr;
        } else {
            // In both cases, if the <counter-style> argument is omitted it defaults to `decimal`.
            counter_style = CustomIdentStyleValue::create("decimal"_fly_string);
        }

        transaction.commit();
        return CounterStyleValue::create_counters(counter_name.release_value(), join_string->string_value(), counter_style.release_nonnull());
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_counter_definitions_value(TokenStream<ComponentValue>& tokens, AllowReversed allow_reversed, i32 default_value_if_not_reversed)
{
    // If AllowReversed is Yes, parses:
    //   [ <counter-name> <integer>? | <reversed-counter-name> <integer>? ]+
    // Otherwise parses:
    //   [ <counter-name> <integer>? ]+

    // FIXME: This disabled parsing of `reversed()` counters. Remove this line once they're supported.
    allow_reversed = AllowReversed::No;

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    Vector<CounterDefinition> counter_definitions;
    while (tokens.has_next_token()) {
        auto per_item_transaction = tokens.begin_transaction();
        CounterDefinition definition {};

        // <counter-name> | <reversed-counter-name>
        auto& token = tokens.consume_a_token();
        if (token.is(Token::Type::Ident)) {
            definition.name = token.token().ident();
            definition.is_reversed = false;
        } else if (allow_reversed == AllowReversed::Yes && token.is_function("reversed"sv)) {
            TokenStream function_tokens { token.function().value };
            function_tokens.discard_whitespace();
            auto& name_token = function_tokens.consume_a_token();
            if (!name_token.is(Token::Type::Ident))
                break;
            function_tokens.discard_whitespace();
            if (function_tokens.has_next_token())
                break;

            definition.name = name_token.token().ident();
            definition.is_reversed = true;
        } else {
            break;
        }
        tokens.discard_whitespace();

        // <integer>?
        definition.value = parse_integer_value(tokens);
        if (!definition.value && !definition.is_reversed)
            definition.value = IntegerStyleValue::create(default_value_if_not_reversed);

        counter_definitions.append(move(definition));
        tokens.discard_whitespace();
        per_item_transaction.commit();
    }

    if (counter_definitions.is_empty())
        return {};

    transaction.commit();
    return CounterDefinitionsStyleValue::create(move(counter_definitions));
}

RefPtr<CSSStyleValue> Parser::parse_ratio_value(TokenStream<ComponentValue>& tokens)
{
    if (auto ratio = parse_ratio(tokens); ratio.has_value())
        return RatioStyleValue::create(ratio.release_value());
    return nullptr;
}

RefPtr<StringStyleValue> Parser::parse_string_value(TokenStream<ComponentValue>& tokens)
{
    auto const& peek = tokens.next_token();
    if (peek.is(Token::Type::String)) {
        tokens.discard_a_token();
        return StringStyleValue::create(peek.token().string());
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_image_value(TokenStream<ComponentValue>& tokens)
{
    if (auto url = parse_url_function(tokens); url.has_value())
        return ImageStyleValue::create(url.value());

    if (auto linear_gradient = parse_linear_gradient_function(tokens))
        return linear_gradient;

    if (auto conic_gradient = parse_conic_gradient_function(tokens))
        return conic_gradient;

    if (auto radial_gradient = parse_radial_gradient_function(tokens))
        return radial_gradient;

    return nullptr;
}

// https://svgwg.org/svg2-draft/painting.html#SpecifyingPaint
RefPtr<CSSStyleValue> Parser::parse_paint_value(TokenStream<ComponentValue>& tokens)
{
    // `<paint> = none | <color> | <url> [none | <color>]? | context-fill | context-stroke`

    auto parse_color_or_none = [&]() -> Optional<RefPtr<CSSStyleValue>> {
        if (auto color = parse_color_value(tokens))
            return color;

        // NOTE: <color> also accepts identifiers, so we do this identifier check last.
        if (tokens.next_token().is(Token::Type::Ident)) {
            auto maybe_keyword = keyword_from_string(tokens.next_token().token().ident());
            if (maybe_keyword.has_value()) {
                // FIXME: Accept `context-fill` and `context-stroke`
                switch (*maybe_keyword) {
                case Keyword::None:
                    tokens.discard_a_token();
                    return CSSKeywordValue::create(*maybe_keyword);
                default:
                    return nullptr;
                }
            }
        }

        return OptionalNone {};
    };

    // FIMXE: Allow context-fill/context-stroke here
    if (auto color_or_none = parse_color_or_none(); color_or_none.has_value())
        return *color_or_none;

    if (auto url = parse_url_value(tokens)) {
        tokens.discard_whitespace();
        if (auto color_or_none = parse_color_or_none(); color_or_none == nullptr) {
            // Fail to parse if the fallback is invalid, but otherwise ignore it.
            // FIXME: Use fallback color
            return nullptr;
        }
        return url;
    }

    return nullptr;
}

// https://www.w3.org/TR/css-values-4/#position
RefPtr<PositionStyleValue> Parser::parse_position_value(TokenStream<ComponentValue>& tokens, PositionParsingMode position_parsing_mode)
{
    auto parse_position_edge = [](ComponentValue const& token) -> Optional<PositionEdge> {
        if (!token.is(Token::Type::Ident))
            return {};
        auto keyword = keyword_from_string(token.token().ident());
        if (!keyword.has_value())
            return {};
        return keyword_to_position_edge(*keyword);
    };

    auto parse_length_percentage = [&](ComponentValue const& token) -> Optional<LengthPercentage> {
        if (token.is(Token::Type::EndOfFile))
            return {};

        if (auto dimension = parse_dimension(token); dimension.has_value()) {
            if (dimension->is_length_percentage())
                return dimension->length_percentage();
            return {};
        }

        if (auto calc = parse_calculated_value(token); calc && calc->resolves_to_length_percentage())
            return LengthPercentage { calc.release_nonnull() };

        return {};
    };

    auto is_horizontal = [](PositionEdge edge, bool accept_center) -> bool {
        switch (edge) {
        case PositionEdge::Left:
        case PositionEdge::Right:
            return true;
        case PositionEdge::Center:
            return accept_center;
        default:
            return false;
        }
    };

    auto is_vertical = [](PositionEdge edge, bool accept_center) -> bool {
        switch (edge) {
        case PositionEdge::Top:
        case PositionEdge::Bottom:
            return true;
        case PositionEdge::Center:
            return accept_center;
        default:
            return false;
        }
    };

    auto make_edge_style_value = [](PositionEdge position_edge, bool is_horizontal) -> NonnullRefPtr<EdgeStyleValue> {
        if (position_edge == PositionEdge::Center)
            return EdgeStyleValue::create(is_horizontal ? PositionEdge::Left : PositionEdge::Top, Percentage { 50 });
        return EdgeStyleValue::create(position_edge, Length::make_px(0));
    };

    // <position> = [
    //   [ left | center | right | top | bottom | <length-percentage> ]
    // |
    //   [ left | center | right ] && [ top | center | bottom ]
    // |
    //   [ left | center | right | <length-percentage> ]
    //   [ top | center | bottom | <length-percentage> ]
    // |
    //   [ [ left | right ] <length-percentage> ] &&
    //   [ [ top | bottom ] <length-percentage> ]
    // ]

    // [ left | center | right | top | bottom | <length-percentage> ]
    auto alternative_1 = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        tokens.discard_whitespace();
        auto const& token = tokens.consume_a_token();

        // [ left | center | right | top | bottom ]
        if (auto maybe_edge = parse_position_edge(token); maybe_edge.has_value()) {
            auto edge = maybe_edge.release_value();
            transaction.commit();

            // [ left | right ]
            if (is_horizontal(edge, false))
                return PositionStyleValue::create(make_edge_style_value(edge, true), make_edge_style_value(PositionEdge::Center, false));

            // [ top | bottom ]
            if (is_vertical(edge, false))
                return PositionStyleValue::create(make_edge_style_value(PositionEdge::Center, true), make_edge_style_value(edge, false));

            // [ center ]
            VERIFY(edge == PositionEdge::Center);
            return PositionStyleValue::create(make_edge_style_value(PositionEdge::Center, true), make_edge_style_value(PositionEdge::Center, false));
        }

        // [ <length-percentage> ]
        if (auto maybe_percentage = parse_length_percentage(token); maybe_percentage.has_value()) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create(PositionEdge::Left, *maybe_percentage), make_edge_style_value(PositionEdge::Center, false));
        }

        return nullptr;
    };

    // [ left | center | right ] && [ top | center | bottom ]
    auto alternative_2 = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        tokens.discard_whitespace();

        // Parse out two position edges
        auto maybe_first_edge = parse_position_edge(tokens.consume_a_token());
        if (!maybe_first_edge.has_value())
            return nullptr;

        auto first_edge = maybe_first_edge.release_value();
        tokens.discard_whitespace();

        auto maybe_second_edge = parse_position_edge(tokens.consume_a_token());
        if (!maybe_second_edge.has_value())
            return nullptr;

        auto second_edge = maybe_second_edge.release_value();

        // If 'left' or 'right' is given, that position is X and the other is Y.
        // Conversely -
        // If 'top' or 'bottom' is given, that position is Y and the other is X.
        if (is_vertical(first_edge, false) || is_horizontal(second_edge, false))
            swap(first_edge, second_edge);

        // [ left | center | right ] [ top | bottom | center ]
        if (is_horizontal(first_edge, true) && is_vertical(second_edge, true)) {
            transaction.commit();
            return PositionStyleValue::create(make_edge_style_value(first_edge, true), make_edge_style_value(second_edge, false));
        }

        return nullptr;
    };

    // [ left | center | right | <length-percentage> ]
    // [ top | center | bottom | <length-percentage> ]
    auto alternative_3 = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        auto parse_position_or_length = [&](bool as_horizontal) -> RefPtr<EdgeStyleValue> {
            tokens.discard_whitespace();
            auto const& token = tokens.consume_a_token();

            if (auto maybe_position = parse_position_edge(token); maybe_position.has_value()) {
                auto position = maybe_position.release_value();
                bool valid = as_horizontal ? is_horizontal(position, true) : is_vertical(position, true);
                if (!valid)
                    return nullptr;
                return make_edge_style_value(position, as_horizontal);
            }

            auto maybe_length = parse_length_percentage(token);
            if (!maybe_length.has_value())
                return nullptr;

            return EdgeStyleValue::create(as_horizontal ? PositionEdge::Left : PositionEdge::Top, maybe_length.release_value());
        };

        // [ left | center | right | <length-percentage> ]
        auto horizontal_edge = parse_position_or_length(true);
        if (!horizontal_edge)
            return nullptr;

        // [ top | center | bottom | <length-percentage> ]
        auto vertical_edge = parse_position_or_length(false);
        if (!vertical_edge)
            return nullptr;

        transaction.commit();
        return PositionStyleValue::create(horizontal_edge.release_nonnull(), vertical_edge.release_nonnull());
    };

    // [ [ left | right ] <length-percentage> ] &&
    // [ [ top | bottom ] <length-percentage> ]
    auto alternative_4 = [&]() -> RefPtr<PositionStyleValue> {
        struct PositionAndLength {
            PositionEdge position;
            LengthPercentage length;
        };

        auto parse_position_and_length = [&]() -> Optional<PositionAndLength> {
            tokens.discard_whitespace();

            auto maybe_position = parse_position_edge(tokens.consume_a_token());
            if (!maybe_position.has_value())
                return {};

            tokens.discard_whitespace();

            auto maybe_length = parse_length_percentage(tokens.consume_a_token());
            if (!maybe_length.has_value())
                return {};

            return PositionAndLength {
                .position = maybe_position.release_value(),
                .length = maybe_length.release_value(),
            };
        };

        auto transaction = tokens.begin_transaction();

        auto maybe_group1 = parse_position_and_length();
        if (!maybe_group1.has_value())
            return nullptr;

        auto maybe_group2 = parse_position_and_length();
        if (!maybe_group2.has_value())
            return nullptr;

        auto group1 = maybe_group1.release_value();
        auto group2 = maybe_group2.release_value();

        // [ [ left | right ] <length-percentage> ] [ [ top | bottom ] <length-percentage> ]
        if (is_horizontal(group1.position, false) && is_vertical(group2.position, false)) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create(group1.position, group1.length), EdgeStyleValue::create(group2.position, group2.length));
        }

        // [ [ top | bottom ] <length-percentage> ] [ [ left | right ] <length-percentage> ]
        if (is_vertical(group1.position, false) && is_horizontal(group2.position, false)) {
            transaction.commit();
            return PositionStyleValue::create(EdgeStyleValue::create(group2.position, group2.length), EdgeStyleValue::create(group1.position, group1.length));
        }

        return nullptr;
    };

    // The extra 3-value syntax that's allowed for background-position:
    // [ center | [ left | right ] <length-percentage>? ] &&
    // [ center | [ top | bottom ] <length-percentage>? ]
    auto alternative_5_for_background_position = [&]() -> RefPtr<PositionStyleValue> {
        auto transaction = tokens.begin_transaction();

        struct PositionAndMaybeLength {
            PositionEdge position;
            Optional<LengthPercentage> length;
        };

        // [ <position> <length-percentage>? ]
        auto parse_position_and_maybe_length = [&]() -> Optional<PositionAndMaybeLength> {
            tokens.discard_whitespace();

            auto maybe_position = parse_position_edge(tokens.consume_a_token());
            if (!maybe_position.has_value())
                return {};

            tokens.discard_whitespace();

            auto maybe_length = parse_length_percentage(tokens.next_token());
            if (maybe_length.has_value()) {
                // 'center' cannot be followed by a <length-percentage>
                if (maybe_position.value() == PositionEdge::Center && maybe_length.has_value())
                    return {};
                tokens.discard_a_token();
            }

            return PositionAndMaybeLength {
                .position = maybe_position.release_value(),
                .length = move(maybe_length),
            };
        };

        auto maybe_group1 = parse_position_and_maybe_length();
        if (!maybe_group1.has_value())
            return nullptr;

        auto maybe_group2 = parse_position_and_maybe_length();
        if (!maybe_group2.has_value())
            return nullptr;

        auto group1 = maybe_group1.release_value();
        auto group2 = maybe_group2.release_value();

        // 2-value or 4-value if both <length-percentage>s are present or missing.
        if (group1.length.has_value() == group2.length.has_value())
            return nullptr;

        // If 'left' or 'right' is given, that position is X and the other is Y.
        // Conversely -
        // If 'top' or 'bottom' is given, that position is Y and the other is X.
        if (is_vertical(group1.position, false) || is_horizontal(group2.position, false))
            swap(group1, group2);

        // [ center | [ left | right ] ]
        if (!is_horizontal(group1.position, true))
            return nullptr;

        // [ center | [ top | bottom ] ]
        if (!is_vertical(group2.position, true))
            return nullptr;

        auto to_style_value = [&](PositionAndMaybeLength const& group, bool is_horizontal) -> NonnullRefPtr<EdgeStyleValue> {
            if (group.position == PositionEdge::Center)
                return EdgeStyleValue::create(is_horizontal ? PositionEdge::Left : PositionEdge::Top, Percentage { 50 });

            return EdgeStyleValue::create(group.position, group.length.value_or(Length::make_px(0)));
        };

        transaction.commit();
        return PositionStyleValue::create(to_style_value(group1, true), to_style_value(group2, false));
    };

    // Note: The alternatives must be attempted in this order since shorter alternatives can match a prefix of longer ones.
    if (auto position = alternative_4())
        return position;
    if (position_parsing_mode == PositionParsingMode::BackgroundPosition) {
        if (auto position = alternative_5_for_background_position())
            return position;
    }
    if (auto position = alternative_3())
        return position;
    if (auto position = alternative_2())
        return position;
    if (auto position = alternative_1())
        return position;
    return nullptr;
}

template<typename ParseFunction>
RefPtr<CSSStyleValue> Parser::parse_comma_separated_value_list(TokenStream<ComponentValue>& tokens, ParseFunction parse_one_value)
{
    auto first = parse_one_value(tokens);
    if (!first || !tokens.has_next_token())
        return first;

    StyleValueVector values;
    values.append(first.release_nonnull());

    while (tokens.has_next_token()) {
        if (!tokens.consume_a_token().is(Token::Type::Comma))
            return nullptr;

        if (auto maybe_value = parse_one_value(tokens)) {
            values.append(maybe_value.release_nonnull());
            continue;
        }
        return nullptr;
    }

    return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
}

RefPtr<CSSStyleValue> Parser::parse_simple_comma_separated_value_list(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    return parse_comma_separated_value_list(tokens, [this, property_id](auto& tokens) -> RefPtr<CSSStyleValue> {
        if (auto value = parse_css_value_for_property(property_id, tokens))
            return value;
        tokens.reconsume_current_input_token();
        return nullptr;
    });
}

RefPtr<CSSStyleValue> Parser::parse_all_as_single_keyword_value(TokenStream<ComponentValue>& tokens, Keyword keyword)
{
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto keyword_value = parse_keyword_value(tokens);
    tokens.discard_whitespace();

    if (tokens.has_next_token() || !keyword_value || keyword_value->to_keyword() != keyword)
        return {};

    transaction.commit();
    return keyword_value;
}

static void remove_property(Vector<PropertyID>& properties, PropertyID property_to_remove)
{
    properties.remove_first_matching([&](auto it) { return it == property_to_remove; });
}

// https://www.w3.org/TR/css-sizing-4/#aspect-ratio
RefPtr<CSSStyleValue> Parser::parse_aspect_ratio_value(TokenStream<ComponentValue>& tokens)
{
    // `auto || <ratio>`
    RefPtr<CSSStyleValue> auto_value;
    RefPtr<CSSStyleValue> ratio_value;

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto maybe_value = parse_css_value_for_property(PropertyID::AspectRatio, tokens);
        if (!maybe_value)
            return nullptr;

        if (maybe_value->is_ratio()) {
            if (ratio_value)
                return nullptr;
            ratio_value = maybe_value.release_nonnull();
            continue;
        }

        if (maybe_value->is_keyword() && maybe_value->as_keyword().keyword() == Keyword::Auto) {
            if (auto_value)
                return nullptr;
            auto_value = maybe_value.release_nonnull();
            continue;
        }

        return nullptr;
    }

    if (auto_value && ratio_value) {
        transaction.commit();
        return StyleValueList::create(
            StyleValueVector { auto_value.release_nonnull(), ratio_value.release_nonnull() },
            StyleValueList::Separator::Space);
    }

    if (ratio_value) {
        transaction.commit();
        return ratio_value.release_nonnull();
    }

    if (auto_value) {
        transaction.commit();
        return auto_value.release_nonnull();
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_background_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto make_background_shorthand = [&](auto background_color, auto background_image, auto background_position, auto background_size, auto background_repeat, auto background_attachment, auto background_origin, auto background_clip) {
        return ShorthandStyleValue::create(PropertyID::Background,
            { PropertyID::BackgroundColor, PropertyID::BackgroundImage, PropertyID::BackgroundPosition, PropertyID::BackgroundSize, PropertyID::BackgroundRepeat, PropertyID::BackgroundAttachment, PropertyID::BackgroundOrigin, PropertyID::BackgroundClip },
            { move(background_color), move(background_image), move(background_position), move(background_size), move(background_repeat), move(background_attachment), move(background_origin), move(background_clip) });
    };

    StyleValueVector background_images;
    StyleValueVector background_positions;
    StyleValueVector background_sizes;
    StyleValueVector background_repeats;
    StyleValueVector background_attachments;
    StyleValueVector background_clips;
    StyleValueVector background_origins;
    RefPtr<CSSStyleValue> background_color;

    auto initial_background_image = property_initial_value(m_context.realm(), PropertyID::BackgroundImage);
    auto initial_background_position = property_initial_value(m_context.realm(), PropertyID::BackgroundPosition);
    auto initial_background_size = property_initial_value(m_context.realm(), PropertyID::BackgroundSize);
    auto initial_background_repeat = property_initial_value(m_context.realm(), PropertyID::BackgroundRepeat);
    auto initial_background_attachment = property_initial_value(m_context.realm(), PropertyID::BackgroundAttachment);
    auto initial_background_clip = property_initial_value(m_context.realm(), PropertyID::BackgroundClip);
    auto initial_background_origin = property_initial_value(m_context.realm(), PropertyID::BackgroundOrigin);
    auto initial_background_color = property_initial_value(m_context.realm(), PropertyID::BackgroundColor);

    // Per-layer values
    RefPtr<CSSStyleValue> background_image;
    RefPtr<CSSStyleValue> background_position;
    RefPtr<CSSStyleValue> background_size;
    RefPtr<CSSStyleValue> background_repeat;
    RefPtr<CSSStyleValue> background_attachment;
    RefPtr<CSSStyleValue> background_clip;
    RefPtr<CSSStyleValue> background_origin;

    bool has_multiple_layers = false;
    // BackgroundSize is always parsed as part of BackgroundPosition, so we don't include it here.
    Vector<PropertyID> remaining_layer_properties {
        PropertyID::BackgroundAttachment,
        PropertyID::BackgroundClip,
        PropertyID::BackgroundColor,
        PropertyID::BackgroundImage,
        PropertyID::BackgroundOrigin,
        PropertyID::BackgroundPosition,
        PropertyID::BackgroundRepeat,
    };

    auto background_layer_is_valid = [&](bool allow_background_color) -> bool {
        if (allow_background_color) {
            if (background_color)
                return true;
        } else {
            if (background_color)
                return false;
        }
        return background_image || background_position || background_size || background_repeat || background_attachment || background_clip || background_origin;
    };

    auto complete_background_layer = [&]() {
        background_images.append(background_image ? background_image.release_nonnull() : initial_background_image);
        background_positions.append(background_position ? background_position.release_nonnull() : initial_background_position);
        background_sizes.append(background_size ? background_size.release_nonnull() : initial_background_size);
        background_repeats.append(background_repeat ? background_repeat.release_nonnull() : initial_background_repeat);
        background_attachments.append(background_attachment ? background_attachment.release_nonnull() : initial_background_attachment);

        if (!background_origin && !background_clip) {
            background_origin = initial_background_origin;
            background_clip = initial_background_clip;
        } else if (!background_clip) {
            background_clip = background_origin;
        }
        background_origins.append(background_origin.release_nonnull());
        background_clips.append(background_clip.release_nonnull());

        background_image = nullptr;
        background_position = nullptr;
        background_size = nullptr;
        background_repeat = nullptr;
        background_attachment = nullptr;
        background_clip = nullptr;
        background_origin = nullptr;

        remaining_layer_properties.clear_with_capacity();
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundAttachment);
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundClip);
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundColor);
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundImage);
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundOrigin);
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundPosition);
        remaining_layer_properties.unchecked_append(PropertyID::BackgroundRepeat);
    };

    while (tokens.has_next_token()) {
        if (tokens.next_token().is(Token::Type::Comma)) {
            has_multiple_layers = true;
            if (!background_layer_is_valid(false))
                return nullptr;
            complete_background_layer();
            tokens.discard_a_token();
            continue;
        }

        auto value_and_property = parse_css_value_for_properties(remaining_layer_properties, tokens);
        if (!value_and_property.has_value())
            return nullptr;
        auto& value = value_and_property->style_value;
        remove_property(remaining_layer_properties, value_and_property->property);

        switch (value_and_property->property) {
        case PropertyID::BackgroundAttachment:
            VERIFY(!background_attachment);
            background_attachment = value.release_nonnull();
            continue;
        case PropertyID::BackgroundColor:
            VERIFY(!background_color);
            background_color = value.release_nonnull();
            continue;
        case PropertyID::BackgroundImage:
            VERIFY(!background_image);
            background_image = value.release_nonnull();
            continue;
        case PropertyID::BackgroundClip:
        case PropertyID::BackgroundOrigin: {
            // background-origin and background-clip accept the same values. From the spec:
            //   "If one <box> value is present then it sets both background-origin and background-clip to that value.
            //    If two values are present, then the first sets background-origin and the second background-clip."
            //        - https://www.w3.org/TR/css-backgrounds-3/#background
            // So, we put the first one in background-origin, then if we get a second, we put it in background-clip.
            // If we only get one, we copy the value before creating the ShorthandStyleValue.
            if (!background_origin) {
                background_origin = value.release_nonnull();
            } else if (!background_clip) {
                background_clip = value.release_nonnull();
            } else {
                VERIFY_NOT_REACHED();
            }
            continue;
        }
        case PropertyID::BackgroundPosition: {
            VERIFY(!background_position);
            background_position = value.release_nonnull();

            // Attempt to parse `/ <background-size>`
            auto background_size_transaction = tokens.begin_transaction();
            auto& maybe_slash = tokens.consume_a_token();
            if (maybe_slash.is_delim('/')) {
                if (auto maybe_background_size = parse_single_background_size_value(tokens)) {
                    background_size_transaction.commit();
                    background_size = maybe_background_size.release_nonnull();
                    continue;
                }
                return nullptr;
            }
            continue;
        }
        case PropertyID::BackgroundRepeat: {
            VERIFY(!background_repeat);
            tokens.reconsume_current_input_token();
            if (auto maybe_repeat = parse_single_background_repeat_value(tokens)) {
                background_repeat = maybe_repeat.release_nonnull();
                continue;
            }
            return nullptr;
        }
        default:
            VERIFY_NOT_REACHED();
        }

        return nullptr;
    }

    if (!background_layer_is_valid(true))
        return nullptr;

    // We only need to create StyleValueLists if there are multiple layers.
    // Otherwise, we can pass the single StyleValues directly.
    if (has_multiple_layers) {
        complete_background_layer();

        if (!background_color)
            background_color = initial_background_color;
        transaction.commit();
        return make_background_shorthand(
            background_color.release_nonnull(),
            StyleValueList::create(move(background_images), StyleValueList::Separator::Comma),
            StyleValueList::create(move(background_positions), StyleValueList::Separator::Comma),
            StyleValueList::create(move(background_sizes), StyleValueList::Separator::Comma),
            StyleValueList::create(move(background_repeats), StyleValueList::Separator::Comma),
            StyleValueList::create(move(background_attachments), StyleValueList::Separator::Comma),
            StyleValueList::create(move(background_origins), StyleValueList::Separator::Comma),
            StyleValueList::create(move(background_clips), StyleValueList::Separator::Comma));
    }

    if (!background_color)
        background_color = initial_background_color;
    if (!background_image)
        background_image = initial_background_image;
    if (!background_position)
        background_position = initial_background_position;
    if (!background_size)
        background_size = initial_background_size;
    if (!background_repeat)
        background_repeat = initial_background_repeat;
    if (!background_attachment)
        background_attachment = initial_background_attachment;

    if (!background_origin && !background_clip) {
        background_origin = initial_background_origin;
        background_clip = initial_background_clip;
    } else if (!background_clip) {
        background_clip = background_origin;
    }

    transaction.commit();
    return make_background_shorthand(
        background_color.release_nonnull(),
        background_image.release_nonnull(),
        background_position.release_nonnull(),
        background_size.release_nonnull(),
        background_repeat.release_nonnull(),
        background_attachment.release_nonnull(),
        background_origin.release_nonnull(),
        background_clip.release_nonnull());
}

static Optional<LengthPercentage> style_value_to_length_percentage(auto value)
{
    if (value->is_percentage())
        return LengthPercentage { value->as_percentage().percentage() };
    if (value->is_length())
        return LengthPercentage { value->as_length().length() };
    if (value->is_math())
        return LengthPercentage { value->as_math() };
    return {};
}

RefPtr<CSSStyleValue> Parser::parse_single_background_position_x_or_y_value(TokenStream<ComponentValue>& tokens, PropertyID property)
{
    PositionEdge relative_edge {};
    if (property == PropertyID::BackgroundPositionX) {
        // [ center | [ [ left | right | x-start | x-end ]? <length-percentage>? ]! ]#
        relative_edge = PositionEdge::Left;
    } else if (property == PropertyID::BackgroundPositionY) {
        // [ center | [ [ top | bottom | y-start | y-end ]? <length-percentage>? ]! ]#
        relative_edge = PositionEdge::Top;
    } else {
        VERIFY_NOT_REACHED();
    }

    auto transaction = tokens.begin_transaction();
    if (!tokens.has_next_token())
        return nullptr;

    auto value = parse_css_value_for_property(property, tokens);
    if (!value)
        return nullptr;

    if (value->is_keyword()) {
        auto keyword = value->to_keyword();
        if (keyword == Keyword::Center) {
            transaction.commit();
            return EdgeStyleValue::create(relative_edge, Percentage { 50 });
        }
        if (auto edge = keyword_to_position_edge(keyword); edge.has_value()) {
            relative_edge = *edge;
        } else {
            return nullptr;
        }
        if (tokens.has_next_token()) {
            value = parse_css_value_for_property(property, tokens);
            if (!value) {
                transaction.commit();
                return EdgeStyleValue::create(relative_edge, Length::make_px(0));
            }
        }
    }

    auto offset = style_value_to_length_percentage(value);
    if (offset.has_value()) {
        transaction.commit();
        return EdgeStyleValue::create(relative_edge, *offset);
    }

    // If no offset is provided create this element but with an offset of default value of zero
    transaction.commit();
    return EdgeStyleValue::create(relative_edge, Length::make_px(0));
}

RefPtr<CSSStyleValue> Parser::parse_single_background_repeat_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto is_directional_repeat = [](CSSStyleValue const& value) -> bool {
        auto keyword = value.to_keyword();
        return keyword == Keyword::RepeatX || keyword == Keyword::RepeatY;
    };

    auto as_repeat = [](Keyword keyword) -> Optional<Repeat> {
        switch (keyword) {
        case Keyword::NoRepeat:
            return Repeat::NoRepeat;
        case Keyword::Repeat:
            return Repeat::Repeat;
        case Keyword::Round:
            return Repeat::Round;
        case Keyword::Space:
            return Repeat::Space;
        default:
            return {};
        }
    };

    auto maybe_x_value = parse_css_value_for_property(PropertyID::BackgroundRepeat, tokens);
    if (!maybe_x_value)
        return nullptr;
    auto x_value = maybe_x_value.release_nonnull();

    if (is_directional_repeat(*x_value)) {
        auto keyword = x_value->to_keyword();
        transaction.commit();
        return BackgroundRepeatStyleValue::create(
            keyword == Keyword::RepeatX ? Repeat::Repeat : Repeat::NoRepeat,
            keyword == Keyword::RepeatX ? Repeat::NoRepeat : Repeat::Repeat);
    }

    auto x_repeat = as_repeat(x_value->to_keyword());
    if (!x_repeat.has_value())
        return nullptr;

    // See if we have a second value for Y
    auto maybe_y_value = parse_css_value_for_property(PropertyID::BackgroundRepeat, tokens);
    if (!maybe_y_value) {
        // We don't have a second value, so use x for both
        transaction.commit();
        return BackgroundRepeatStyleValue::create(x_repeat.value(), x_repeat.value());
    }
    auto y_value = maybe_y_value.release_nonnull();
    if (is_directional_repeat(*y_value))
        return nullptr;

    auto y_repeat = as_repeat(y_value->to_keyword());
    if (!y_repeat.has_value())
        return nullptr;

    transaction.commit();
    return BackgroundRepeatStyleValue::create(x_repeat.value(), y_repeat.value());
}

RefPtr<CSSStyleValue> Parser::parse_single_background_size_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto get_length_percentage = [](CSSStyleValue& style_value) -> Optional<LengthPercentage> {
        if (style_value.has_auto())
            return LengthPercentage { Length::make_auto() };
        if (style_value.is_percentage())
            return LengthPercentage { style_value.as_percentage().percentage() };
        if (style_value.is_length())
            return LengthPercentage { style_value.as_length().length() };
        if (style_value.is_math())
            return LengthPercentage { style_value.as_math() };
        return {};
    };

    auto maybe_x_value = parse_css_value_for_property(PropertyID::BackgroundSize, tokens);
    if (!maybe_x_value)
        return nullptr;
    auto x_value = maybe_x_value.release_nonnull();

    if (x_value->to_keyword() == Keyword::Cover || x_value->to_keyword() == Keyword::Contain) {
        transaction.commit();
        return x_value;
    }

    auto maybe_y_value = parse_css_value_for_property(PropertyID::BackgroundSize, tokens);
    if (!maybe_y_value) {
        auto y_value = LengthPercentage { Length::make_auto() };
        auto x_size = get_length_percentage(*x_value);
        if (!x_size.has_value())
            return nullptr;

        transaction.commit();
        return BackgroundSizeStyleValue::create(x_size.value(), y_value);
    }

    auto y_value = maybe_y_value.release_nonnull();
    auto x_size = get_length_percentage(*x_value);
    auto y_size = get_length_percentage(*y_value);

    if (!x_size.has_value() || !y_size.has_value())
        return nullptr;

    transaction.commit();
    return BackgroundSizeStyleValue::create(x_size.release_value(), y_size.release_value());
}

RefPtr<CSSStyleValue> Parser::parse_border_value(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    RefPtr<CSSStyleValue> border_width;
    RefPtr<CSSStyleValue> border_color;
    RefPtr<CSSStyleValue> border_style;

    auto color_property = PropertyID::Invalid;
    auto style_property = PropertyID::Invalid;
    auto width_property = PropertyID::Invalid;

    switch (property_id) {
    case PropertyID::Border:
        color_property = PropertyID::BorderColor;
        style_property = PropertyID::BorderStyle;
        width_property = PropertyID::BorderWidth;
        break;
    case PropertyID::BorderBottom:
        color_property = PropertyID::BorderBottomColor;
        style_property = PropertyID::BorderBottomStyle;
        width_property = PropertyID::BorderBottomWidth;
        break;
    case PropertyID::BorderLeft:
        color_property = PropertyID::BorderLeftColor;
        style_property = PropertyID::BorderLeftStyle;
        width_property = PropertyID::BorderLeftWidth;
        break;
    case PropertyID::BorderRight:
        color_property = PropertyID::BorderRightColor;
        style_property = PropertyID::BorderRightStyle;
        width_property = PropertyID::BorderRightWidth;
        break;
    case PropertyID::BorderTop:
        color_property = PropertyID::BorderTopColor;
        style_property = PropertyID::BorderTopStyle;
        width_property = PropertyID::BorderTopWidth;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto remaining_longhands = Vector { width_property, color_property, style_property };
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;
        remove_property(remaining_longhands, property_and_value->property);

        if (property_and_value->property == width_property) {
            VERIFY(!border_width);
            border_width = value.release_nonnull();
        } else if (property_and_value->property == color_property) {
            VERIFY(!border_color);
            border_color = value.release_nonnull();
        } else if (property_and_value->property == style_property) {
            VERIFY(!border_style);
            border_style = value.release_nonnull();
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    if (!border_width)
        border_width = property_initial_value(m_context.realm(), width_property);
    if (!border_style)
        border_style = property_initial_value(m_context.realm(), style_property);
    if (!border_color)
        border_color = property_initial_value(m_context.realm(), color_property);

    transaction.commit();
    return ShorthandStyleValue::create(property_id,
        { width_property, style_property, color_property },
        { border_width.release_nonnull(), border_style.release_nonnull(), border_color.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_border_radius_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.remaining_token_count() == 2) {
        auto transaction = tokens.begin_transaction();
        auto horizontal = parse_length_percentage(tokens);
        auto vertical = parse_length_percentage(tokens);
        if (horizontal.has_value() && vertical.has_value()) {
            transaction.commit();
            return BorderRadiusStyleValue::create(horizontal.release_value(), vertical.release_value());
        }
    }

    if (tokens.remaining_token_count() == 1) {
        auto transaction = tokens.begin_transaction();
        auto radius = parse_length_percentage(tokens);
        if (radius.has_value()) {
            transaction.commit();
            return BorderRadiusStyleValue::create(radius.value(), radius.value());
        }
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_border_radius_shorthand_value(TokenStream<ComponentValue>& tokens)
{
    auto top_left = [&](Vector<LengthPercentage>& radii) { return radii[0]; };
    auto top_right = [&](Vector<LengthPercentage>& radii) {
        switch (radii.size()) {
        case 4:
        case 3:
        case 2:
            return radii[1];
        case 1:
            return radii[0];
        default:
            VERIFY_NOT_REACHED();
        }
    };
    auto bottom_right = [&](Vector<LengthPercentage>& radii) {
        switch (radii.size()) {
        case 4:
        case 3:
            return radii[2];
        case 2:
        case 1:
            return radii[0];
        default:
            VERIFY_NOT_REACHED();
        }
    };
    auto bottom_left = [&](Vector<LengthPercentage>& radii) {
        switch (radii.size()) {
        case 4:
            return radii[3];
        case 3:
        case 2:
            return radii[1];
        case 1:
            return radii[0];
        default:
            VERIFY_NOT_REACHED();
        }
    };

    Vector<LengthPercentage> horizontal_radii;
    Vector<LengthPercentage> vertical_radii;
    bool reading_vertical = false;
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        if (tokens.next_token().is_delim('/')) {
            if (reading_vertical || horizontal_radii.is_empty())
                return nullptr;

            reading_vertical = true;
            tokens.discard_a_token(); // `/`
            continue;
        }

        auto maybe_dimension = parse_length_percentage(tokens);
        if (!maybe_dimension.has_value())
            return nullptr;
        if (reading_vertical) {
            vertical_radii.append(maybe_dimension.release_value());
        } else {
            horizontal_radii.append(maybe_dimension.release_value());
        }
    }

    if (horizontal_radii.size() > 4 || vertical_radii.size() > 4
        || horizontal_radii.is_empty()
        || (reading_vertical && vertical_radii.is_empty()))
        return nullptr;

    auto top_left_radius = BorderRadiusStyleValue::create(top_left(horizontal_radii),
        vertical_radii.is_empty() ? top_left(horizontal_radii) : top_left(vertical_radii));
    auto top_right_radius = BorderRadiusStyleValue::create(top_right(horizontal_radii),
        vertical_radii.is_empty() ? top_right(horizontal_radii) : top_right(vertical_radii));
    auto bottom_right_radius = BorderRadiusStyleValue::create(bottom_right(horizontal_radii),
        vertical_radii.is_empty() ? bottom_right(horizontal_radii) : bottom_right(vertical_radii));
    auto bottom_left_radius = BorderRadiusStyleValue::create(bottom_left(horizontal_radii),
        vertical_radii.is_empty() ? bottom_left(horizontal_radii) : bottom_left(vertical_radii));

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::BorderRadius,
        { PropertyID::BorderTopLeftRadius, PropertyID::BorderTopRightRadius, PropertyID::BorderBottomRightRadius, PropertyID::BorderBottomLeftRadius },
        { move(top_left_radius), move(top_right_radius), move(bottom_right_radius), move(bottom_left_radius) });
}

RefPtr<CSSStyleValue> Parser::parse_columns_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.remaining_token_count() > 2)
        return nullptr;

    RefPtr<CSSStyleValue> column_count;
    RefPtr<CSSStyleValue> column_width;

    Vector<PropertyID> remaining_longhands { PropertyID::ColumnCount, PropertyID::ColumnWidth };
    int found_autos = 0;

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;

        // since the values can be in either order, we want to skip over autos
        if (value->has_auto()) {
            found_autos++;
            continue;
        }

        remove_property(remaining_longhands, property_and_value->property);

        switch (property_and_value->property) {
        case PropertyID::ColumnCount: {
            VERIFY(!column_count);
            column_count = value.release_nonnull();
            continue;
        }
        case PropertyID::ColumnWidth: {
            VERIFY(!column_width);
            column_width = value.release_nonnull();
            continue;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (found_autos > 2)
        return nullptr;

    if (found_autos == 2) {
        column_count = CSSKeywordValue::create(Keyword::Auto);
        column_width = CSSKeywordValue::create(Keyword::Auto);
    }

    if (found_autos == 1) {
        if (!column_count)
            column_count = CSSKeywordValue::create(Keyword::Auto);
        if (!column_width)
            column_width = CSSKeywordValue::create(Keyword::Auto);
    }

    if (!column_count)
        column_count = property_initial_value(m_context.realm(), PropertyID::ColumnCount);
    if (!column_width)
        column_width = property_initial_value(m_context.realm(), PropertyID::ColumnWidth);

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::Columns,
        { PropertyID::ColumnCount, PropertyID::ColumnWidth },
        { column_count.release_nonnull(), column_width.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_shadow_value(TokenStream<ComponentValue>& tokens, AllowInsetKeyword allow_inset_keyword)
{
    // "none"
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    return parse_comma_separated_value_list(tokens, [this, allow_inset_keyword](auto& tokens) {
        return parse_single_shadow_value(tokens, allow_inset_keyword);
    });
}

RefPtr<CSSStyleValue> Parser::parse_single_shadow_value(TokenStream<ComponentValue>& tokens, AllowInsetKeyword allow_inset_keyword)
{
    auto transaction = tokens.begin_transaction();

    RefPtr<CSSStyleValue> color;
    RefPtr<CSSStyleValue> offset_x;
    RefPtr<CSSStyleValue> offset_y;
    RefPtr<CSSStyleValue> blur_radius;
    RefPtr<CSSStyleValue> spread_distance;
    Optional<ShadowPlacement> placement;

    auto possibly_dynamic_length = [&](ComponentValue const& token) -> RefPtr<CSSStyleValue> {
        auto tokens = TokenStream<ComponentValue>::of_single_token(token);
        auto maybe_length = parse_length(tokens);
        if (!maybe_length.has_value())
            return nullptr;
        return maybe_length->as_style_value();
    };

    while (tokens.has_next_token()) {
        if (auto maybe_color = parse_color_value(tokens); maybe_color) {
            if (color)
                return nullptr;
            color = maybe_color.release_nonnull();
            continue;
        }

        auto const& token = tokens.next_token();
        if (auto maybe_offset_x = possibly_dynamic_length(token); maybe_offset_x) {
            // horizontal offset
            if (offset_x)
                return nullptr;
            offset_x = maybe_offset_x;
            tokens.discard_a_token();

            // vertical offset
            if (!tokens.has_next_token())
                return nullptr;
            auto maybe_offset_y = possibly_dynamic_length(tokens.next_token());
            if (!maybe_offset_y)
                return nullptr;
            offset_y = maybe_offset_y;
            tokens.discard_a_token();

            // blur radius (optional)
            if (!tokens.has_next_token())
                break;
            auto maybe_blur_radius = possibly_dynamic_length(tokens.next_token());
            if (!maybe_blur_radius)
                continue;
            blur_radius = maybe_blur_radius;
            tokens.discard_a_token();

            // spread distance (optional)
            if (!tokens.has_next_token())
                break;
            auto maybe_spread_distance = possibly_dynamic_length(tokens.next_token());
            if (!maybe_spread_distance)
                continue;
            spread_distance = maybe_spread_distance;
            tokens.discard_a_token();

            continue;
        }

        if (allow_inset_keyword == AllowInsetKeyword::Yes && token.is_ident("inset"sv)) {
            if (placement.has_value())
                return nullptr;
            placement = ShadowPlacement::Inner;
            tokens.discard_a_token();
            continue;
        }

        if (token.is(Token::Type::Comma))
            break;

        return nullptr;
    }

    // If color is absent, default to `currentColor`
    if (!color)
        color = CSSKeywordValue::create(Keyword::Currentcolor);

    // x/y offsets are required
    if (!offset_x || !offset_y)
        return nullptr;

    // Other lengths default to 0
    if (!blur_radius)
        blur_radius = LengthStyleValue::create(Length::make_px(0));
    if (!spread_distance)
        spread_distance = LengthStyleValue::create(Length::make_px(0));

    // Placement is outer by default
    if (!placement.has_value())
        placement = ShadowPlacement::Outer;

    transaction.commit();
    return ShadowStyleValue::create(color.release_nonnull(), offset_x.release_nonnull(), offset_y.release_nonnull(), blur_radius.release_nonnull(), spread_distance.release_nonnull(), placement.release_value());
}

RefPtr<CSSStyleValue> Parser::parse_rotate_value(TokenStream<ComponentValue>& tokens)
{
    // Value:	none | <angle> | [ x | y | z | <number>{3} ] && <angle>

    if (tokens.remaining_token_count() == 1) {
        // "none"
        if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
            return none;

        // <angle>
        if (auto angle = parse_angle_value(tokens))
            return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(0), NumberStyleValue::create(0), NumberStyleValue::create(1));
    }

    auto parse_one_of_xyz = [&]() -> Optional<ComponentValue const&> {
        auto transaction = tokens.begin_transaction();
        auto const& axis = tokens.consume_a_token();

        if (axis.is_ident("x"sv) || axis.is_ident("y"sv) || axis.is_ident("z"sv)) {
            transaction.commit();
            return axis;
        }

        return {};
    };

    // [ x | y | z ] && <angle>
    if (tokens.remaining_token_count() == 2) {
        // Try parsing `x <angle>`
        if (auto axis = parse_one_of_xyz(); axis.has_value()) {
            if (auto angle = parse_angle_value(tokens); angle) {
                if (axis->is_ident("x"sv))
                    return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(1), NumberStyleValue::create(0), NumberStyleValue::create(0));
                if (axis->is_ident("y"sv))
                    return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(0), NumberStyleValue::create(1), NumberStyleValue::create(0));
                if (axis->is_ident("z"sv))
                    return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(0), NumberStyleValue::create(0), NumberStyleValue::create(1));
            }
        }

        // Try parsing `<angle> x`
        if (auto angle = parse_angle_value(tokens); angle) {
            if (auto axis = parse_one_of_xyz(); axis.has_value()) {
                if (axis->is_ident("x"sv))
                    return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(1), NumberStyleValue::create(0), NumberStyleValue::create(0));
                if (axis->is_ident("y"sv))
                    return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(0), NumberStyleValue::create(1), NumberStyleValue::create(0));
                if (axis->is_ident("z"sv))
                    return RotationStyleValue::create(angle.release_nonnull(), NumberStyleValue::create(0), NumberStyleValue::create(0), NumberStyleValue::create(1));
            }
        }
    }

    auto parse_three_numbers = [&]() -> Optional<StyleValueVector> {
        auto transaction = tokens.begin_transaction();
        StyleValueVector numbers;
        for (size_t i = 0; i < 3; ++i) {
            if (auto number = parse_number_value(tokens); number) {
                numbers.append(number.release_nonnull());
            } else {
                return {};
            }
        }
        transaction.commit();
        return numbers;
    };

    // <number>{3} && <angle>
    if (tokens.remaining_token_count() == 4) {
        // Try parsing <number>{3} <angle>
        if (auto maybe_numbers = parse_three_numbers(); maybe_numbers.has_value()) {
            if (auto angle = parse_angle_value(tokens); angle) {
                auto numbers = maybe_numbers.release_value();
                return RotationStyleValue::create(angle.release_nonnull(), numbers[0], numbers[1], numbers[2]);
            }
        }

        // Try parsing <angle> <number>{3}
        if (auto angle = parse_angle_value(tokens); angle) {
            if (auto maybe_numbers = parse_three_numbers(); maybe_numbers.has_value()) {
                auto numbers = maybe_numbers.release_value();
                return RotationStyleValue::create(angle.release_nonnull(), numbers[0], numbers[1], numbers[2]);
            }
        }
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_content_value(TokenStream<ComponentValue>& tokens)
{
    // FIXME: `content` accepts several kinds of function() type, which we don't handle in property_accepts_value() yet.

    auto is_single_value_keyword = [](Keyword keyword) -> bool {
        switch (keyword) {
        case Keyword::None:
        case Keyword::Normal:
            return true;
        default:
            return false;
        }
    };

    if (tokens.remaining_token_count() == 1) {
        auto transaction = tokens.begin_transaction();
        if (auto keyword = parse_keyword_value(tokens)) {
            if (is_single_value_keyword(keyword->to_keyword())) {
                transaction.commit();
                return keyword;
            }
        }
    }

    auto transaction = tokens.begin_transaction();

    StyleValueVector content_values;
    StyleValueVector alt_text_values;
    bool in_alt_text = false;

    while (tokens.has_next_token()) {
        auto& next = tokens.next_token();
        if (next.is_delim('/')) {
            if (in_alt_text || content_values.is_empty())
                return nullptr;
            in_alt_text = true;
            tokens.discard_a_token();
            continue;
        }

        if (auto style_value = parse_css_value_for_property(PropertyID::Content, tokens)) {
            if (is_single_value_keyword(style_value->to_keyword()))
                return nullptr;

            if (in_alt_text) {
                alt_text_values.append(style_value.release_nonnull());
            } else {
                content_values.append(style_value.release_nonnull());
            }
            continue;
        }

        return nullptr;
    }

    if (content_values.is_empty())
        return nullptr;
    if (in_alt_text && alt_text_values.is_empty())
        return nullptr;

    RefPtr<StyleValueList> alt_text;
    if (!alt_text_values.is_empty())
        alt_text = StyleValueList::create(move(alt_text_values), StyleValueList::Separator::Space);

    transaction.commit();
    return ContentStyleValue::create(StyleValueList::create(move(content_values), StyleValueList::Separator::Space), move(alt_text));
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-increment
RefPtr<CSSStyleValue> Parser::parse_counter_increment_value(TokenStream<ComponentValue>& tokens)
{
    // [ <counter-name> <integer>? ]+ | none
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    return parse_counter_definitions_value(tokens, AllowReversed::No, 1);
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-reset
RefPtr<CSSStyleValue> Parser::parse_counter_reset_value(TokenStream<ComponentValue>& tokens)
{
    // [ <counter-name> <integer>? | <reversed-counter-name> <integer>? ]+ | none
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    return parse_counter_definitions_value(tokens, AllowReversed::Yes, 0);
}

// https://drafts.csswg.org/css-lists-3/#propdef-counter-set
RefPtr<CSSStyleValue> Parser::parse_counter_set_value(TokenStream<ComponentValue>& tokens)
{
    // [ <counter-name> <integer>? ]+ | none
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    return parse_counter_definitions_value(tokens, AllowReversed::No, 0);
}

// https://www.w3.org/TR/css-display-3/#the-display-properties
RefPtr<CSSStyleValue> Parser::parse_display_value(TokenStream<ComponentValue>& tokens)
{
    auto parse_single_component_display = [this](TokenStream<ComponentValue>& tokens) -> Optional<Display> {
        auto transaction = tokens.begin_transaction();
        if (auto keyword_value = parse_keyword_value(tokens)) {
            auto keyword = keyword_value->to_keyword();
            if (keyword == Keyword::ListItem) {
                transaction.commit();
                return Display::from_short(Display::Short::ListItem);
            }

            if (auto display_outside = keyword_to_display_outside(keyword); display_outside.has_value()) {
                transaction.commit();
                switch (display_outside.value()) {
                case DisplayOutside::Block:
                    return Display::from_short(Display::Short::Block);
                case DisplayOutside::Inline:
                    return Display::from_short(Display::Short::Inline);
                case DisplayOutside::RunIn:
                    return Display::from_short(Display::Short::RunIn);
                }
            }

            if (auto display_inside = keyword_to_display_inside(keyword); display_inside.has_value()) {
                transaction.commit();
                switch (display_inside.value()) {
                case DisplayInside::Flow:
                    return Display::from_short(Display::Short::Flow);
                case DisplayInside::FlowRoot:
                    return Display::from_short(Display::Short::FlowRoot);
                case DisplayInside::Table:
                    return Display::from_short(Display::Short::Table);
                case DisplayInside::Flex:
                    return Display::from_short(Display::Short::Flex);
                case DisplayInside::Grid:
                    return Display::from_short(Display::Short::Grid);
                case DisplayInside::Ruby:
                    return Display::from_short(Display::Short::Ruby);
                case DisplayInside::Math:
                    return Display::from_short(Display::Short::Math);
                }
            }

            if (auto display_internal = keyword_to_display_internal(keyword); display_internal.has_value()) {
                transaction.commit();
                return Display { display_internal.value() };
            }

            if (auto display_box = keyword_to_display_box(keyword); display_box.has_value()) {
                transaction.commit();
                switch (display_box.value()) {
                case DisplayBox::Contents:
                    return Display::from_short(Display::Short::Contents);
                case DisplayBox::None:
                    return Display::from_short(Display::Short::None);
                }
            }

            if (auto display_legacy = keyword_to_display_legacy(keyword); display_legacy.has_value()) {
                transaction.commit();
                switch (display_legacy.value()) {
                case DisplayLegacy::InlineBlock:
                    return Display::from_short(Display::Short::InlineBlock);
                case DisplayLegacy::InlineTable:
                    return Display::from_short(Display::Short::InlineTable);
                case DisplayLegacy::InlineFlex:
                    return Display::from_short(Display::Short::InlineFlex);
                case DisplayLegacy::InlineGrid:
                    return Display::from_short(Display::Short::InlineGrid);
                }
            }
        }
        return OptionalNone {};
    };

    auto parse_multi_component_display = [this](TokenStream<ComponentValue>& tokens) -> Optional<Display> {
        auto list_item = Display::ListItem::No;
        Optional<DisplayInside> inside;
        Optional<DisplayOutside> outside;

        auto transaction = tokens.begin_transaction();
        while (tokens.has_next_token()) {
            if (auto value = parse_keyword_value(tokens)) {
                auto keyword = value->to_keyword();
                if (keyword == Keyword::ListItem) {
                    if (list_item == Display::ListItem::Yes)
                        return {};
                    list_item = Display::ListItem::Yes;
                    continue;
                }
                if (auto inside_value = keyword_to_display_inside(keyword); inside_value.has_value()) {
                    if (inside.has_value())
                        return {};
                    inside = inside_value.value();
                    continue;
                }
                if (auto outside_value = keyword_to_display_outside(keyword); outside_value.has_value()) {
                    if (outside.has_value())
                        return {};
                    outside = outside_value.value();
                    continue;
                }
            }

            // Not a display value, abort.
            dbgln_if(CSS_PARSER_DEBUG, "Unrecognized display value: `{}`", tokens.next_token().to_string());
            return {};
        }

        // The spec does not allow any other inside values to be combined with list-item
        // <display-outside>? && [ flow | flow-root ]? && list-item
        if (list_item == Display::ListItem::Yes && inside.has_value() && inside != DisplayInside::Flow && inside != DisplayInside::FlowRoot)
            return {};

        transaction.commit();
        return Display { outside.value_or(DisplayOutside::Block), inside.value_or(DisplayInside::Flow), list_item };
    };

    Optional<Display> display;
    if (tokens.remaining_token_count() == 1)
        display = parse_single_component_display(tokens);
    else
        display = parse_multi_component_display(tokens);

    if (display.has_value())
        return DisplayStyleValue::create(display.value());

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_filter_value_list_value(TokenStream<ComponentValue>& tokens)
{
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    auto transaction = tokens.begin_transaction();

    // FIXME: <url>s are ignored for now
    // <filter-value-list> = [ <filter-function> | <url> ]+

    enum class FilterToken {
        // Color filters:
        Brightness,
        Contrast,
        Grayscale,
        Invert,
        Opacity,
        Saturate,
        Sepia,
        // Special filters:
        Blur,
        DropShadow,
        HueRotate
    };

    auto filter_token_to_operation = [&](auto filter) {
        VERIFY(to_underlying(filter) < to_underlying(FilterToken::Blur));
        return static_cast<FilterOperation::Color::Type>(filter);
    };

    auto parse_filter_function_name = [&](auto name) -> Optional<FilterToken> {
        if (name.equals_ignoring_ascii_case("blur"sv))
            return FilterToken::Blur;
        if (name.equals_ignoring_ascii_case("brightness"sv))
            return FilterToken::Brightness;
        if (name.equals_ignoring_ascii_case("contrast"sv))
            return FilterToken::Contrast;
        if (name.equals_ignoring_ascii_case("drop-shadow"sv))
            return FilterToken::DropShadow;
        if (name.equals_ignoring_ascii_case("grayscale"sv))
            return FilterToken::Grayscale;
        if (name.equals_ignoring_ascii_case("hue-rotate"sv))
            return FilterToken::HueRotate;
        if (name.equals_ignoring_ascii_case("invert"sv))
            return FilterToken::Invert;
        if (name.equals_ignoring_ascii_case("opacity"sv))
            return FilterToken::Opacity;
        if (name.equals_ignoring_ascii_case("saturate"sv))
            return FilterToken::Saturate;
        if (name.equals_ignoring_ascii_case("sepia"sv))
            return FilterToken::Sepia;
        return {};
    };

    auto parse_filter_function = [&](auto filter_token, auto const& function_values) -> Optional<FilterFunction> {
        TokenStream tokens { function_values };
        tokens.discard_whitespace();

        auto if_no_more_tokens_return = [&](auto filter) -> Optional<FilterFunction> {
            tokens.discard_whitespace();
            if (tokens.has_next_token())
                return {};
            return filter;
        };

        if (filter_token == FilterToken::Blur) {
            // blur( <length>? )
            if (!tokens.has_next_token())
                return FilterOperation::Blur {};
            auto blur_radius = parse_length(tokens);
            tokens.discard_whitespace();
            if (!blur_radius.has_value())
                return {};
            return if_no_more_tokens_return(FilterOperation::Blur { blur_radius.value() });
        } else if (filter_token == FilterToken::DropShadow) {
            if (!tokens.has_next_token())
                return {};
            // drop-shadow( [ <color>? && <length>{2,3} ] )
            // Note: The following code is a little awkward to allow the color to be before or after the lengths.
            Optional<LengthOrCalculated> maybe_radius = {};
            auto maybe_color = parse_color_value(tokens);
            tokens.discard_whitespace();
            auto x_offset = parse_length(tokens);
            tokens.discard_whitespace();
            if (!x_offset.has_value() || !tokens.has_next_token())
                return {};

            auto y_offset = parse_length(tokens);
            tokens.discard_whitespace();
            if (!y_offset.has_value())
                return {};

            if (tokens.has_next_token()) {
                maybe_radius = parse_length(tokens);
                tokens.discard_whitespace();
                if (!maybe_color && (!maybe_radius.has_value() || tokens.has_next_token())) {
                    maybe_color = parse_color_value(tokens);
                    if (!maybe_color)
                        return {};
                } else if (!maybe_radius.has_value()) {
                    return {};
                }
            }
            Optional<Color> color = {};
            if (maybe_color)
                color = maybe_color->to_color({});

            return if_no_more_tokens_return(FilterOperation::DropShadow { x_offset.value(), y_offset.value(), maybe_radius, color });
        } else if (filter_token == FilterToken::HueRotate) {
            // hue-rotate( [ <angle> | <zero> ]? )
            if (!tokens.has_next_token())
                return FilterOperation::HueRotate {};

            if (tokens.next_token().is(Token::Type::Number)) {
                // hue-rotate(0)
                auto number = tokens.consume_a_token().token().number();
                if (number.is_integer() && number.integer_value() == 0)
                    return if_no_more_tokens_return(FilterOperation::HueRotate { FilterOperation::HueRotate::Zero {} });
                return {};
            }

            if (auto angle = parse_angle(tokens); angle.has_value())
                return if_no_more_tokens_return(FilterOperation::HueRotate { angle.value() });

            return {};
        } else {
            // Simple filters:
            // brightness( <number-percentage>? )
            // contrast( <number-percentage>? )
            // grayscale( <number-percentage>? )
            // invert( <number-percentage>? )
            // opacity( <number-percentage>? )
            // sepia( <number-percentage>? )
            // saturate( <number-percentage>? )
            if (!tokens.has_next_token())
                return FilterOperation::Color { filter_token_to_operation(filter_token) };
            auto amount = parse_number_percentage(tokens);
            return if_no_more_tokens_return(FilterOperation::Color { filter_token_to_operation(filter_token), amount });
        }
    };

    Vector<FilterFunction> filter_value_list {};

    while (tokens.has_next_token()) {
        tokens.discard_whitespace();
        if (!tokens.has_next_token())
            break;
        auto& token = tokens.consume_a_token();
        if (!token.is_function())
            return nullptr;
        auto filter_token = parse_filter_function_name(token.function().name);
        if (!filter_token.has_value())
            return nullptr;
        auto filter_function = parse_filter_function(*filter_token, token.function().value);
        if (!filter_function.has_value())
            return nullptr;
        filter_value_list.append(*filter_function);
    }

    if (filter_value_list.is_empty())
        return nullptr;

    transaction.commit();
    return FilterValueListStyleValue::create(move(filter_value_list));
}

RefPtr<CSSStyleValue> Parser::parse_flex_shorthand_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto make_flex_shorthand = [&](NonnullRefPtr<CSSStyleValue> flex_grow, NonnullRefPtr<CSSStyleValue> flex_shrink, NonnullRefPtr<CSSStyleValue> flex_basis) {
        transaction.commit();
        return ShorthandStyleValue::create(PropertyID::Flex,
            { PropertyID::FlexGrow, PropertyID::FlexShrink, PropertyID::FlexBasis },
            { move(flex_grow), move(flex_shrink), move(flex_basis) });
    };

    if (tokens.remaining_token_count() == 1) {
        // One-value syntax: <flex-grow> | <flex-basis> | none
        auto properties = Array { PropertyID::FlexGrow, PropertyID::FlexBasis, PropertyID::Flex };
        auto property_and_value = parse_css_value_for_properties(properties, tokens);
        if (!property_and_value.has_value())
            return nullptr;

        auto& value = property_and_value->style_value;
        switch (property_and_value->property) {
        case PropertyID::FlexGrow: {
            // NOTE: The spec says that flex-basis should be 0 here, but other engines currently use 0%.
            // https://github.com/w3c/csswg-drafts/issues/5742
            auto flex_basis = PercentageStyleValue::create(Percentage(0));
            auto one = NumberStyleValue::create(1);
            return make_flex_shorthand(*value, one, flex_basis);
        }
        case PropertyID::FlexBasis: {
            auto one = NumberStyleValue::create(1);
            return make_flex_shorthand(one, one, *value);
        }
        case PropertyID::Flex: {
            if (value->is_keyword() && value->to_keyword() == Keyword::None) {
                auto zero = NumberStyleValue::create(0);
                return make_flex_shorthand(zero, zero, CSSKeywordValue::create(Keyword::Auto));
            }
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }

        return nullptr;
    }

    RefPtr<CSSStyleValue> flex_grow;
    RefPtr<CSSStyleValue> flex_shrink;
    RefPtr<CSSStyleValue> flex_basis;

    // NOTE: FlexGrow has to be before FlexBasis. `0` is a valid FlexBasis, but only
    //       if FlexGrow (along with optional FlexShrink) have already been specified.
    auto remaining_longhands = Vector { PropertyID::FlexGrow, PropertyID::FlexBasis };

    while (tokens.has_next_token()) {
        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;
        remove_property(remaining_longhands, property_and_value->property);

        switch (property_and_value->property) {
        case PropertyID::FlexGrow: {
            VERIFY(!flex_grow);
            flex_grow = value.release_nonnull();

            // Flex-shrink may optionally follow directly after.
            auto maybe_flex_shrink = parse_css_value_for_property(PropertyID::FlexShrink, tokens);
            if (maybe_flex_shrink)
                flex_shrink = maybe_flex_shrink.release_nonnull();
            continue;
        }
        case PropertyID::FlexBasis: {
            VERIFY(!flex_basis);
            flex_basis = value.release_nonnull();
            continue;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (!flex_grow)
        flex_grow = property_initial_value(m_context.realm(), PropertyID::FlexGrow);
    if (!flex_shrink)
        flex_shrink = property_initial_value(m_context.realm(), PropertyID::FlexShrink);
    if (!flex_basis) {
        // NOTE: The spec says that flex-basis should be 0 here, but other engines currently use 0%.
        // https://github.com/w3c/csswg-drafts/issues/5742
        flex_basis = PercentageStyleValue::create(Percentage(0));
    }

    return make_flex_shorthand(flex_grow.release_nonnull(), flex_shrink.release_nonnull(), flex_basis.release_nonnull());
}

RefPtr<CSSStyleValue> Parser::parse_flex_flow_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<CSSStyleValue> flex_direction;
    RefPtr<CSSStyleValue> flex_wrap;

    auto remaining_longhands = Vector { PropertyID::FlexDirection, PropertyID::FlexWrap };
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;
        remove_property(remaining_longhands, property_and_value->property);

        switch (property_and_value->property) {
        case PropertyID::FlexDirection:
            VERIFY(!flex_direction);
            flex_direction = value.release_nonnull();
            continue;
        case PropertyID::FlexWrap:
            VERIFY(!flex_wrap);
            flex_wrap = value.release_nonnull();
            continue;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (!flex_direction)
        flex_direction = property_initial_value(m_context.realm(), PropertyID::FlexDirection);
    if (!flex_wrap)
        flex_wrap = property_initial_value(m_context.realm(), PropertyID::FlexWrap);

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::FlexFlow,
        { PropertyID::FlexDirection, PropertyID::FlexWrap },
        { flex_direction.release_nonnull(), flex_wrap.release_nonnull() });
}

bool Parser::is_generic_font_family(Keyword keyword)
{
    switch (keyword) {
    case Keyword::Cursive:
    case Keyword::Fantasy:
    case Keyword::Monospace:
    case Keyword::Serif:
    case Keyword::SansSerif:
    case Keyword::UiMonospace:
    case Keyword::UiRounded:
    case Keyword::UiSerif:
    case Keyword::UiSansSerif:
        return true;
    default:
        return false;
    }
}

RefPtr<CSSStyleValue> Parser::parse_font_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<CSSStyleValue> font_width;
    RefPtr<CSSStyleValue> font_style;
    RefPtr<CSSStyleValue> font_weight;
    RefPtr<CSSStyleValue> font_size;
    RefPtr<CSSStyleValue> line_height;
    RefPtr<CSSStyleValue> font_families;
    RefPtr<CSSStyleValue> font_variant;

    // FIXME: Handle system fonts. (caption, icon, menu, message-box, small-caption, status-bar)

    // Several sub-properties can be "normal", and appear in any order: style, variant, weight, stretch
    // So, we have to handle that separately.
    int normal_count = 0;

    // FIXME: `font-variant` allows a lot of different values which aren't allowed in the `font` shorthand.
    // FIXME: `font-width` allows <percentage> values, which aren't allowed in the `font` shorthand.
    auto remaining_longhands = Vector { PropertyID::FontSize, PropertyID::FontStyle, PropertyID::FontVariant, PropertyID::FontWeight, PropertyID::FontWidth };
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        auto& peek_token = tokens.next_token();
        if (peek_token.is_ident("normal"sv)) {
            normal_count++;
            tokens.discard_a_token();
            continue;
        }

        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;
        remove_property(remaining_longhands, property_and_value->property);

        switch (property_and_value->property) {
        case PropertyID::FontSize: {
            VERIFY(!font_size);
            font_size = value.release_nonnull();

            // Consume `/ line-height` if present
            if (tokens.next_token().is_delim('/')) {
                tokens.discard_a_token();
                auto maybe_line_height = parse_css_value_for_property(PropertyID::LineHeight, tokens);
                if (!maybe_line_height)
                    return nullptr;
                line_height = maybe_line_height.release_nonnull();
            }

            // Consume font-families
            auto maybe_font_families = parse_font_family_value(tokens);
            // font-family comes last, so we must not have any tokens left over.
            if (!maybe_font_families || tokens.has_next_token())
                return nullptr;
            font_families = maybe_font_families.release_nonnull();
            continue;
        }
        case PropertyID::FontWidth: {
            VERIFY(!font_width);
            font_width = value.release_nonnull();
            continue;
        }
        case PropertyID::FontStyle: {
            VERIFY(!font_style);
            font_style = value.release_nonnull();
            continue;
        }
        case PropertyID::FontVariant: {
            VERIFY(!font_variant);
            font_variant = value.release_nonnull();
            continue;
        }
        case PropertyID::FontWeight: {
            VERIFY(!font_weight);
            font_weight = value.release_nonnull();
            continue;
        }
        default:
            VERIFY_NOT_REACHED();
        }

        return nullptr;
    }

    // Since normal is the default value for all the properties that can have it, we don't have to actually
    // set anything to normal here. It'll be set when we create the ShorthandStyleValue below.
    // We just need to make sure we were not given more normals than will fit.
    int unset_value_count = (font_style ? 0 : 1) + (font_weight ? 0 : 1) + (font_variant ? 0 : 1) + (font_width ? 0 : 1);
    if (unset_value_count < normal_count)
        return nullptr;

    if (!font_size || !font_families)
        return nullptr;

    if (!font_style)
        font_style = property_initial_value(m_context.realm(), PropertyID::FontStyle);
    if (!font_variant)
        font_variant = property_initial_value(m_context.realm(), PropertyID::FontVariant);
    if (!font_weight)
        font_weight = property_initial_value(m_context.realm(), PropertyID::FontWeight);
    if (!font_width)
        font_width = property_initial_value(m_context.realm(), PropertyID::FontWidth);
    if (!line_height)
        line_height = property_initial_value(m_context.realm(), PropertyID::LineHeight);

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::Font,
        { PropertyID::FontStyle, PropertyID::FontVariant, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::FontSize, PropertyID::LineHeight, PropertyID::FontFamily },
        { font_style.release_nonnull(), font_variant.release_nonnull(), font_weight.release_nonnull(), font_width.release_nonnull(), font_size.release_nonnull(), line_height.release_nonnull(), font_families.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_font_family_value(TokenStream<ComponentValue>& tokens)
{
    auto next_is_comma_or_eof = [&]() -> bool {
        return !tokens.has_next_token() || tokens.next_token().is(Token::Type::Comma);
    };

    // Note: Font-family names can either be a quoted string, or a keyword, or a series of custom-idents.
    // eg, these are equivalent:
    //     font-family: my cool     font\!, serif;
    //     font-family: "my cool font!", serif;
    StyleValueVector font_families;
    Vector<String> current_name_parts;
    while (tokens.has_next_token()) {
        auto const& peek = tokens.next_token();

        if (peek.is(Token::Type::String)) {
            // `font-family: my cool "font";` is invalid.
            if (!current_name_parts.is_empty())
                return nullptr;
            tokens.discard_a_token(); // String
            if (!next_is_comma_or_eof())
                return nullptr;
            font_families.append(StringStyleValue::create(peek.token().string()));
            tokens.discard_a_token(); // Comma
            continue;
        }

        if (peek.is(Token::Type::Ident)) {
            // If this is a valid identifier, it's NOT a custom-ident and can't be part of a larger name.

            // CSS-wide keywords are not allowed
            if (auto builtin = parse_builtin_value(tokens))
                return nullptr;

            auto maybe_keyword = keyword_from_string(peek.token().ident());
            // Can't have a generic-font-name as a token in an unquoted font name.
            if (maybe_keyword.has_value() && is_generic_font_family(maybe_keyword.value())) {
                if (!current_name_parts.is_empty())
                    return nullptr;
                tokens.discard_a_token(); // Ident
                if (!next_is_comma_or_eof())
                    return nullptr;
                font_families.append(CSSKeywordValue::create(maybe_keyword.value()));
                tokens.discard_a_token(); // Comma
                continue;
            }
            current_name_parts.append(tokens.consume_a_token().token().ident().to_string());
            continue;
        }

        if (peek.is(Token::Type::Comma)) {
            if (current_name_parts.is_empty())
                return nullptr;
            tokens.discard_a_token(); // Comma
            // This is really a series of custom-idents, not just one. But for the sake of simplicity we'll make it one.
            font_families.append(CustomIdentStyleValue::create(MUST(String::join(' ', current_name_parts))));
            current_name_parts.clear();
            // Can't have a trailing comma
            if (!tokens.has_next_token())
                return nullptr;
            continue;
        }

        return nullptr;
    }

    if (!current_name_parts.is_empty()) {
        // This is really a series of custom-idents, not just one. But for the sake of simplicity we'll make it one.
        font_families.append(CustomIdentStyleValue::create(MUST(String::join(' ', current_name_parts))));
        current_name_parts.clear();
    }

    if (font_families.is_empty())
        return nullptr;
    return StyleValueList::create(move(font_families), StyleValueList::Separator::Comma);
}

RefPtr<CSSStyleValue> Parser::parse_font_language_override_value(TokenStream<ComponentValue>& tokens)
{
    // https://drafts.csswg.org/css-fonts/#propdef-font-language-override
    // This is `normal | <string>` but with the constraint that the string has to be 4 characters long:
    // Shorter strings are right-padded with spaces, and longer strings are invalid.

    if (auto normal = parse_all_as_single_keyword_value(tokens, Keyword::Normal))
        return normal;

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    if (auto string = parse_string_value(tokens)) {
        auto string_value = string->string_value();
        tokens.discard_whitespace();
        if (tokens.has_next_token()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Failed to parse font-language-override: unexpected trailing tokens");
            return nullptr;
        }
        auto length = string_value.code_points().length();
        if (length > 4) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Failed to parse font-language-override: <string> value \"{}\" is too long", string_value);
            return nullptr;
        }
        transaction.commit();
        if (length < 4)
            return StringStyleValue::create(MUST(String::formatted("{:<4}", string_value)));
        return string;
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_font_feature_settings_value(TokenStream<ComponentValue>& tokens)
{
    // https://drafts.csswg.org/css-fonts/#propdef-font-feature-settings
    // normal | <feature-tag-value>#

    // normal
    if (auto normal = parse_all_as_single_keyword_value(tokens, Keyword::Normal))
        return normal;

    // <feature-tag-value>#
    auto transaction = tokens.begin_transaction();
    auto tag_values = parse_a_comma_separated_list_of_component_values(tokens);

    // "The computed value of font-feature-settings is a map, so any duplicates in the specified value must not be preserved.
    // If the same feature tag appears more than once, the value associated with the last appearance supersedes any previous
    // value for that axis."
    // So, we deduplicate them here using a HashSet.

    OrderedHashMap<FlyString, NonnullRefPtr<OpenTypeTaggedStyleValue>> feature_tags_map;
    for (auto const& values : tag_values) {
        // <feature-tag-value> = <opentype-tag> [ <integer [0,∞]> | on | off ]?
        TokenStream tag_tokens { values };
        tag_tokens.discard_whitespace();
        auto opentype_tag = parse_opentype_tag_value(tag_tokens);
        tag_tokens.discard_whitespace();
        RefPtr<CSSStyleValue> value;
        if (tag_tokens.has_next_token()) {
            if (auto integer = parse_integer_value(tag_tokens)) {
                if (integer->is_integer() && integer->as_integer().value() < 0)
                    return nullptr;
                value = integer;
            } else {
                // A value of on is synonymous with 1 and off is synonymous with 0.
                auto keyword = parse_keyword_value(tag_tokens);
                if (!keyword)
                    return nullptr;
                switch (keyword->to_keyword()) {
                case Keyword::On:
                    value = IntegerStyleValue::create(1);
                    break;
                case Keyword::Off:
                    value = IntegerStyleValue::create(0);
                    break;
                default:
                    return nullptr;
                }
            }
            tag_tokens.discard_whitespace();
        } else {
            // "If the value is omitted, a value of 1 is assumed."
            value = IntegerStyleValue::create(1);
        }

        if (!opentype_tag || !value || tag_tokens.has_next_token())
            return nullptr;

        feature_tags_map.set(opentype_tag->string_value(), OpenTypeTaggedStyleValue::create(opentype_tag->string_value(), value.release_nonnull()));
    }

    // "The computed value contains the de-duplicated feature tags, sorted in ascending order by code unit."
    StyleValueVector feature_tags;
    feature_tags.ensure_capacity(feature_tags_map.size());
    for (auto const& [key, feature_tag] : feature_tags_map)
        feature_tags.append(feature_tag);

    quick_sort(feature_tags, [](auto& a, auto& b) {
        return a->as_open_type_tagged().tag() < b->as_open_type_tagged().tag();
    });

    transaction.commit();
    return StyleValueList::create(move(feature_tags), StyleValueList::Separator::Comma);
}

RefPtr<CSSStyleValue> Parser::parse_font_variation_settings_value(TokenStream<ComponentValue>& tokens)
{
    // https://drafts.csswg.org/css-fonts/#propdef-font-variation-settings
    // normal | [ <opentype-tag> <number>]#

    // normal
    if (auto normal = parse_all_as_single_keyword_value(tokens, Keyword::Normal))
        return normal;

    // [ <opentype-tag> <number>]#
    auto transaction = tokens.begin_transaction();
    auto tag_values = parse_a_comma_separated_list_of_component_values(tokens);

    // "If the same axis name appears more than once, the value associated with the last appearance supersedes any
    // previous value for that axis. This deduplication is observable by accessing the computed value of this property."
    // So, we deduplicate them here using a HashSet.

    OrderedHashMap<FlyString, NonnullRefPtr<OpenTypeTaggedStyleValue>> axis_tags_map;
    for (auto const& values : tag_values) {
        TokenStream tag_tokens { values };
        tag_tokens.discard_whitespace();
        auto opentype_tag = parse_opentype_tag_value(tag_tokens);
        tag_tokens.discard_whitespace();
        auto number = parse_number_value(tag_tokens);
        tag_tokens.discard_whitespace();

        if (!opentype_tag || !number || tag_tokens.has_next_token())
            return nullptr;

        axis_tags_map.set(opentype_tag->string_value(), OpenTypeTaggedStyleValue::create(opentype_tag->string_value(), number.release_nonnull()));
    }

    // "The computed value contains the de-duplicated axis names, sorted in ascending order by code unit."
    StyleValueVector axis_tags;
    axis_tags.ensure_capacity(axis_tags_map.size());
    for (auto const& [key, axis_tag] : axis_tags_map)
        axis_tags.append(axis_tag);

    quick_sort(axis_tags, [](auto& a, auto& b) {
        return a->as_open_type_tagged().tag() < b->as_open_type_tagged().tag();
    });

    transaction.commit();
    return StyleValueList::create(move(axis_tags), StyleValueList::Separator::Comma);
}

Vector<ParsedFontFace::Source> Parser::parse_as_font_face_src()
{
    return parse_font_face_src(m_token_stream);
}

template<typename T>
Vector<ParsedFontFace::Source> Parser::parse_font_face_src(TokenStream<T>& component_values)
{
    // FIXME: Get this information from the system somehow?
    // Format-name table: https://www.w3.org/TR/css-fonts-4/#font-format-definitions
    auto font_format_is_supported = [](StringView name) {
        // The spec requires us to treat opentype and truetype as synonymous.
        if (name.is_one_of_ignoring_ascii_case("opentype"sv, "truetype"sv, "woff"sv, "woff2"sv))
            return true;
        return false;
    };

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
                TokenStream format_tokens { function.value };
                format_tokens.discard_whitespace();
                auto const& format_name_token = format_tokens.consume_a_token();
                StringView format_name;
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

                format = FlyString::from_utf8(format_name).release_value_but_fixme_should_propagate_errors();
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
            if (first.function().value.is_empty()) {
                continue;
            }
            supported_sources.empend(first.function().value.first().to_string(), Optional<FlyString> {});
            continue;
        }

        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (failed to parse url from: {}); discarding.", first.to_debug_string());
        return {};
    }

    return supported_sources;
}
template Vector<ParsedFontFace::Source> Parser::parse_font_face_src(TokenStream<Token>& component_values);
template Vector<ParsedFontFace::Source> Parser::parse_font_face_src(TokenStream<ComponentValue>& component_values);

RefPtr<CSSStyleValue> Parser::parse_list_style_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<CSSStyleValue> list_position;
    RefPtr<CSSStyleValue> list_image;
    RefPtr<CSSStyleValue> list_type;
    int found_nones = 0;

    Vector<PropertyID> remaining_longhands { PropertyID::ListStyleImage, PropertyID::ListStylePosition, PropertyID::ListStyleType };

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        if (auto const& peek = tokens.next_token(); peek.is_ident("none"sv)) {
            tokens.discard_a_token();
            found_nones++;
            continue;
        }

        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;
        remove_property(remaining_longhands, property_and_value->property);

        switch (property_and_value->property) {
        case PropertyID::ListStylePosition: {
            VERIFY(!list_position);
            list_position = value.release_nonnull();
            continue;
        }
        case PropertyID::ListStyleImage: {
            VERIFY(!list_image);
            list_image = value.release_nonnull();
            continue;
        }
        case PropertyID::ListStyleType: {
            VERIFY(!list_type);
            list_type = value.release_nonnull();
            continue;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (found_nones > 2)
        return nullptr;

    if (found_nones == 2) {
        if (list_image || list_type)
            return nullptr;
        auto none = CSSKeywordValue::create(Keyword::None);
        list_image = none;
        list_type = none;

    } else if (found_nones == 1) {
        if (list_image && list_type)
            return nullptr;
        auto none = CSSKeywordValue::create(Keyword::None);
        if (!list_image)
            list_image = none;
        if (!list_type)
            list_type = none;
    }

    if (!list_position)
        list_position = property_initial_value(m_context.realm(), PropertyID::ListStylePosition);
    if (!list_image)
        list_image = property_initial_value(m_context.realm(), PropertyID::ListStyleImage);
    if (!list_type)
        list_type = property_initial_value(m_context.realm(), PropertyID::ListStyleType);

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::ListStyle,
        { PropertyID::ListStylePosition, PropertyID::ListStyleImage, PropertyID::ListStyleType },
        { list_position.release_nonnull(), list_image.release_nonnull(), list_type.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_math_depth_value(TokenStream<ComponentValue>& tokens)
{
    // https://w3c.github.io/mathml-core/#propdef-math-depth
    // auto-add | add(<integer>) | <integer>
    auto transaction = tokens.begin_transaction();

    auto const& token = tokens.consume_a_token();
    if (tokens.has_next_token())
        return nullptr;

    // auto-add
    if (token.is_ident("auto-add"sv)) {
        transaction.commit();
        return MathDepthStyleValue::create_auto_add();
    }

    // FIXME: Make it easier to parse "thing that might be <bar> or literally anything that resolves to it" and get rid of this
    auto parse_something_that_resolves_to_integer = [this](ComponentValue const& token) -> RefPtr<CSSStyleValue> {
        if (token.is(Token::Type::Number) && token.token().number().is_integer())
            return IntegerStyleValue::create(token.token().to_integer());
        if (auto value = parse_calculated_value(token); value && value->resolves_to_number())
            return value;
        return nullptr;
    };

    // add(<integer>)
    if (token.is_function("add"sv)) {
        auto add_tokens = TokenStream { token.function().value };
        add_tokens.discard_whitespace();
        auto const& integer_token = add_tokens.consume_a_token();
        add_tokens.discard_whitespace();
        if (add_tokens.has_next_token())
            return nullptr;
        if (auto integer_value = parse_something_that_resolves_to_integer(integer_token)) {
            transaction.commit();
            return MathDepthStyleValue::create_add(integer_value.release_nonnull());
        }
        return nullptr;
    }

    // <integer>
    if (auto integer_value = parse_something_that_resolves_to_integer(token)) {
        transaction.commit();
        return MathDepthStyleValue::create_integer(integer_value.release_nonnull());
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_overflow_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_x_value = parse_css_value_for_property(PropertyID::OverflowX, tokens);
    if (!maybe_x_value)
        return nullptr;
    auto maybe_y_value = parse_css_value_for_property(PropertyID::OverflowY, tokens);
    transaction.commit();
    if (maybe_y_value) {
        return ShorthandStyleValue::create(PropertyID::Overflow,
            { PropertyID::OverflowX, PropertyID::OverflowY },
            { maybe_x_value.release_nonnull(), maybe_y_value.release_nonnull() });
    }
    return ShorthandStyleValue::create(PropertyID::Overflow,
        { PropertyID::OverflowX, PropertyID::OverflowY },
        { *maybe_x_value, *maybe_x_value });
}

RefPtr<CSSStyleValue> Parser::parse_place_content_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_align_content_value = parse_css_value_for_property(PropertyID::AlignContent, tokens);
    if (!maybe_align_content_value)
        return nullptr;

    if (!tokens.has_next_token()) {
        if (!property_accepts_keyword(PropertyID::JustifyContent, maybe_align_content_value->to_keyword()))
            return nullptr;
        transaction.commit();
        return ShorthandStyleValue::create(PropertyID::PlaceContent,
            { PropertyID::AlignContent, PropertyID::JustifyContent },
            { *maybe_align_content_value, *maybe_align_content_value });
    }

    auto maybe_justify_content_value = parse_css_value_for_property(PropertyID::JustifyContent, tokens);
    if (!maybe_justify_content_value)
        return nullptr;
    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::PlaceContent,
        { PropertyID::AlignContent, PropertyID::JustifyContent },
        { maybe_align_content_value.release_nonnull(), maybe_justify_content_value.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_place_items_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_align_items_value = parse_css_value_for_property(PropertyID::AlignItems, tokens);
    if (!maybe_align_items_value)
        return nullptr;

    if (!tokens.has_next_token()) {
        if (!property_accepts_keyword(PropertyID::JustifyItems, maybe_align_items_value->to_keyword()))
            return nullptr;
        transaction.commit();
        return ShorthandStyleValue::create(PropertyID::PlaceItems,
            { PropertyID::AlignItems, PropertyID::JustifyItems },
            { *maybe_align_items_value, *maybe_align_items_value });
    }

    auto maybe_justify_items_value = parse_css_value_for_property(PropertyID::JustifyItems, tokens);
    if (!maybe_justify_items_value)
        return nullptr;
    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::PlaceItems,
        { PropertyID::AlignItems, PropertyID::JustifyItems },
        { *maybe_align_items_value, *maybe_justify_items_value });
}

RefPtr<CSSStyleValue> Parser::parse_place_self_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_align_self_value = parse_css_value_for_property(PropertyID::AlignSelf, tokens);
    if (!maybe_align_self_value)
        return nullptr;

    if (!tokens.has_next_token()) {
        if (!property_accepts_keyword(PropertyID::JustifySelf, maybe_align_self_value->to_keyword()))
            return nullptr;
        transaction.commit();
        return ShorthandStyleValue::create(PropertyID::PlaceSelf,
            { PropertyID::AlignSelf, PropertyID::JustifySelf },
            { *maybe_align_self_value, *maybe_align_self_value });
    }

    auto maybe_justify_self_value = parse_css_value_for_property(PropertyID::JustifySelf, tokens);
    if (!maybe_justify_self_value)
        return nullptr;
    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::PlaceSelf,
        { PropertyID::AlignSelf, PropertyID::JustifySelf },
        { *maybe_align_self_value, *maybe_justify_self_value });
}

RefPtr<CSSStyleValue> Parser::parse_quotes_value(TokenStream<ComponentValue>& tokens)
{
    // https://www.w3.org/TR/css-content-3/#quotes-property
    // auto | none | [ <string> <string> ]+
    auto transaction = tokens.begin_transaction();

    if (tokens.remaining_token_count() == 1) {
        auto keyword = parse_keyword_value(tokens);
        if (keyword && property_accepts_keyword(PropertyID::Quotes, keyword->to_keyword())) {
            transaction.commit();
            return keyword;
        }
        return nullptr;
    }

    // Parse an even number of <string> values.
    if (tokens.remaining_token_count() % 2 != 0)
        return nullptr;

    StyleValueVector string_values;
    while (tokens.has_next_token()) {
        auto maybe_string = parse_string_value(tokens);
        if (!maybe_string)
            return nullptr;

        string_values.append(maybe_string.release_nonnull());
    }

    transaction.commit();
    return StyleValueList::create(move(string_values), StyleValueList::Separator::Space);
}

RefPtr<CSSStyleValue> Parser::parse_text_decoration_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<CSSStyleValue> decoration_line;
    RefPtr<CSSStyleValue> decoration_thickness;
    RefPtr<CSSStyleValue> decoration_style;
    RefPtr<CSSStyleValue> decoration_color;

    auto remaining_longhands = Vector { PropertyID::TextDecorationColor, PropertyID::TextDecorationLine, PropertyID::TextDecorationStyle, PropertyID::TextDecorationThickness };

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto property_and_value = parse_css_value_for_properties(remaining_longhands, tokens);
        if (!property_and_value.has_value())
            return nullptr;
        auto& value = property_and_value->style_value;
        remove_property(remaining_longhands, property_and_value->property);

        switch (property_and_value->property) {
        case PropertyID::TextDecorationColor: {
            VERIFY(!decoration_color);
            decoration_color = value.release_nonnull();
            continue;
        }
        case PropertyID::TextDecorationLine: {
            VERIFY(!decoration_line);
            tokens.reconsume_current_input_token();
            auto parsed_decoration_line = parse_text_decoration_line_value(tokens);
            if (!parsed_decoration_line)
                return nullptr;
            decoration_line = parsed_decoration_line.release_nonnull();
            continue;
        }
        case PropertyID::TextDecorationThickness: {
            VERIFY(!decoration_thickness);
            decoration_thickness = value.release_nonnull();
            continue;
        }
        case PropertyID::TextDecorationStyle: {
            VERIFY(!decoration_style);
            decoration_style = value.release_nonnull();
            continue;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (!decoration_line)
        decoration_line = property_initial_value(m_context.realm(), PropertyID::TextDecorationLine);
    if (!decoration_thickness)
        decoration_thickness = property_initial_value(m_context.realm(), PropertyID::TextDecorationThickness);
    if (!decoration_style)
        decoration_style = property_initial_value(m_context.realm(), PropertyID::TextDecorationStyle);
    if (!decoration_color)
        decoration_color = property_initial_value(m_context.realm(), PropertyID::TextDecorationColor);

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::TextDecoration,
        { PropertyID::TextDecorationLine, PropertyID::TextDecorationThickness, PropertyID::TextDecorationStyle, PropertyID::TextDecorationColor },
        { decoration_line.release_nonnull(), decoration_thickness.release_nonnull(), decoration_style.release_nonnull(), decoration_color.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_text_decoration_line_value(TokenStream<ComponentValue>& tokens)
{
    StyleValueVector style_values;

    while (tokens.has_next_token()) {
        auto maybe_value = parse_css_value_for_property(PropertyID::TextDecorationLine, tokens);
        if (!maybe_value)
            break;
        auto value = maybe_value.release_nonnull();

        if (auto maybe_line = keyword_to_text_decoration_line(value->to_keyword()); maybe_line.has_value()) {
            if (maybe_line == TextDecorationLine::None) {
                if (!style_values.is_empty())
                    break;
                return value;
            }
            if (style_values.contains_slow(value))
                break;
            style_values.append(move(value));
            continue;
        }

        break;
    }

    if (style_values.is_empty())
        return nullptr;
    return StyleValueList::create(move(style_values), StyleValueList::Separator::Space);
}

RefPtr<CSSStyleValue> Parser::parse_easing_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    tokens.discard_whitespace();

    auto const& part = tokens.consume_a_token();

    if (part.is(Token::Type::Ident)) {
        auto name = part.token().ident();
        auto maybe_simple_easing = [&] -> RefPtr<EasingStyleValue> {
            if (name.equals_ignoring_ascii_case("linear"sv))
                return EasingStyleValue::create(EasingStyleValue::Linear::identity());
            if (name.equals_ignoring_ascii_case("ease"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease());
            if (name.equals_ignoring_ascii_case("ease-in"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_in());
            if (name.equals_ignoring_ascii_case("ease-out"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_out());
            if (name.equals_ignoring_ascii_case("ease-in-out"sv))
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_in_out());
            if (name.equals_ignoring_ascii_case("step-start"sv))
                return EasingStyleValue::create(EasingStyleValue::Steps::step_start());
            if (name.equals_ignoring_ascii_case("step-end"sv))
                return EasingStyleValue::create(EasingStyleValue::Steps::step_end());
            return {};
        }();

        if (!maybe_simple_easing)
            return nullptr;

        transaction.commit();
        return maybe_simple_easing;
    }

    if (!part.is_function())
        return nullptr;

    TokenStream argument_tokens { part.function().value };
    auto comma_separated_arguments = parse_a_comma_separated_list_of_component_values(argument_tokens);

    // Remove whitespace
    for (auto& argument : comma_separated_arguments)
        argument.remove_all_matching([](auto& value) { return value.is(Token::Type::Whitespace); });

    auto name = part.function().name;
    if (name.equals_ignoring_ascii_case("linear"sv)) {
        // linear() = linear( [ <number> && <percentage>{0,2} ]# )
        Vector<EasingStyleValue::Linear::Stop> stops;
        for (auto const& argument : comma_separated_arguments) {
            TokenStream argument_tokens { argument };

            Optional<double> output;
            Optional<double> first_input;
            Optional<double> second_input;

            if (argument_tokens.next_token().is(Token::Type::Number))
                output = argument_tokens.consume_a_token().token().number_value();

            if (argument_tokens.next_token().is(Token::Type::Percentage)) {
                first_input = argument_tokens.consume_a_token().token().percentage() / 100;
                if (argument_tokens.next_token().is(Token::Type::Percentage)) {
                    second_input = argument_tokens.consume_a_token().token().percentage() / 100;
                }
            }

            if (argument_tokens.next_token().is(Token::Type::Number)) {
                if (output.has_value())
                    return nullptr;
                output = argument_tokens.consume_a_token().token().number_value();
            }

            if (argument_tokens.has_next_token() || !output.has_value())
                return nullptr;

            stops.append({ output.value(), first_input, first_input.has_value() });
            if (second_input.has_value())
                stops.append({ output.value(), second_input, true });
        }

        if (stops.is_empty())
            return nullptr;

        transaction.commit();
        return EasingStyleValue::create(EasingStyleValue::Linear { move(stops) });
    }

    if (name.equals_ignoring_ascii_case("cubic-bezier"sv)) {
        if (comma_separated_arguments.size() != 4)
            return nullptr;

        for (auto const& argument : comma_separated_arguments) {
            if (argument.size() != 1)
                return nullptr;
            if (!argument[0].is(Token::Type::Number))
                return nullptr;
        }

        EasingStyleValue::CubicBezier bezier {
            comma_separated_arguments[0][0].token().number_value(),
            comma_separated_arguments[1][0].token().number_value(),
            comma_separated_arguments[2][0].token().number_value(),
            comma_separated_arguments[3][0].token().number_value(),
        };

        if (bezier.x1 < 0.0 || bezier.x1 > 1.0 || bezier.x2 < 0.0 || bezier.x2 > 1.0)
            return nullptr;

        transaction.commit();
        return EasingStyleValue::create(bezier);
    }

    if (name.equals_ignoring_ascii_case("steps"sv)) {
        if (comma_separated_arguments.is_empty() || comma_separated_arguments.size() > 2)
            return nullptr;

        for (auto const& argument : comma_separated_arguments) {
            if (argument.size() != 1)
                return nullptr;
        }

        EasingStyleValue::Steps steps;

        auto const& intervals_argument = comma_separated_arguments[0][0];
        if (!intervals_argument.is(Token::Type::Number))
            return nullptr;
        if (!intervals_argument.token().number().is_integer())
            return nullptr;
        auto intervals = intervals_argument.token().to_integer();

        if (comma_separated_arguments.size() == 2) {
            TokenStream identifier_stream { comma_separated_arguments[1] };
            auto keyword_value = parse_keyword_value(identifier_stream);
            if (!keyword_value)
                return nullptr;
            switch (keyword_value->to_keyword()) {
            case Keyword::JumpStart:
                steps.position = EasingStyleValue::Steps::Position::JumpStart;
                break;
            case Keyword::JumpEnd:
                steps.position = EasingStyleValue::Steps::Position::JumpEnd;
                break;
            case Keyword::JumpBoth:
                steps.position = EasingStyleValue::Steps::Position::JumpBoth;
                break;
            case Keyword::JumpNone:
                steps.position = EasingStyleValue::Steps::Position::JumpNone;
                break;
            case Keyword::Start:
                steps.position = EasingStyleValue::Steps::Position::Start;
                break;
            case Keyword::End:
                steps.position = EasingStyleValue::Steps::Position::End;
                break;
            default:
                return nullptr;
            }
        }

        // Perform extra validation
        // https://drafts.csswg.org/css-easing/#step-easing-functions
        // If the <step-position> is jump-none, the <integer> must be at least 2, or the function is invalid.
        // Otherwise, the <integer> must be at least 1, or the function is invalid.
        if (steps.position == EasingStyleValue::Steps::Position::JumpNone) {
            if (intervals <= 1)
                return nullptr;
        } else if (intervals <= 0) {
            return nullptr;
        }

        steps.number_of_intervals = intervals;
        transaction.commit();
        return EasingStyleValue::create(steps);
    }

    return nullptr;
}

// https://www.w3.org/TR/css-transforms-1/#transform-property
RefPtr<CSSStyleValue> Parser::parse_transform_value(TokenStream<ComponentValue>& tokens)
{
    // <transform> = none | <transform-list>
    // <transform-list> = <transform-function>+

    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    StyleValueVector transformations;
    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto const& part = tokens.consume_a_token();
        if (!part.is_function())
            return nullptr;
        auto maybe_function = transform_function_from_string(part.function().name);
        if (!maybe_function.has_value())
            return nullptr;
        auto function = maybe_function.release_value();
        auto function_metadata = transform_function_metadata(function);

        auto function_tokens = TokenStream { part.function().value };
        auto arguments = parse_a_comma_separated_list_of_component_values(function_tokens);

        if (arguments.size() > function_metadata.parameters.size()) {
            dbgln_if(CSS_PARSER_DEBUG, "Too many arguments to {}. max: {}", part.function().name, function_metadata.parameters.size());
            return nullptr;
        }

        if (arguments.size() < function_metadata.parameters.size() && function_metadata.parameters[arguments.size()].required) {
            dbgln_if(CSS_PARSER_DEBUG, "Required parameter at position {} is missing", arguments.size());
            return nullptr;
        }

        StyleValueVector values;
        for (auto argument_index = 0u; argument_index < arguments.size(); ++argument_index) {
            TokenStream argument_tokens { arguments[argument_index] };
            argument_tokens.discard_whitespace();

            auto const& value = argument_tokens.consume_a_token();
            RefPtr<CSSMathValue> maybe_calc_value = parse_calculated_value(value);

            switch (function_metadata.parameters[argument_index].type) {
            case TransformFunctionParameterType::Angle: {
                // These are `<angle> | <zero>` in the spec, so we have to check for both kinds.
                if (maybe_calc_value && maybe_calc_value->resolves_to_angle()) {
                    values.append(maybe_calc_value.release_nonnull());
                } else if (value.is(Token::Type::Number) && value.token().number_value() == 0) {
                    values.append(AngleStyleValue::create(Angle::make_degrees(0)));
                } else {
                    // FIXME: Remove this reconsume once all parsing functions are TokenStream-based.
                    argument_tokens.reconsume_current_input_token();
                    auto dimension_value = parse_dimension_value(argument_tokens);
                    if (!dimension_value || !dimension_value->is_angle())
                        return nullptr;
                    values.append(dimension_value.release_nonnull());
                }
                break;
            }
            case TransformFunctionParameterType::Length:
            case TransformFunctionParameterType::LengthNone: {
                if (maybe_calc_value && maybe_calc_value->resolves_to_length()) {
                    argument_tokens.discard_a_token(); // calc()
                    values.append(maybe_calc_value.release_nonnull());
                } else {
                    // FIXME: Remove this reconsume once all parsing functions are TokenStream-based.
                    argument_tokens.reconsume_current_input_token();

                    if (function_metadata.parameters[argument_index].type == TransformFunctionParameterType::LengthNone) {
                        auto keyword_transaction = argument_tokens.begin_transaction();
                        auto keyword_value = parse_keyword_value(argument_tokens);
                        if (keyword_value && keyword_value->to_keyword() == Keyword::None) {
                            values.append(keyword_value.release_nonnull());
                            keyword_transaction.commit();
                            break;
                        }
                    }

                    auto dimension_value = parse_dimension_value(argument_tokens);
                    if (!dimension_value || !dimension_value->is_length())
                        return nullptr;

                    values.append(dimension_value.release_nonnull());
                }
                break;
            }
            case TransformFunctionParameterType::LengthPercentage: {
                if (maybe_calc_value && maybe_calc_value->resolves_to_length_percentage()) {
                    values.append(maybe_calc_value.release_nonnull());
                } else {
                    // FIXME: Remove this reconsume once all parsing functions are TokenStream-based.
                    argument_tokens.reconsume_current_input_token();
                    auto dimension_value = parse_dimension_value(argument_tokens);
                    if (!dimension_value)
                        return nullptr;

                    if (dimension_value->is_percentage() || dimension_value->is_length())
                        values.append(dimension_value.release_nonnull());
                    else
                        return nullptr;
                }
                break;
            }
            case TransformFunctionParameterType::Number: {
                if (maybe_calc_value && maybe_calc_value->resolves_to_number()) {
                    values.append(maybe_calc_value.release_nonnull());
                } else {
                    // FIXME: Remove this reconsume once all parsing functions are TokenStream-based.
                    argument_tokens.reconsume_current_input_token();
                    auto number = parse_number_value(argument_tokens);
                    if (!number)
                        return nullptr;
                    values.append(number.release_nonnull());
                }
                break;
            }
            case TransformFunctionParameterType::NumberPercentage: {
                if (maybe_calc_value && maybe_calc_value->resolves_to_number()) {
                    values.append(maybe_calc_value.release_nonnull());
                } else {
                    // FIXME: Remove this reconsume once all parsing functions are TokenStream-based.
                    argument_tokens.reconsume_current_input_token();
                    auto number_or_percentage = parse_number_percentage_value(argument_tokens);
                    if (!number_or_percentage)
                        return nullptr;
                    values.append(number_or_percentage.release_nonnull());
                }
                break;
            }
            }

            argument_tokens.discard_whitespace();
            if (argument_tokens.has_next_token())
                return nullptr;
        }

        transformations.append(TransformationStyleValue::create(function, move(values)));
    }
    transaction.commit();
    return StyleValueList::create(move(transformations), StyleValueList::Separator::Space);
}

// https://www.w3.org/TR/css-transforms-1/#propdef-transform-origin
// FIXME: This only supports a 2D position
RefPtr<CSSStyleValue> Parser::parse_transform_origin_value(TokenStream<ComponentValue>& tokens)
{
    enum class Axis {
        None,
        X,
        Y,
    };

    struct AxisOffset {
        Axis axis;
        NonnullRefPtr<CSSStyleValue> offset;
    };

    auto to_axis_offset = [](RefPtr<CSSStyleValue> value) -> Optional<AxisOffset> {
        if (!value)
            return OptionalNone {};
        if (value->is_percentage())
            return AxisOffset { Axis::None, value->as_percentage() };
        if (value->is_length())
            return AxisOffset { Axis::None, value->as_length() };
        if (value->is_keyword()) {
            switch (value->to_keyword()) {
            case Keyword::Top:
                return AxisOffset { Axis::Y, PercentageStyleValue::create(Percentage(0)) };
            case Keyword::Left:
                return AxisOffset { Axis::X, PercentageStyleValue::create(Percentage(0)) };
            case Keyword::Center:
                return AxisOffset { Axis::None, PercentageStyleValue::create(Percentage(50)) };
            case Keyword::Bottom:
                return AxisOffset { Axis::Y, PercentageStyleValue::create(Percentage(100)) };
            case Keyword::Right:
                return AxisOffset { Axis::X, PercentageStyleValue::create(Percentage(100)) };
            default:
                return OptionalNone {};
            }
        }
        if (value->is_math()) {
            return AxisOffset { Axis::None, value->as_math() };
        }
        return OptionalNone {};
    };

    auto transaction = tokens.begin_transaction();

    auto make_list = [&transaction](NonnullRefPtr<CSSStyleValue> const& x_value, NonnullRefPtr<CSSStyleValue> const& y_value) -> NonnullRefPtr<StyleValueList> {
        transaction.commit();
        return StyleValueList::create(StyleValueVector { x_value, y_value }, StyleValueList::Separator::Space);
    };

    switch (tokens.remaining_token_count()) {
    case 1: {
        auto single_value = to_axis_offset(parse_css_value_for_property(PropertyID::TransformOrigin, tokens));
        if (!single_value.has_value())
            return nullptr;
        // If only one value is specified, the second value is assumed to be center.
        // FIXME: If one or two values are specified, the third value is assumed to be 0px.
        switch (single_value->axis) {
        case Axis::None:
        case Axis::X:
            return make_list(single_value->offset, PercentageStyleValue::create(Percentage(50)));
        case Axis::Y:
            return make_list(PercentageStyleValue::create(Percentage(50)), single_value->offset);
        }
        VERIFY_NOT_REACHED();
    }
    case 2: {
        auto first_value = to_axis_offset(parse_css_value_for_property(PropertyID::TransformOrigin, tokens));
        auto second_value = to_axis_offset(parse_css_value_for_property(PropertyID::TransformOrigin, tokens));
        if (!first_value.has_value() || !second_value.has_value())
            return nullptr;

        RefPtr<CSSStyleValue> x_value;
        RefPtr<CSSStyleValue> y_value;

        if (first_value->axis == Axis::X) {
            x_value = first_value->offset;
        } else if (first_value->axis == Axis::Y) {
            y_value = first_value->offset;
        }

        if (second_value->axis == Axis::X) {
            if (x_value)
                return nullptr;
            x_value = second_value->offset;
            // Put the other in Y since its axis can't have been X
            y_value = first_value->offset;
        } else if (second_value->axis == Axis::Y) {
            if (y_value)
                return nullptr;
            y_value = second_value->offset;
            // Put the other in X since its axis can't have been Y
            x_value = first_value->offset;
        } else {
            if (x_value) {
                VERIFY(!y_value);
                y_value = second_value->offset;
            } else {
                VERIFY(!x_value);
                x_value = second_value->offset;
            }
        }
        // If two or more values are defined and either no value is a keyword, or the only used keyword is center,
        // then the first value represents the horizontal position (or offset) and the second represents the vertical position (or offset).
        // FIXME: A third value always represents the Z position (or offset) and must be of type <length>.
        if (first_value->axis == Axis::None && second_value->axis == Axis::None) {
            x_value = first_value->offset;
            y_value = second_value->offset;
        }
        return make_list(x_value.release_nonnull(), y_value.release_nonnull());
    }
    }

    return nullptr;
}

RefPtr<CSSStyleValue> Parser::parse_transition_value(TokenStream<ComponentValue>& tokens)
{
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return none;

    Vector<TransitionStyleValue::Transition> transitions;
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        TransitionStyleValue::Transition transition;
        auto time_value_count = 0;

        while (tokens.has_next_token() && !tokens.next_token().is(Token::Type::Comma)) {
            if (auto time = parse_time(tokens); time.has_value()) {
                switch (time_value_count) {
                case 0:
                    transition.duration = time.release_value();
                    break;
                case 1:
                    transition.delay = time.release_value();
                    break;
                default:
                    dbgln_if(CSS_PARSER_DEBUG, "Transition property has more than two time values");
                    return {};
                }
                time_value_count++;
                continue;
            }

            if (auto easing = parse_easing_value(tokens)) {
                if (transition.easing) {
                    dbgln_if(CSS_PARSER_DEBUG, "Transition property has multiple easing values");
                    return {};
                }

                transition.easing = easing->as_easing();
                continue;
            }

            if (tokens.next_token().is(Token::Type::Ident)) {
                if (transition.property_name) {
                    dbgln_if(CSS_PARSER_DEBUG, "Transition property has multiple property identifiers");
                    return {};
                }

                auto ident = tokens.consume_a_token().token().ident();
                if (auto property = property_id_from_string(ident); property.has_value())
                    transition.property_name = CustomIdentStyleValue::create(ident);

                continue;
            }

            dbgln_if(CSS_PARSER_DEBUG, "Transition property has unexpected token \"{}\"", tokens.next_token().to_string());
            return {};
        }

        if (!transition.property_name)
            transition.property_name = CustomIdentStyleValue::create("all"_fly_string);

        if (!transition.easing)
            transition.easing = EasingStyleValue::create(EasingStyleValue::CubicBezier::ease());

        transitions.append(move(transition));

        if (!tokens.next_token().is(Token::Type::Comma))
            break;

        tokens.discard_a_token();
    }

    transaction.commit();
    return TransitionStyleValue::create(move(transitions));
}

RefPtr<CSSStyleValue> Parser::parse_as_css_value(PropertyID property_id)
{
    auto component_values = parse_a_list_of_component_values(m_token_stream);
    auto tokens = TokenStream(component_values);
    auto parsed_value = parse_css_value(property_id, tokens);
    if (parsed_value.is_error())
        return nullptr;
    return parsed_value.release_value();
}

Optional<CSS::GridSize> Parser::parse_grid_size(ComponentValue const& component_value)
{
    if (component_value.is_function()) {
        if (auto maybe_calculated = parse_calculated_value(component_value)) {
            if (maybe_calculated->resolves_to_length_percentage())
                return GridSize(LengthPercentage(maybe_calculated.release_nonnull()));
            // FIXME: Support calculated <flex>
        }

        return {};
    }
    if (component_value.is_ident("auto"sv))
        return GridSize::make_auto();
    if (component_value.is_ident("max-content"sv))
        return GridSize(GridSize::Type::MaxContent);
    if (component_value.is_ident("min-content"sv))
        return GridSize(GridSize::Type::MinContent);
    auto dimension = parse_dimension(component_value);
    if (!dimension.has_value())
        return {};
    if (dimension->is_length())
        return GridSize(dimension->length());
    else if (dimension->is_percentage())
        return GridSize(dimension->percentage());
    else if (dimension->is_flex())
        return GridSize(dimension->flex());
    return {};
}

Optional<CSS::GridFitContent> Parser::parse_fit_content(Vector<ComponentValue> const& component_values)
{
    // https://www.w3.org/TR/css-grid-2/#valdef-grid-template-columns-fit-content
    // 'fit-content( <length-percentage> )'
    // Represents the formula max(minimum, min(limit, max-content)), where minimum represents an auto minimum (which is often, but not always,
    // equal to a min-content minimum), and limit is the track sizing function passed as an argument to fit-content().
    // This is essentially calculated as the smaller of minmax(auto, max-content) and minmax(auto, limit).
    auto function_tokens = TokenStream(component_values);
    function_tokens.discard_whitespace();
    auto maybe_length_percentage = parse_length_percentage(function_tokens);
    if (maybe_length_percentage.has_value())
        return CSS::GridFitContent(CSS::GridSize(CSS::GridSize::Type::FitContent, maybe_length_percentage.value()));
    return {};
}

Optional<CSS::GridMinMax> Parser::parse_min_max(Vector<ComponentValue> const& component_values)
{
    // https://www.w3.org/TR/css-grid-2/#valdef-grid-template-columns-minmax
    // 'minmax(min, max)'
    // Defines a size range greater than or equal to min and less than or equal to max. If the max is
    // less than the min, then the max will be floored by the min (essentially yielding minmax(min,
    // min)). As a maximum, a <flex> value sets the track’s flex factor; it is invalid as a minimum.
    auto function_tokens = TokenStream(component_values);
    auto comma_separated_list = parse_a_comma_separated_list_of_component_values(function_tokens);
    if (comma_separated_list.size() != 2)
        return {};

    TokenStream part_one_tokens { comma_separated_list[0] };
    part_one_tokens.discard_whitespace();
    if (!part_one_tokens.has_next_token())
        return {};
    NonnullRawPtr<ComponentValue const> current_token = part_one_tokens.consume_a_token();
    auto min_grid_size = parse_grid_size(current_token);

    TokenStream part_two_tokens { comma_separated_list[1] };
    part_two_tokens.discard_whitespace();
    if (!part_two_tokens.has_next_token())
        return {};
    current_token = part_two_tokens.consume_a_token();
    auto max_grid_size = parse_grid_size(current_token);

    if (min_grid_size.has_value() && max_grid_size.has_value()) {
        // https://www.w3.org/TR/css-grid-2/#valdef-grid-template-columns-minmax
        // As a maximum, a <flex> value sets the track’s flex factor; it is invalid as a minimum.
        if (min_grid_size.value().is_flexible_length())
            return {};
        return CSS::GridMinMax(min_grid_size.value(), max_grid_size.value());
    }
    return {};
}

Optional<CSS::GridRepeat> Parser::parse_repeat(Vector<ComponentValue> const& component_values)
{
    // https://www.w3.org/TR/css-grid-2/#repeat-syntax
    // 7.2.3.1. Syntax of repeat()
    // The generic form of the repeat() syntax is, approximately,
    // repeat( [ <integer [1,∞]> | auto-fill | auto-fit ] , <track-list> )
    auto is_auto_fill = false;
    auto is_auto_fit = false;
    auto function_tokens = TokenStream(component_values);
    auto comma_separated_list = parse_a_comma_separated_list_of_component_values(function_tokens);
    if (comma_separated_list.size() != 2)
        return {};
    // The first argument specifies the number of repetitions.
    TokenStream part_one_tokens { comma_separated_list[0] };
    part_one_tokens.discard_whitespace();
    if (!part_one_tokens.has_next_token())
        return {};
    auto& current_token = part_one_tokens.consume_a_token();

    auto repeat_count = 0;
    if (current_token.is(Token::Type::Number) && current_token.token().number().is_integer() && current_token.token().number_value() > 0)
        repeat_count = current_token.token().number_value();
    else if (current_token.is_ident("auto-fill"sv))
        is_auto_fill = true;
    else if (current_token.is_ident("auto-fit"sv))
        is_auto_fit = true;

    // The second argument is a track list, which is repeated that number of times.
    TokenStream part_two_tokens { comma_separated_list[1] };
    part_two_tokens.discard_whitespace();
    if (!part_two_tokens.has_next_token())
        return {};

    Vector<Variant<ExplicitGridTrack, GridLineNames>> repeat_params;
    auto last_object_was_line_names = false;
    while (part_two_tokens.has_next_token()) {
        auto const& token = part_two_tokens.consume_a_token();
        Vector<String> line_names;
        if (token.is_block()) {
            if (last_object_was_line_names)
                return {};
            last_object_was_line_names = true;
            if (!token.block().is_square())
                return {};
            TokenStream block_tokens { token.block().value };
            while (block_tokens.has_next_token()) {
                auto const& current_block_token = block_tokens.consume_a_token();
                line_names.append(current_block_token.token().ident().to_string());
                block_tokens.discard_whitespace();
            }
            repeat_params.append(GridLineNames { move(line_names) });
            part_two_tokens.discard_whitespace();
        } else {
            last_object_was_line_names = false;
            auto track_sizing_function = parse_track_sizing_function(token);
            if (!track_sizing_function.has_value())
                return {};
            // However, there are some restrictions:
            // The repeat() notation can’t be nested.
            if (track_sizing_function.value().is_repeat())
                return {};

            // Automatic repetitions (auto-fill or auto-fit) cannot be combined with intrinsic or flexible sizes.
            // Note that 'auto' is also an intrinsic size (and thus not permitted) but we can't use
            // track_sizing_function.is_auto(..) to check for it, as it requires AvailableSize, which is why there is
            // a separate check for it below.
            // https://www.w3.org/TR/css-grid-2/#repeat-syntax
            // https://www.w3.org/TR/css-grid-2/#intrinsic-sizing-function
            if (track_sizing_function.value().is_default()
                && (track_sizing_function.value().grid_size().is_flexible_length() || token.is_ident("auto"sv))
                && (is_auto_fill || is_auto_fit))
                return {};

            repeat_params.append(track_sizing_function.value());
            part_two_tokens.discard_whitespace();
        }
    }

    // Thus the precise syntax of the repeat() notation has several forms:
    // <track-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <track-size> ]+ <line-names>? )
    // <auto-repeat>  = repeat( [ auto-fill | auto-fit ] , [ <line-names>? <fixed-size> ]+ <line-names>? )
    // <fixed-repeat> = repeat( [ <integer [1,∞]> ] , [ <line-names>? <fixed-size> ]+ <line-names>? )
    // <name-repeat>  = repeat( [ <integer [1,∞]> | auto-fill ], <line-names>+)

    // The <track-repeat> variant can represent the repetition of any <track-size>, but is limited to a
    // fixed number of repetitions.

    // The <auto-repeat> variant can repeat automatically to fill a space, but requires definite track
    // sizes so that the number of repetitions can be calculated. It can only appear once in the track
    // list, but the same track list can also contain <fixed-repeat>s.

    // The <name-repeat> variant is for adding line names to subgrids. It can only be used with the
    // subgrid keyword and cannot specify track sizes, only line names.

    // If a repeat() function that is not a <name-repeat> ends up placing two <line-names> adjacent to
    // each other, the name lists are merged. For example, repeat(2, [a] 1fr [b]) is equivalent to [a]
    // 1fr [b a] 1fr [b].
    if (is_auto_fill)
        return GridRepeat(GridTrackSizeList(move(repeat_params)), GridRepeat::Type::AutoFill);
    else if (is_auto_fit)
        return GridRepeat(GridTrackSizeList(move(repeat_params)), GridRepeat::Type::AutoFit);
    else
        return GridRepeat(GridTrackSizeList(move(repeat_params)), repeat_count);
}

Optional<CSS::ExplicitGridTrack> Parser::parse_track_sizing_function(ComponentValue const& token)
{
    if (token.is_function()) {
        auto const& function_token = token.function();
        if (function_token.name.equals_ignoring_ascii_case("repeat"sv)) {
            auto maybe_repeat = parse_repeat(function_token.value);
            if (maybe_repeat.has_value())
                return CSS::ExplicitGridTrack(maybe_repeat.value());
            else
                return {};
        } else if (function_token.name.equals_ignoring_ascii_case("minmax"sv)) {
            auto maybe_min_max_value = parse_min_max(function_token.value);
            if (maybe_min_max_value.has_value())
                return CSS::ExplicitGridTrack(maybe_min_max_value.value());
            else
                return {};
        } else if (function_token.name.equals_ignoring_ascii_case("fit-content"sv)) {
            auto maybe_fit_content_value = parse_fit_content(function_token.value);
            if (maybe_fit_content_value.has_value())
                return CSS::ExplicitGridTrack(maybe_fit_content_value.value());
            return {};
        } else if (auto maybe_calculated = parse_calculated_value(token)) {
            return CSS::ExplicitGridTrack(GridSize(LengthPercentage(maybe_calculated.release_nonnull())));
        }
        return {};
    } else if (token.is_ident("auto"sv)) {
        return CSS::ExplicitGridTrack(GridSize(Length::make_auto()));
    } else if (token.is_block()) {
        return {};
    } else {
        auto grid_size = parse_grid_size(token);
        if (!grid_size.has_value())
            return {};
        return CSS::ExplicitGridTrack(grid_size.value());
    }
}

RefPtr<CSSStyleValue> Parser::parse_grid_track_size_list(TokenStream<ComponentValue>& tokens, bool allow_separate_line_name_blocks)
{
    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return GridTrackSizeListStyleValue::make_none();

    auto transaction = tokens.begin_transaction();

    Vector<Variant<ExplicitGridTrack, GridLineNames>> track_list;
    auto last_object_was_line_names = false;
    while (tokens.has_next_token()) {
        auto const& token = tokens.consume_a_token();
        if (token.is_block()) {
            if (last_object_was_line_names && !allow_separate_line_name_blocks) {
                transaction.commit();
                return GridTrackSizeListStyleValue::make_auto();
            }
            last_object_was_line_names = true;
            Vector<String> line_names;
            if (!token.block().is_square()) {
                transaction.commit();
                return GridTrackSizeListStyleValue::make_auto();
            }
            TokenStream block_tokens { token.block().value };
            block_tokens.discard_whitespace();
            while (block_tokens.has_next_token()) {
                auto const& current_block_token = block_tokens.consume_a_token();
                line_names.append(current_block_token.token().ident().to_string());
                block_tokens.discard_whitespace();
            }
            track_list.append(GridLineNames { move(line_names) });
        } else {
            last_object_was_line_names = false;
            auto track_sizing_function = parse_track_sizing_function(token);
            if (!track_sizing_function.has_value()) {
                transaction.commit();
                return GridTrackSizeListStyleValue::make_auto();
            }
            // FIXME: Handle multiple repeat values (should combine them here, or remove
            // any other ones if the first one is auto-fill, etc.)
            track_list.append(track_sizing_function.value());
        }
    }

    transaction.commit();
    return GridTrackSizeListStyleValue::create(GridTrackSizeList(move(track_list)));
}

// https://www.w3.org/TR/css-grid-1/#grid-auto-flow-property
RefPtr<GridAutoFlowStyleValue> Parser::parse_grid_auto_flow_value(TokenStream<ComponentValue>& tokens)
{
    // [ row | column ] || dense
    if (!tokens.has_next_token())
        return nullptr;

    auto transaction = tokens.begin_transaction();

    auto parse_axis = [&]() -> Optional<GridAutoFlowStyleValue::Axis> {
        auto transaction = tokens.begin_transaction();
        auto const& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};
        auto const& ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("row"sv)) {
            transaction.commit();
            return GridAutoFlowStyleValue::Axis::Row;
        } else if (ident.equals_ignoring_ascii_case("column"sv)) {
            transaction.commit();
            return GridAutoFlowStyleValue::Axis::Column;
        }
        return {};
    };

    auto parse_dense = [&]() -> Optional<GridAutoFlowStyleValue::Dense> {
        auto transaction = tokens.begin_transaction();
        auto const& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};
        auto const& ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("dense"sv)) {
            transaction.commit();
            return GridAutoFlowStyleValue::Dense::Yes;
        }
        return {};
    };

    Optional<GridAutoFlowStyleValue::Axis> axis;
    Optional<GridAutoFlowStyleValue::Dense> dense;
    if (axis = parse_axis(); axis.has_value()) {
        dense = parse_dense();
    } else if (dense = parse_dense(); dense.has_value()) {
        axis = parse_axis();
    }

    if (tokens.has_next_token())
        return nullptr;

    transaction.commit();
    return GridAutoFlowStyleValue::create(axis.value_or(GridAutoFlowStyleValue::Axis::Row), dense.value_or(GridAutoFlowStyleValue::Dense::No));
}

// https://drafts.csswg.org/css-overflow/#propdef-scrollbar-gutter
RefPtr<CSSStyleValue> Parser::parse_scrollbar_gutter_value(TokenStream<ComponentValue>& tokens)
{
    // auto | stable && both-edges?
    if (!tokens.has_next_token())
        return nullptr;

    auto transaction = tokens.begin_transaction();

    auto parse_stable = [&]() -> Optional<bool> {
        auto transaction = tokens.begin_transaction();
        auto const& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};
        auto const& ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("auto"sv)) {
            transaction.commit();
            return false;
        } else if (ident.equals_ignoring_ascii_case("stable"sv)) {
            transaction.commit();
            return true;
        }
        return {};
    };

    auto parse_both_edges = [&]() -> Optional<bool> {
        auto transaction = tokens.begin_transaction();
        auto const& token = tokens.consume_a_token();
        if (!token.is(Token::Type::Ident))
            return {};
        auto const& ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("both-edges"sv)) {
            transaction.commit();
            return true;
        }
        return {};
    };

    Optional<bool> stable;
    Optional<bool> both_edges;
    if (stable = parse_stable(); stable.has_value()) {
        if (stable.value())
            both_edges = parse_both_edges();
    } else if (both_edges = parse_both_edges(); both_edges.has_value()) {
        stable = parse_stable();
        if (!stable.has_value() || !stable.value())
            return nullptr;
    }

    if (tokens.has_next_token())
        return nullptr;

    transaction.commit();

    ScrollbarGutter gutter_value;
    if (both_edges.has_value())
        gutter_value = ScrollbarGutter::BothEdges;
    else if (stable.has_value() && stable.value())
        gutter_value = ScrollbarGutter::Stable;
    else
        gutter_value = ScrollbarGutter::Auto;
    return ScrollbarGutterStyleValue::create(gutter_value);
}

RefPtr<CSSStyleValue> Parser::parse_grid_auto_track_sizes(TokenStream<ComponentValue>& tokens)
{
    // https://www.w3.org/TR/css-grid-2/#auto-tracks
    // <track-size>+
    Vector<Variant<ExplicitGridTrack, GridLineNames>> track_list;
    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto const& token = tokens.consume_a_token();
        auto track_sizing_function = parse_track_sizing_function(token);
        if (!track_sizing_function.has_value()) {
            transaction.commit();
            return GridTrackSizeListStyleValue::make_auto();
        }
        // FIXME: Handle multiple repeat values (should combine them here, or remove
        //        any other ones if the first one is auto-fill, etc.)
        track_list.append(track_sizing_function.value());
    }
    transaction.commit();
    return GridTrackSizeListStyleValue::create(GridTrackSizeList(move(track_list)));
}

RefPtr<GridTrackPlacementStyleValue> Parser::parse_grid_track_placement(TokenStream<ComponentValue>& tokens)
{
    // FIXME: This shouldn't be needed. Right now, the below code returns a CSSStyleValue even if no tokens are consumed!
    if (!tokens.has_next_token())
        return nullptr;

    // https://www.w3.org/TR/css-grid-2/#line-placement
    // Line-based Placement: the grid-row-start, grid-column-start, grid-row-end, and grid-column-end properties
    // <grid-line> =
    //     auto |
    //     <custom-ident> |
    //     [ <integer> && <custom-ident>? ] |
    //     [ span && [ <integer> || <custom-ident> ] ]
    auto is_valid_integer = [](auto& token) -> bool {
        // An <integer> value of zero makes the declaration invalid.
        if (token.is(Token::Type::Number) && token.token().number().is_integer() && token.token().number_value() != 0)
            return true;
        return false;
    };
    auto parse_custom_ident = [this](auto& tokens) {
        // The <custom-ident> additionally excludes the keywords span and auto.
        return parse_custom_ident_value(tokens, { "span"sv, "auto"sv });
    };

    auto transaction = tokens.begin_transaction();

    // FIXME: Handle the single-token case inside the loop instead, so that we can more easily call this from
    //        `parse_grid_area_shorthand_value()` using a single TokenStream.
    if (tokens.remaining_token_count() == 1) {
        if (auto custom_ident = parse_custom_ident(tokens)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line({}, custom_ident->custom_ident().to_string()));
        }
        auto const& token = tokens.consume_a_token();
        if (auto maybe_calculated = parse_calculated_value(token); maybe_calculated && maybe_calculated->resolves_to_number()) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(static_cast<int>(maybe_calculated->resolve_integer().value()), {}));
        }
        if (token.is_ident("auto"sv)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_auto());
        }
        if (token.is_ident("span"sv)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_span(1));
        }
        if (is_valid_integer(token)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(static_cast<int>(token.token().number_value()), {}));
        }
        return nullptr;
    }

    auto span_value = false;
    auto span_or_position_value = 0;
    String identifier_value;
    while (tokens.has_next_token()) {
        auto const& token = tokens.next_token();
        if (token.is_ident("auto"sv))
            return nullptr;
        if (token.is_ident("span"sv)) {
            if (span_value)
                return nullptr;
            tokens.discard_a_token(); // span
            span_value = true;
            continue;
        }
        if (is_valid_integer(token)) {
            if (span_or_position_value != 0)
                return nullptr;
            span_or_position_value = static_cast<int>(tokens.consume_a_token().token().to_integer());
            continue;
        }
        if (auto custom_ident = parse_custom_ident(tokens)) {
            if (!identifier_value.is_empty())
                return nullptr;
            identifier_value = custom_ident->custom_ident().to_string();
            continue;
        }
        break;
    }

    // Negative integers or zero are invalid.
    if (span_value && span_or_position_value < 1)
        return nullptr;

    // If the <integer> is omitted, it defaults to 1.
    if (span_or_position_value == 0)
        span_or_position_value = 1;

    transaction.commit();
    if (!identifier_value.is_empty())
        return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line(span_or_position_value, identifier_value));
    return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_span(span_or_position_value));
}

RefPtr<CSSStyleValue> Parser::parse_grid_track_placement_shorthand_value(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    auto start_property = (property_id == PropertyID::GridColumn) ? PropertyID::GridColumnStart : PropertyID::GridRowStart;
    auto end_property = (property_id == PropertyID::GridColumn) ? PropertyID::GridColumnEnd : PropertyID::GridRowEnd;

    auto transaction = tokens.begin_transaction();
    NonnullRawPtr<ComponentValue const> current_token = tokens.consume_a_token();

    Vector<ComponentValue> track_start_placement_tokens;
    while (true) {
        if (current_token->is_delim('/'))
            break;
        track_start_placement_tokens.append(current_token);
        if (!tokens.has_next_token())
            break;
        current_token = tokens.consume_a_token();
    }

    Vector<ComponentValue> track_end_placement_tokens;
    if (tokens.has_next_token()) {
        current_token = tokens.consume_a_token();
        while (true) {
            track_end_placement_tokens.append(current_token);
            if (!tokens.has_next_token())
                break;
            current_token = tokens.consume_a_token();
        }
    }

    TokenStream track_start_placement_token_stream { track_start_placement_tokens };
    auto parsed_start_value = parse_grid_track_placement(track_start_placement_token_stream);
    if (parsed_start_value && track_end_placement_tokens.is_empty()) {
        transaction.commit();
        if (parsed_start_value->grid_track_placement().has_identifier()) {
            auto custom_ident = parsed_start_value.release_nonnull();
            return ShorthandStyleValue::create(property_id, { start_property, end_property }, { custom_ident, custom_ident });
        }
        return ShorthandStyleValue::create(property_id,
            { start_property, end_property },
            { parsed_start_value.release_nonnull(), GridTrackPlacementStyleValue::create(GridTrackPlacement::make_auto()) });
    }

    TokenStream track_end_placement_token_stream { track_end_placement_tokens };
    auto parsed_end_value = parse_grid_track_placement(track_end_placement_token_stream);
    if (parsed_start_value && parsed_end_value) {
        transaction.commit();
        return ShorthandStyleValue::create(property_id,
            { start_property, end_property },
            { parsed_start_value.release_nonnull(), parsed_end_value.release_nonnull() });
    }

    return nullptr;
}

// https://www.w3.org/TR/css-grid-2/#explicit-grid-shorthand
// 7.4. Explicit Grid Shorthand: the grid-template property
RefPtr<CSSStyleValue> Parser::parse_grid_track_size_list_shorthand_value(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    // The grid-template property is a shorthand for setting grid-template-columns, grid-template-rows,
    // and grid-template-areas in a single declaration. It has several distinct syntax forms:
    // none
    //    - Sets all three properties to their initial values (none).
    // <'grid-template-rows'> / <'grid-template-columns'>
    //    - Sets grid-template-rows and grid-template-columns to the specified values, respectively, and sets grid-template-areas to none.
    // [ <line-names>? <string> <track-size>? <line-names>? ]+ [ / <explicit-track-list> ]?
    //    - Sets grid-template-areas to the strings listed.
    //    - Sets grid-template-rows to the <track-size>s following each string (filling in auto for any missing sizes),
    //      and splicing in the named lines defined before/after each size.
    //    - Sets grid-template-columns to the track listing specified after the slash (or none, if not specified).
    auto transaction = tokens.begin_transaction();

    // FIXME: Read the parts in place if possible, instead of constructing separate vectors and streams.
    Vector<ComponentValue> template_rows_tokens;
    Vector<ComponentValue> template_columns_tokens;
    Vector<ComponentValue> template_area_tokens;

    bool found_forward_slash = false;

    while (tokens.has_next_token()) {
        auto& token = tokens.consume_a_token();
        if (token.is_delim('/')) {
            if (found_forward_slash)
                return nullptr;
            found_forward_slash = true;
            continue;
        }
        if (found_forward_slash) {
            template_columns_tokens.append(token);
            continue;
        }
        if (token.is(Token::Type::String))
            template_area_tokens.append(token);
        else
            template_rows_tokens.append(token);
    }

    TokenStream template_area_token_stream { template_area_tokens };
    TokenStream template_rows_token_stream { template_rows_tokens };
    TokenStream template_columns_token_stream { template_columns_tokens };
    auto parsed_template_areas_values = parse_grid_template_areas_value(template_area_token_stream);
    auto parsed_template_rows_values = parse_grid_track_size_list(template_rows_token_stream, true);
    auto parsed_template_columns_values = parse_grid_track_size_list(template_columns_token_stream);

    if (template_area_token_stream.has_next_token()
        || template_rows_token_stream.has_next_token()
        || template_columns_token_stream.has_next_token())
        return nullptr;

    transaction.commit();
    return ShorthandStyleValue::create(property_id,
        { PropertyID::GridTemplateAreas, PropertyID::GridTemplateRows, PropertyID::GridTemplateColumns },
        { parsed_template_areas_values.release_nonnull(), parsed_template_rows_values.release_nonnull(), parsed_template_columns_values.release_nonnull() });
}

RefPtr<CSSStyleValue> Parser::parse_grid_area_shorthand_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto parse_placement_tokens = [&](Vector<ComponentValue>& placement_tokens, bool check_for_delimiter = true) -> void {
        while (tokens.has_next_token()) {
            auto& current_token = tokens.consume_a_token();
            if (check_for_delimiter && current_token.is_delim('/'))
                break;
            placement_tokens.append(current_token);
        }
    };

    Vector<ComponentValue> row_start_placement_tokens;
    parse_placement_tokens(row_start_placement_tokens);

    Vector<ComponentValue> column_start_placement_tokens;
    if (tokens.has_next_token())
        parse_placement_tokens(column_start_placement_tokens);

    Vector<ComponentValue> row_end_placement_tokens;
    if (tokens.has_next_token())
        parse_placement_tokens(row_end_placement_tokens);

    Vector<ComponentValue> column_end_placement_tokens;
    if (tokens.has_next_token())
        parse_placement_tokens(column_end_placement_tokens, false);

    // https://www.w3.org/TR/css-grid-2/#placement-shorthands
    // The grid-area property is a shorthand for grid-row-start, grid-column-start, grid-row-end and
    // grid-column-end.
    TokenStream row_start_placement_token_stream { row_start_placement_tokens };
    auto row_start_style_value = parse_grid_track_placement(row_start_placement_token_stream);
    if (row_start_placement_token_stream.has_next_token())
        return nullptr;

    TokenStream column_start_placement_token_stream { column_start_placement_tokens };
    auto column_start_style_value = parse_grid_track_placement(column_start_placement_token_stream);
    if (column_start_placement_token_stream.has_next_token())
        return nullptr;

    TokenStream row_end_placement_token_stream { row_end_placement_tokens };
    auto row_end_style_value = parse_grid_track_placement(row_end_placement_token_stream);
    if (row_end_placement_token_stream.has_next_token())
        return nullptr;

    TokenStream column_end_placement_token_stream { column_end_placement_tokens };
    auto column_end_style_value = parse_grid_track_placement(column_end_placement_token_stream);
    if (column_end_placement_token_stream.has_next_token())
        return nullptr;

    // If four <grid-line> values are specified, grid-row-start is set to the first value, grid-column-start
    // is set to the second value, grid-row-end is set to the third value, and grid-column-end is set to the
    // fourth value.
    auto row_start = GridTrackPlacement::make_auto();
    auto column_start = GridTrackPlacement::make_auto();
    auto row_end = GridTrackPlacement::make_auto();
    auto column_end = GridTrackPlacement::make_auto();

    if (row_start_style_value)
        row_start = row_start_style_value.release_nonnull()->as_grid_track_placement().grid_track_placement();

    // When grid-column-start is omitted, if grid-row-start is a <custom-ident>, all four longhands are set to
    // that value. Otherwise, it is set to auto.
    if (column_start_style_value)
        column_start = column_start_style_value.release_nonnull()->as_grid_track_placement().grid_track_placement();
    else
        column_start = row_start;

    // When grid-row-end is omitted, if grid-row-start is a <custom-ident>, grid-row-end is set to that
    // <custom-ident>; otherwise, it is set to auto.
    if (row_end_style_value)
        row_end = row_end_style_value.release_nonnull()->as_grid_track_placement().grid_track_placement();
    else
        row_end = column_start;

    // When grid-column-end is omitted, if grid-column-start is a <custom-ident>, grid-column-end is set to
    // that <custom-ident>; otherwise, it is set to auto.
    if (column_end_style_value)
        column_end = column_end_style_value.release_nonnull()->as_grid_track_placement().grid_track_placement();
    else
        column_end = row_end;

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::GridArea,
        { PropertyID::GridRowStart, PropertyID::GridColumnStart, PropertyID::GridRowEnd, PropertyID::GridColumnEnd },
        { GridTrackPlacementStyleValue::create(row_start), GridTrackPlacementStyleValue::create(column_start), GridTrackPlacementStyleValue::create(row_end), GridTrackPlacementStyleValue::create(column_end) });
}

RefPtr<CSSStyleValue> Parser::parse_grid_shorthand_value(TokenStream<ComponentValue>& tokens)
{
    // <'grid-template'> |
    // FIXME: <'grid-template-rows'> / [ auto-flow && dense? ] <'grid-auto-columns'>? |
    // FIXME: [ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>
    return parse_grid_track_size_list_shorthand_value(PropertyID::Grid, tokens);
}

// https://www.w3.org/TR/css-grid-1/#grid-template-areas-property
RefPtr<CSSStyleValue> Parser::parse_grid_template_areas_value(TokenStream<ComponentValue>& tokens)
{
    // none | <string>+
    Vector<Vector<String>> grid_area_rows;

    if (auto none = parse_all_as_single_keyword_value(tokens, Keyword::None))
        return GridTemplateAreaStyleValue::create(move(grid_area_rows));

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token() && tokens.next_token().is(Token::Type::String)) {
        Vector<String> grid_area_columns;
        auto const parts = MUST(tokens.consume_a_token().token().string().to_string().split(' '));
        for (auto& part : parts) {
            grid_area_columns.append(part);
        }
        grid_area_rows.append(move(grid_area_columns));
    }
    transaction.commit();
    return GridTemplateAreaStyleValue::create(grid_area_rows);
}

static bool block_contains_var_or_attr(SimpleBlock const& block);

static bool function_contains_var_or_attr(Function const& function)
{
    if (function.name.equals_ignoring_ascii_case("var"sv) || function.name.equals_ignoring_ascii_case("attr"sv))
        return true;
    for (auto const& token : function.value) {
        if (token.is_function() && function_contains_var_or_attr(token.function()))
            return true;
        if (token.is_block() && block_contains_var_or_attr(token.block()))
            return true;
    }
    return false;
}

bool block_contains_var_or_attr(SimpleBlock const& block)
{
    for (auto const& token : block.value) {
        if (token.is_function() && function_contains_var_or_attr(token.function()))
            return true;
        if (token.is_block() && block_contains_var_or_attr(token.block()))
            return true;
    }
    return false;
}

Parser::ParseErrorOr<NonnullRefPtr<CSSStyleValue>> Parser::parse_css_value(PropertyID property_id, TokenStream<ComponentValue>& unprocessed_tokens, Optional<String> original_source_text)
{
    m_context.set_current_property_id(property_id);
    Vector<ComponentValue> component_values;
    bool contains_var_or_attr = false;
    bool const property_accepts_custom_ident = property_accepts_type(property_id, ValueType::CustomIdent);

    while (unprocessed_tokens.has_next_token()) {
        auto const& token = unprocessed_tokens.consume_a_token();

        if (token.is(Token::Type::Semicolon)) {
            unprocessed_tokens.reconsume_current_input_token();
            break;
        }

        if (property_id != PropertyID::Custom) {
            if (token.is(Token::Type::Whitespace))
                continue;

            if (!property_accepts_custom_ident && token.is(Token::Type::Ident) && has_ignored_vendor_prefix(token.token().ident()))
                return ParseError::IncludesIgnoredVendorPrefix;
        }

        if (!contains_var_or_attr) {
            if (token.is_function() && function_contains_var_or_attr(token.function()))
                contains_var_or_attr = true;
            else if (token.is_block() && block_contains_var_or_attr(token.block()))
                contains_var_or_attr = true;
        }

        component_values.append(token);
    }

    if (property_id == PropertyID::Custom || contains_var_or_attr)
        return UnresolvedStyleValue::create(move(component_values), contains_var_or_attr, original_source_text);

    if (component_values.is_empty())
        return ParseError::SyntaxError;

    auto tokens = TokenStream { component_values };

    if (component_values.size() == 1) {
        if (auto parsed_value = parse_builtin_value(tokens))
            return parsed_value.release_nonnull();
    }

    // Special-case property handling
    switch (property_id) {
    case PropertyID::AspectRatio:
        if (auto parsed_value = parse_aspect_ratio_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackdropFilter:
    case PropertyID::Filter:
        if (auto parsed_value = parse_filter_value_list_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Background:
        if (auto parsed_value = parse_background_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackgroundAttachment:
    case PropertyID::BackgroundClip:
    case PropertyID::BackgroundImage:
    case PropertyID::BackgroundOrigin:
        if (auto parsed_value = parse_simple_comma_separated_value_list(property_id, tokens))
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackgroundPosition:
        if (auto parsed_value = parse_comma_separated_value_list(tokens, [this](auto& tokens) { return parse_position_value(tokens, PositionParsingMode::BackgroundPosition); }))
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackgroundPositionX:
    case PropertyID::BackgroundPositionY:
        if (auto parsed_value = parse_comma_separated_value_list(tokens, [this, property_id](auto& tokens) { return parse_single_background_position_x_or_y_value(tokens, property_id); }))
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackgroundRepeat:
        if (auto parsed_value = parse_comma_separated_value_list(tokens, [this](auto& tokens) { return parse_single_background_repeat_value(tokens); }))
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackgroundSize:
        if (auto parsed_value = parse_comma_separated_value_list(tokens, [this](auto& tokens) { return parse_single_background_size_value(tokens); }))
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Border:
    case PropertyID::BorderBottom:
    case PropertyID::BorderLeft:
    case PropertyID::BorderRight:
    case PropertyID::BorderTop:
        if (auto parsed_value = parse_border_value(property_id, tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BorderTopLeftRadius:
    case PropertyID::BorderTopRightRadius:
    case PropertyID::BorderBottomRightRadius:
    case PropertyID::BorderBottomLeftRadius:
        if (auto parsed_value = parse_border_radius_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BorderRadius:
        if (auto parsed_value = parse_border_radius_shorthand_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BoxShadow:
        if (auto parsed_value = parse_shadow_value(tokens, AllowInsetKeyword::Yes); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Columns:
        if (auto parsed_value = parse_columns_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Content:
        if (auto parsed_value = parse_content_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::CounterIncrement:
        if (auto parsed_value = parse_counter_increment_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::CounterReset:
        if (auto parsed_value = parse_counter_reset_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::CounterSet:
        if (auto parsed_value = parse_counter_set_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Display:
        if (auto parsed_value = parse_display_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Flex:
        if (auto parsed_value = parse_flex_shorthand_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::FlexFlow:
        if (auto parsed_value = parse_flex_flow_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Font:
        if (auto parsed_value = parse_font_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::FontFamily:
        if (auto parsed_value = parse_font_family_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::FontFeatureSettings:
        if (auto parsed_value = parse_font_feature_settings_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::FontLanguageOverride:
        if (auto parsed_value = parse_font_language_override_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::FontVariationSettings:
        if (auto parsed_value = parse_font_variation_settings_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridArea:
        if (auto parsed_value = parse_grid_area_shorthand_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridAutoFlow:
        if (auto parsed_value = parse_grid_auto_flow_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridColumn:
        if (auto parsed_value = parse_grid_track_placement_shorthand_value(property_id, tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridColumnEnd:
        if (auto parsed_value = parse_grid_track_placement(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridColumnStart:
        if (auto parsed_value = parse_grid_track_placement(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridRow:
        if (auto parsed_value = parse_grid_track_placement_shorthand_value(property_id, tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridRowEnd:
        if (auto parsed_value = parse_grid_track_placement(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridRowStart:
        if (auto parsed_value = parse_grid_track_placement(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Grid:
        if (auto parsed_value = parse_grid_shorthand_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridTemplate:
        if (auto parsed_value = parse_grid_track_size_list_shorthand_value(property_id, tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridTemplateAreas:
        if (auto parsed_value = parse_grid_template_areas_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridTemplateColumns:
        if (auto parsed_value = parse_grid_track_size_list(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridTemplateRows:
        if (auto parsed_value = parse_grid_track_size_list(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridAutoColumns:
        if (auto parsed_value = parse_grid_auto_track_sizes(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::GridAutoRows:
        if (auto parsed_value = parse_grid_auto_track_sizes(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::ListStyle:
        if (auto parsed_value = parse_list_style_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::MathDepth:
        if (auto parsed_value = parse_math_depth_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Overflow:
        if (auto parsed_value = parse_overflow_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::PlaceContent:
        if (auto parsed_value = parse_place_content_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::PlaceItems:
        if (auto parsed_value = parse_place_items_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::PlaceSelf:
        if (auto parsed_value = parse_place_self_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Quotes:
        if (auto parsed_value = parse_quotes_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Rotate:
        if (auto parsed_value = parse_rotate_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::ScrollbarGutter:
        if (auto parsed_value = parse_scrollbar_gutter_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::TextDecoration:
        if (auto parsed_value = parse_text_decoration_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::TextDecorationLine:
        if (auto parsed_value = parse_text_decoration_line_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::TextShadow:
        if (auto parsed_value = parse_shadow_value(tokens, AllowInsetKeyword::No); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Transform:
        if (auto parsed_value = parse_transform_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::TransformOrigin:
        if (auto parsed_value = parse_transform_origin_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Transition:
        if (auto parsed_value = parse_transition_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    default:
        break;
    }

    // If there's only 1 ComponentValue, we can only produce a single CSSStyleValue.
    if (component_values.size() == 1) {
        auto stream = TokenStream { component_values };
        if (auto parsed_value = parse_css_value_for_property(property_id, stream))
            return parsed_value.release_nonnull();
    } else {
        StyleValueVector parsed_values;
        auto stream = TokenStream { component_values };
        while (auto parsed_value = parse_css_value_for_property(property_id, stream)) {
            parsed_values.append(parsed_value.release_nonnull());
            if (!stream.has_next_token())
                break;
        }

        // Some types (such as <ratio>) can be made from multiple ComponentValues, so if we only made 1 CSSStyleValue, return it directly.
        if (parsed_values.size() == 1)
            return *parsed_values.take_first();

        if (!parsed_values.is_empty() && parsed_values.size() <= property_maximum_value_count(property_id))
            return StyleValueList::create(move(parsed_values), StyleValueList::Separator::Space);
    }

    // We have multiple values, but the property claims to accept only a single one, check if it's a shorthand property.
    auto unassigned_properties = longhands_for_shorthand(property_id);
    if (unassigned_properties.is_empty())
        return ParseError::SyntaxError;

    auto stream = TokenStream { component_values };

    HashMap<UnderlyingType<PropertyID>, Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>>> assigned_values;

    while (stream.has_next_token() && !unassigned_properties.is_empty()) {
        auto property_and_value = parse_css_value_for_properties(unassigned_properties, stream);
        if (property_and_value.has_value()) {
            auto property = property_and_value->property;
            auto value = property_and_value->style_value;
            auto& values = assigned_values.ensure(to_underlying(property));
            if (values.size() + 1 == property_maximum_value_count(property)) {
                // We're done with this property, move on to the next one.
                unassigned_properties.remove_first_matching([&](auto& unassigned_property) { return unassigned_property == property; });
            }

            values.append(value.release_nonnull());
            continue;
        }

        // No property matched, so we're done.
        dbgln("No property (from {} properties) matched {}", unassigned_properties.size(), stream.next_token().to_debug_string());
        for (auto id : unassigned_properties)
            dbgln("    {}", string_from_property_id(id));
        break;
    }

    for (auto& property : unassigned_properties)
        assigned_values.ensure(to_underlying(property)).append(property_initial_value(m_context.realm(), property));

    stream.discard_whitespace();
    if (stream.has_next_token())
        return ParseError::SyntaxError;

    Vector<PropertyID> longhand_properties;
    longhand_properties.ensure_capacity(assigned_values.size());
    for (auto& it : assigned_values)
        longhand_properties.unchecked_append(static_cast<PropertyID>(it.key));

    StyleValueVector longhand_values;
    longhand_values.ensure_capacity(assigned_values.size());
    for (auto& it : assigned_values) {
        if (it.value.size() == 1)
            longhand_values.unchecked_append(it.value.take_first());
        else
            longhand_values.unchecked_append(StyleValueList::create(move(it.value), StyleValueList::Separator::Space));
    }

    return { ShorthandStyleValue::create(property_id, move(longhand_properties), move(longhand_values)) };
}

RefPtr<CSSStyleValue> Parser::parse_css_value_for_property(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    return parse_css_value_for_properties({ &property_id, 1 }, tokens)
        .map([](auto& it) { return it.style_value; })
        .value_or(nullptr);
}

Optional<Parser::PropertyAndValue> Parser::parse_css_value_for_properties(ReadonlySpan<PropertyID> property_ids, TokenStream<ComponentValue>& tokens)
{
    auto any_property_accepts_type = [](ReadonlySpan<PropertyID> property_ids, ValueType value_type) -> Optional<PropertyID> {
        for (auto const& property : property_ids) {
            if (property_accepts_type(property, value_type))
                return property;
        }
        return {};
    };
    auto any_property_accepts_type_percentage = [](ReadonlySpan<PropertyID> property_ids, ValueType value_type) -> Optional<PropertyID> {
        for (auto const& property : property_ids) {
            if (property_accepts_type(property, value_type) && property_accepts_type(property, ValueType::Percentage))
                return property;
        }
        return {};
    };
    auto any_property_accepts_keyword = [](ReadonlySpan<PropertyID> property_ids, Keyword keyword) -> Optional<PropertyID> {
        for (auto const& property : property_ids) {
            if (property_accepts_keyword(property, keyword))
                return property;
        }
        return {};
    };

    auto& peek_token = tokens.next_token();

    if (auto property = any_property_accepts_type(property_ids, ValueType::EasingFunction); property.has_value()) {
        if (auto maybe_easing_function = parse_easing_value(tokens))
            return PropertyAndValue { *property, maybe_easing_function };
    }

    if (peek_token.is(Token::Type::Ident)) {
        // NOTE: We do not try to parse "CSS-wide keywords" here. https://www.w3.org/TR/css-values-4/#common-keywords
        //       These are only valid on their own, and so should be parsed directly in `parse_css_value()`.
        auto keyword = keyword_from_string(peek_token.token().ident());
        if (keyword.has_value()) {
            if (auto property = any_property_accepts_keyword(property_ids, keyword.value()); property.has_value()) {
                tokens.discard_a_token();
                return PropertyAndValue { *property, CSSKeywordValue::create(keyword.value()) };
            }
        }

        // Custom idents
        if (auto property = any_property_accepts_type(property_ids, ValueType::CustomIdent); property.has_value()) {
            if (auto custom_ident = parse_custom_ident_value(tokens, {}))
                return PropertyAndValue { *property, custom_ident };
        }
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Color); property.has_value()) {
        if (auto maybe_color = parse_color_value(tokens))
            return PropertyAndValue { *property, maybe_color };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Counter); property.has_value()) {
        if (auto maybe_counter = parse_counter_value(tokens))
            return PropertyAndValue { *property, maybe_counter };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Image); property.has_value()) {
        if (auto maybe_image = parse_image_value(tokens))
            return PropertyAndValue { *property, maybe_image };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Position); property.has_value()) {
        if (auto maybe_position = parse_position_value(tokens))
            return PropertyAndValue { *property, maybe_position };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::BackgroundPosition); property.has_value()) {
        if (auto maybe_position = parse_position_value(tokens, PositionParsingMode::BackgroundPosition))
            return PropertyAndValue { *property, maybe_position };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::BasicShape); property.has_value()) {
        if (auto maybe_basic_shape = parse_basic_shape_value(tokens))
            return PropertyAndValue { *property, maybe_basic_shape };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Ratio); property.has_value()) {
        if (auto maybe_ratio = parse_ratio_value(tokens))
            return PropertyAndValue { *property, maybe_ratio };
    }

    auto property_accepting_integer = any_property_accepts_type(property_ids, ValueType::Integer);
    auto property_accepting_number = any_property_accepts_type(property_ids, ValueType::Number);
    bool property_accepts_numeric = property_accepting_integer.has_value() || property_accepting_number.has_value();

    if (peek_token.is(Token::Type::Number) && property_accepts_numeric) {
        if (peek_token.token().number().is_integer() && property_accepting_integer.has_value()) {
            auto integer = IntegerStyleValue::create(peek_token.token().number().integer_value());
            if (property_accepts_integer(*property_accepting_integer, integer->as_integer().integer())) {
                tokens.discard_a_token(); // integer
                return PropertyAndValue { *property_accepting_integer, integer };
            }
        }
        if (property_accepting_number.has_value()) {
            auto number = NumberStyleValue::create(peek_token.token().number().value());
            if (property_accepts_number(*property_accepting_number, number->as_number().number())) {
                tokens.discard_a_token(); // number
                return PropertyAndValue { *property_accepting_number, number };
            }
        }
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::OpenTypeTag); property.has_value()) {
        if (auto maybe_rect = parse_opentype_tag_value(tokens))
            return PropertyAndValue { *property, maybe_rect };
    }

    if (peek_token.is(Token::Type::Percentage)) {
        auto percentage = Percentage(peek_token.token().percentage());
        if (auto property = any_property_accepts_type(property_ids, ValueType::Percentage); property.has_value() && property_accepts_percentage(*property, percentage)) {
            tokens.discard_a_token();
            return PropertyAndValue { *property, PercentageStyleValue::create(percentage) };
        }
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Rect); property.has_value()) {
        if (auto maybe_rect = parse_rect_value(tokens))
            return PropertyAndValue { *property, maybe_rect };
    }

    if (peek_token.is(Token::Type::String)) {
        if (auto property = any_property_accepts_type(property_ids, ValueType::String); property.has_value())
            return PropertyAndValue { *property, StringStyleValue::create(tokens.consume_a_token().token().string()) };
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Url); property.has_value()) {
        if (auto url = parse_url_value(tokens))
            return PropertyAndValue { *property, url };
    }

    bool property_accepts_dimension = any_property_accepts_type(property_ids, ValueType::Angle).has_value()
        || any_property_accepts_type(property_ids, ValueType::Flex).has_value()
        || any_property_accepts_type(property_ids, ValueType::Frequency).has_value()
        || any_property_accepts_type(property_ids, ValueType::Length).has_value()
        || any_property_accepts_type(property_ids, ValueType::Percentage).has_value()
        || any_property_accepts_type(property_ids, ValueType::Resolution).has_value()
        || any_property_accepts_type(property_ids, ValueType::Time).has_value();

    if (property_accepts_dimension) {
        if (peek_token.is(Token::Type::Number) && m_context.is_parsing_svg_presentation_attribute()) {
            auto transaction = tokens.begin_transaction();
            auto const& token = tokens.consume_a_token();
            // https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
            // We need to allow <number> in any place that expects a <length> or <angle>.
            // FIXME: How should these numbers be interpreted? https://github.com/w3c/svgwg/issues/792
            //        For now: Convert them to px lengths, or deg angles.
            auto angle = Angle::make_degrees(token.token().number_value());
            if (auto property = any_property_accepts_type(property_ids, ValueType::Angle); property.has_value() && property_accepts_angle(*property, angle)) {
                transaction.commit();
                return PropertyAndValue { *property, AngleStyleValue::create(angle) };
            }
            auto length = Length::make_px(CSSPixels::nearest_value_for(token.token().number_value()));
            if (auto property = any_property_accepts_type(property_ids, ValueType::Length); property.has_value() && property_accepts_length(*property, length)) {
                transaction.commit();
                return PropertyAndValue { *property, LengthStyleValue::create(length) };
            }
        }

        auto transaction = tokens.begin_transaction();
        if (auto maybe_dimension = parse_dimension(peek_token); maybe_dimension.has_value()) {
            tokens.discard_a_token();
            auto dimension = maybe_dimension.release_value();
            if (dimension.is_angle()) {
                auto angle = dimension.angle();
                if (auto property = any_property_accepts_type(property_ids, ValueType::Angle); property.has_value() && property_accepts_angle(*property, angle)) {
                    transaction.commit();
                    return PropertyAndValue { *property, AngleStyleValue::create(angle) };
                }
            }
            if (dimension.is_flex()) {
                auto flex = dimension.flex();
                if (auto property = any_property_accepts_type(property_ids, ValueType::Flex); property.has_value() && property_accepts_flex(*property, flex)) {
                    transaction.commit();
                    return PropertyAndValue { *property, FlexStyleValue::create(flex) };
                }
            }
            if (dimension.is_frequency()) {
                auto frequency = dimension.frequency();
                if (auto property = any_property_accepts_type(property_ids, ValueType::Frequency); property.has_value() && property_accepts_frequency(*property, frequency)) {
                    transaction.commit();
                    return PropertyAndValue { *property, FrequencyStyleValue::create(frequency) };
                }
            }
            if (dimension.is_length()) {
                auto length = dimension.length();
                if (auto property = any_property_accepts_type(property_ids, ValueType::Length); property.has_value() && property_accepts_length(*property, length)) {
                    transaction.commit();
                    return PropertyAndValue { *property, LengthStyleValue::create(length) };
                }
            }
            if (dimension.is_resolution()) {
                auto resolution = dimension.resolution();
                if (auto property = any_property_accepts_type(property_ids, ValueType::Resolution); property.has_value() && property_accepts_resolution(*property, resolution)) {
                    transaction.commit();
                    return PropertyAndValue { *property, ResolutionStyleValue::create(resolution) };
                }
            }
            if (dimension.is_time()) {
                auto time = dimension.time();
                if (auto property = any_property_accepts_type(property_ids, ValueType::Time); property.has_value() && property_accepts_time(*property, time)) {
                    transaction.commit();
                    return PropertyAndValue { *property, TimeStyleValue::create(time) };
                }
            }
        }
    }

    // In order to not end up parsing `calc()` and other math expressions multiple times,
    // we parse it once, and then see if its resolved type matches what the property accepts.
    if (peek_token.is_function() && (property_accepts_dimension || property_accepts_numeric)) {
        if (auto maybe_calculated = parse_calculated_value(peek_token); maybe_calculated) {
            tokens.discard_a_token();
            auto& calculated = *maybe_calculated;
            // This is a bit sensitive to ordering: `<foo>` and `<percentage>` have to be checked before `<foo-percentage>`.
            // FIXME: When parsing SVG presentation attributes, <number> is permitted wherever <length>, <length-percentage>, or <angle> are.
            //        The specifics are unclear, so I'm ignoring this for calculated values for now.
            //        See https://github.com/w3c/svgwg/issues/792
            if (calculated.resolves_to_percentage()) {
                if (auto property = any_property_accepts_type(property_ids, ValueType::Percentage); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_angle()) {
                if (auto property = any_property_accepts_type(property_ids, ValueType::Angle); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_angle_percentage()) {
                if (auto property = any_property_accepts_type_percentage(property_ids, ValueType::Angle); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_flex()) {
                if (auto property = any_property_accepts_type(property_ids, ValueType::Flex); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_frequency()) {
                if (auto property = any_property_accepts_type(property_ids, ValueType::Frequency); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_frequency_percentage()) {
                if (auto property = any_property_accepts_type_percentage(property_ids, ValueType::Frequency); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_number()) {
                if (property_accepts_numeric) {
                    auto property_or_resolved = property_accepting_integer.value_or_lazy_evaluated([property_accepting_number]() { return property_accepting_number.value(); });
                    return PropertyAndValue { property_or_resolved, calculated };
                }
            } else if (calculated.resolves_to_number_percentage()) {
                if (auto property = any_property_accepts_type_percentage(property_ids, ValueType::Number); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_length()) {
                if (auto property = any_property_accepts_type(property_ids, ValueType::Length); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_length_percentage()) {
                if (auto property = any_property_accepts_type_percentage(property_ids, ValueType::Length); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_time()) {
                if (auto property = any_property_accepts_type(property_ids, ValueType::Time); property.has_value())
                    return PropertyAndValue { *property, calculated };
            } else if (calculated.resolves_to_time_percentage()) {
                if (auto property = any_property_accepts_type_percentage(property_ids, ValueType::Time); property.has_value())
                    return PropertyAndValue { *property, calculated };
            }
        }
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Paint); property.has_value()) {
        if (auto value = parse_paint_value(tokens))
            return PropertyAndValue { *property, value.release_nonnull() };
    }

    return OptionalNone {};
}

class UnparsedCalculationNode final : public CalculationNode {
public:
    static NonnullOwnPtr<UnparsedCalculationNode> create(ComponentValue component_value)
    {
        return adopt_own(*new (nothrow) UnparsedCalculationNode(move(component_value)));
    }
    virtual ~UnparsedCalculationNode() = default;

    ComponentValue& component_value() { return m_component_value; }

    virtual String to_string() const override { VERIFY_NOT_REACHED(); }
    virtual Optional<CSSMathValue::ResolvedType> resolved_type() const override { VERIFY_NOT_REACHED(); }
    virtual Optional<CSSNumericType> determine_type(Web::CSS::PropertyID) const override { VERIFY_NOT_REACHED(); }
    virtual bool contains_percentage() const override { VERIFY_NOT_REACHED(); }
    virtual CSSMathValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CSSMathValue::PercentageBasis const&) const override { VERIFY_NOT_REACHED(); }
    virtual void for_each_child_node(AK::Function<void(NonnullOwnPtr<CalculationNode>&)> const&) override { }

    virtual void dump(StringBuilder& builder, int indent) const override
    {
        builder.appendff("{: >{}}UNPARSED({})\n", "", indent, m_component_value.to_debug_string());
    }
    virtual bool equals(CalculationNode const&) const override { return false; }

private:
    UnparsedCalculationNode(ComponentValue component_value)
        : CalculationNode(Type::Unparsed)
        , m_component_value(move(component_value))
    {
    }

    ComponentValue m_component_value;
};

// https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute
LengthOrCalculated Parser::Parser::parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img)
{
    // When asked to parse a sizes attribute from an element element, with an img element or null img:

    // AD-HOC: If element has no sizes attribute, this algorithm always logs a parse error and then returns 100vw.
    //         The attribute is optional, so avoid spamming the debug log with false positives by just returning early.
    if (!element.has_attribute(HTML::AttributeNames::sizes))
        return Length(100, Length::Type::Vw);

    // 1. Let unparsed sizes list be the result of parsing a comma-separated list of component values
    //    from the value of element's sizes attribute (or the empty string, if the attribute is absent).
    // NOTE: The sizes attribute has already been tokenized into m_token_stream by this point.
    auto unparsed_sizes_list = parse_a_comma_separated_list_of_component_values(m_token_stream);

    // 2. Let size be null.
    Optional<LengthOrCalculated> size;

    auto size_is_auto = [&size]() {
        return !size->is_calculated() && size->value().is_auto();
    };

    auto remove_all_consecutive_whitespace_tokens_from_the_end_of = [](auto& tokens) {
        while (!tokens.is_empty() && tokens.last().is_token() && tokens.last().token().is(Token::Type::Whitespace))
            tokens.take_last();
    };

    // 3. For each unparsed size in unparsed sizes list:
    for (auto i = 0u; i < unparsed_sizes_list.size(); i++) {
        auto& unparsed_size = unparsed_sizes_list[i];

        // 1. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        //    If unparsed size is now empty, that is a parse error; continue.
        remove_all_consecutive_whitespace_tokens_from_the_end_of(unparsed_size);
        if (unparsed_size.is_empty()) {
            log_parse_error();
            dbgln_if(CSS_PARSER_DEBUG, "-> Failed in step 3.1; all whitespace");
            continue;
        }

        // 2. If the last component value in unparsed size is a valid non-negative <source-size-value>,
        //    then set size to its value and remove the component value from unparsed size.
        //    Any CSS function other than the math functions is invalid.
        //    Otherwise, there is a parse error; continue.
        auto last_value_stream = TokenStream<ComponentValue>::of_single_token(unparsed_size.last());
        if (auto source_size_value = parse_source_size_value(last_value_stream); source_size_value.has_value()) {
            size = source_size_value.value();
            unparsed_size.take_last();
        } else {
            log_parse_error();
            dbgln_if(CSS_PARSER_DEBUG, "-> Failed in step 3.2; couldn't parse {} as a <source-size-value>", unparsed_size.last().to_debug_string());
            continue;
        }

        // 3. If size is auto, and img is not null, and img is being rendered, and img allows auto-sizes,
        //    then set size to the concrete object size width of img, in CSS pixels.
        // FIXME: "img is being rendered" - we just see if it has a bitmap for now
        if (size_is_auto() && img && img->immutable_bitmap() && img->allows_auto_sizes()) {
            // FIXME: The spec doesn't seem to tell us how to determine the concrete size of an <img>, so use the default sizing algorithm.
            //        Should this use some of the methods from FormattingContext?
            auto concrete_size = run_default_sizing_algorithm(
                img->width(), img->height(),
                img->natural_width(), img->natural_height(), img->intrinsic_aspect_ratio(),
                // NOTE: https://html.spec.whatwg.org/multipage/rendering.html#img-contain-size
                CSSPixelSize { 300, 150 });
            size = Length::make_px(concrete_size.width());
        }

        // 4. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        //    If unparsed size is now empty:
        remove_all_consecutive_whitespace_tokens_from_the_end_of(unparsed_size);
        if (unparsed_size.is_empty()) {
            // 1. If this was not the last item in unparsed sizes list, that is a parse error.
            if (i != unparsed_sizes_list.size() - 1) {
                log_parse_error();
                dbgln_if(CSS_PARSER_DEBUG, "-> Failed in step 3.4.1; is unparsed size #{}, count {}", i, unparsed_sizes_list.size());
            }

            // 2. If size is not auto, then return size. Otherwise, continue.
            if (!size_is_auto())
                return size.release_value();
            continue;
        }

        // 5. Parse the remaining component values in unparsed size as a <media-condition>.
        //    If it does not parse correctly, or it does parse correctly but the <media-condition> evaluates to false, continue.
        TokenStream<ComponentValue> token_stream { unparsed_size };
        auto media_condition = parse_media_condition(token_stream, MediaCondition::AllowOr::Yes);
        auto const* context_window = m_context.window();
        if (!media_condition || (context_window && media_condition->evaluate(*context_window) == MatchResult::False)) {
            continue;
        }

        // 5. If size is not auto, then return size. Otherwise, continue.
        if (!size_is_auto())
            return size.value();
    }

    // 4. Return 100vw.
    return Length(100, Length::Type::Vw);
}

// https://www.w3.org/TR/css-values-4/#parse-a-calculation
OwnPtr<CalculationNode> Parser::parse_a_calculation(Vector<ComponentValue> const& original_values)
{
    // 1. Discard any <whitespace-token>s from values.
    // 2. An item in values is an “operator” if it’s a <delim-token> with the value "+", "-", "*", or "/". Otherwise, it’s a “value”.
    struct Operator {
        char delim;
    };
    using Value = Variant<NonnullOwnPtr<CalculationNode>, Operator>;
    Vector<Value> values;
    for (auto const& value : original_values) {
        if (value.is(Token::Type::Whitespace))
            continue;
        if (value.is(Token::Type::Delim)) {
            if (first_is_one_of(value.token().delim(), static_cast<u32>('+'), static_cast<u32>('-'), static_cast<u32>('*'), static_cast<u32>('/'))) {
                // NOTE: Sequential operators are invalid syntax.
                if (!values.is_empty() && values.last().has<Operator>())
                    return nullptr;

                values.append(Operator { static_cast<char>(value.token().delim()) });
                continue;
            }
        }

        if (value.is(Token::Type::Ident)) {
            auto maybe_constant = CalculationNode::constant_type_from_string(value.token().ident());
            if (maybe_constant.has_value()) {
                values.append({ ConstantCalculationNode::create(maybe_constant.value()) });
                continue;
            }
        }

        if (value.is(Token::Type::Number)) {
            values.append({ NumericCalculationNode::create(value.token().number()) });
            continue;
        }

        if (auto dimension = parse_dimension(value); dimension.has_value()) {
            if (dimension->is_angle())
                values.append({ NumericCalculationNode::create(dimension->angle()) });
            else if (dimension->is_frequency())
                values.append({ NumericCalculationNode::create(dimension->frequency()) });
            else if (dimension->is_length())
                values.append({ NumericCalculationNode::create(dimension->length()) });
            else if (dimension->is_percentage())
                values.append({ NumericCalculationNode::create(dimension->percentage()) });
            else if (dimension->is_resolution())
                values.append({ NumericCalculationNode::create(dimension->resolution()) });
            else if (dimension->is_time())
                values.append({ NumericCalculationNode::create(dimension->time()) });
            else if (dimension->is_flex()) {
                // https://www.w3.org/TR/css3-grid-layout/#fr-unit
                // NOTE: <flex> values are not <length>s (nor are they compatible with <length>s, like some <percentage> values),
                //       so they cannot be represented in or combined with other unit types in calc() expressions.
                return nullptr;
            } else {
                VERIFY_NOT_REACHED();
            }
            continue;
        }

        values.append({ UnparsedCalculationNode::create(value) });
    }

    // If we have no values, the syntax is invalid.
    if (values.is_empty())
        return nullptr;

    // NOTE: If the first or last value is an operator, the syntax is invalid.
    if (values.first().has<Operator>() || values.last().has<Operator>())
        return nullptr;

    // 3. Collect children into Product and Invert nodes.
    //    For every consecutive run of value items in values separated by "*" or "/" operators:
    while (true) {
        Optional<size_t> first_product_operator = values.find_first_index_if([](auto const& item) {
            return item.template has<Operator>()
                && first_is_one_of(item.template get<Operator>().delim, '*', '/');
        });

        if (!first_product_operator.has_value())
            break;

        auto start_of_run = first_product_operator.value() - 1;
        auto end_of_run = first_product_operator.value() + 1;
        for (auto i = start_of_run + 1; i < values.size(); i += 2) {
            auto& item = values[i];
            if (!item.has<Operator>()) {
                end_of_run = i - 1;
                break;
            }

            auto delim = item.get<Operator>().delim;
            if (!first_is_one_of(delim, '*', '/')) {
                end_of_run = i - 1;
                break;
            }
        }

        // 1. For each "/" operator in the run, replace its right-hand value item rhs with an Invert node containing rhs as its child.
        Vector<NonnullOwnPtr<CalculationNode>> run_values;
        run_values.append(move(values[start_of_run].get<NonnullOwnPtr<CalculationNode>>()));
        for (auto i = start_of_run + 1; i <= end_of_run; i += 2) {
            auto& operator_ = values[i].get<Operator>().delim;
            auto& rhs = values[i + 1];
            if (operator_ == '/') {
                run_values.append(InvertCalculationNode::create(move(rhs.get<NonnullOwnPtr<CalculationNode>>())));
                continue;
            }
            VERIFY(operator_ == '*');
            run_values.append(move(rhs.get<NonnullOwnPtr<CalculationNode>>()));
        }
        // 2. Replace the entire run with a Product node containing the value items of the run as its children.
        auto product_node = ProductCalculationNode::create(move(run_values));
        values.remove(start_of_run, end_of_run - start_of_run + 1);
        values.insert(start_of_run, { move(product_node) });
    }

    // 4. Collect children into Sum and Negate nodes.
    Optional<NonnullOwnPtr<CalculationNode>> single_value;
    {
        // 1. For each "-" operator item in values, replace its right-hand value item rhs with a Negate node containing rhs as its child.
        for (auto i = 0u; i < values.size(); ++i) {
            auto& maybe_minus_operator = values[i];
            if (!maybe_minus_operator.has<Operator>() || maybe_minus_operator.get<Operator>().delim != '-')
                continue;

            auto rhs_index = ++i;
            auto& rhs = values[rhs_index];

            NonnullOwnPtr<CalculationNode> negate_node = NegateCalculationNode::create(move(rhs.get<NonnullOwnPtr<CalculationNode>>()));
            values.remove(rhs_index);
            values.insert(rhs_index, move(negate_node));
        }

        // 2. If values has only one item, and it is a Product node or a parenthesized simple block, replace values with that item.
        if (values.size() == 1) {
            values.first().visit(
                [&](ComponentValue& component_value) {
                    if (component_value.is_block() && component_value.block().is_paren())
                        single_value = UnparsedCalculationNode::create(move(component_value));
                },
                [&](NonnullOwnPtr<CalculationNode>& node) {
                    if (node->type() == CalculationNode::Type::Product)
                        single_value = move(node);
                },
                [](auto&) {});
        }
        //    Otherwise, replace values with a Sum node containing the value items of values as its children.
        if (!single_value.has_value()) {
            values.remove_all_matching([](Value& value) { return value.has<Operator>(); });
            Vector<NonnullOwnPtr<CalculationNode>> value_items;
            value_items.ensure_capacity(values.size());
            for (auto& value : values) {
                if (value.has<Operator>())
                    continue;
                value_items.unchecked_append(move(value.get<NonnullOwnPtr<CalculationNode>>()));
            }
            single_value = SumCalculationNode::create(move(value_items));
        }
    }

    // 5. At this point values is a tree of Sum, Product, Negate, and Invert nodes, with other types of values at the leaf nodes. Process the leaf nodes.
    //     For every leaf node leaf in values:
    bool parsing_failed_for_child_node = false;
    single_value.value()->for_each_child_node([&](NonnullOwnPtr<CalculationNode>& node) {
        if (node->type() != CalculationNode::Type::Unparsed)
            return;

        auto& unparsed_node = static_cast<UnparsedCalculationNode&>(*node);
        auto& component_value = unparsed_node.component_value();

        // 1. If leaf is a parenthesized simple block, replace leaf with the result of parsing a calculation from leaf’s contents.
        if (component_value.is_block() && component_value.block().is_paren()) {
            auto leaf_calculation = parse_a_calculation(component_value.block().value);
            if (!leaf_calculation) {
                parsing_failed_for_child_node = true;
                return;
            }
            node = leaf_calculation.release_nonnull();
            return;
        }

        // 2. If leaf is a math function, replace leaf with the internal representation of that math function.
        // NOTE: All function tokens at this point should be math functions.
        else if (component_value.is_function()) {
            auto& function = component_value.function();
            auto leaf_calculation = parse_a_calc_function_node(function);
            if (!leaf_calculation) {
                parsing_failed_for_child_node = true;
                return;
            }

            node = leaf_calculation.release_nonnull();
            return;
        }

        // NOTE: If we get here, then we have an UnparsedCalculationNode that didn't get replaced with something else.
        //       So, the calc() is invalid.
        dbgln_if(CSS_PARSER_DEBUG, "Leftover UnparsedCalculationNode in calc tree! That probably means the syntax is invalid, but maybe we just didn't implement `{}` yet.", component_value.to_debug_string());
        parsing_failed_for_child_node = true;
        return;
    });

    if (parsing_failed_for_child_node)
        return nullptr;

    // FIXME: 6. Return the result of simplifying a calculation tree from values.
    return single_value.release_value();
}

bool Parser::has_ignored_vendor_prefix(StringView string)
{
    if (!string.starts_with('-'))
        return false;
    if (string.starts_with("--"sv))
        return false;
    if (string.starts_with("-libweb-"sv))
        return false;
    return true;
}

NonnullRefPtr<CSSStyleValue> Parser::resolve_unresolved_style_value(ParsingContext const& context, DOM::Element& element, Optional<Selector::PseudoElement::Type> pseudo_element, PropertyID property_id, UnresolvedStyleValue const& unresolved)
{
    // Unresolved always contains a var() or attr(), unless it is a custom property's value, in which case we shouldn't be trying
    // to produce a different CSSStyleValue from it.
    VERIFY(unresolved.contains_var_or_attr());

    // If the value is invalid, we fall back to `unset`: https://www.w3.org/TR/css-variables-1/#invalid-at-computed-value-time

    auto parser = Parser::create(context, ""sv);
    return parser.resolve_unresolved_style_value(element, pseudo_element, property_id, unresolved);
}

class PropertyDependencyNode : public RefCounted<PropertyDependencyNode> {
public:
    static NonnullRefPtr<PropertyDependencyNode> create(FlyString name)
    {
        return adopt_ref(*new PropertyDependencyNode(move(name)));
    }

    void add_child(NonnullRefPtr<PropertyDependencyNode> new_child)
    {
        for (auto const& child : m_children) {
            if (child->m_name == new_child->m_name)
                return;
        }

        // We detect self-reference already.
        VERIFY(new_child->m_name != m_name);
        m_children.append(move(new_child));
    }

    bool has_cycles()
    {
        if (m_marked)
            return true;

        TemporaryChange change { m_marked, true };
        for (auto& child : m_children) {
            if (child->has_cycles())
                return true;
        }
        return false;
    }

private:
    explicit PropertyDependencyNode(FlyString name)
        : m_name(move(name))
    {
    }

    FlyString m_name;
    Vector<NonnullRefPtr<PropertyDependencyNode>> m_children;
    bool m_marked { false };
};

NonnullRefPtr<CSSStyleValue> Parser::resolve_unresolved_style_value(DOM::Element& element, Optional<Selector::PseudoElement::Type> pseudo_element, PropertyID property_id, UnresolvedStyleValue const& unresolved)
{
    TokenStream unresolved_values_without_variables_expanded { unresolved.values() };
    Vector<ComponentValue> values_with_variables_expanded;

    HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>> dependencies;
    if (!expand_variables(element, pseudo_element, string_from_property_id(property_id), dependencies, unresolved_values_without_variables_expanded, values_with_variables_expanded))
        return CSSKeywordValue::create(Keyword::Unset);

    TokenStream unresolved_values_with_variables_expanded { values_with_variables_expanded };
    Vector<ComponentValue> expanded_values;
    if (!expand_unresolved_values(element, string_from_property_id(property_id), unresolved_values_with_variables_expanded, expanded_values))
        return CSSKeywordValue::create(Keyword::Unset);

    auto expanded_value_tokens = TokenStream { expanded_values };
    if (auto parsed_value = parse_css_value(property_id, expanded_value_tokens); !parsed_value.is_error())
        return parsed_value.release_value();

    return CSSKeywordValue::create(Keyword::Unset);
}

static RefPtr<CSSStyleValue const> get_custom_property(DOM::Element const& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, FlyString const& custom_property_name)
{
    if (pseudo_element.has_value()) {
        if (auto it = element.custom_properties(pseudo_element).find(custom_property_name); it != element.custom_properties(pseudo_element).end())
            return it->value.value;
    }

    for (auto const* current_element = &element; current_element; current_element = current_element->parent_or_shadow_host_element()) {
        if (auto it = current_element->custom_properties({}).find(custom_property_name); it != current_element->custom_properties({}).end())
            return it->value.value;
    }
    return nullptr;
}

bool Parser::expand_variables(DOM::Element& element, Optional<Selector::PseudoElement::Type> pseudo_element, FlyString const& property_name, HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>>& dependencies, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
{
    // Arbitrary large value chosen to avoid the billion-laughs attack.
    // https://www.w3.org/TR/css-variables-1/#long-variables
    size_t const MAX_VALUE_COUNT = 16384;
    if (source.remaining_token_count() + dest.size() > MAX_VALUE_COUNT) {
        dbgln("Stopped expanding CSS variables: maximum length reached.");
        return false;
    }

    auto get_dependency_node = [&](FlyString const& name) -> NonnullRefPtr<PropertyDependencyNode> {
        if (auto existing = dependencies.get(name); existing.has_value())
            return *existing.value();
        auto new_node = PropertyDependencyNode::create(name);
        dependencies.set(name, new_node);
        return new_node;
    };

    while (source.has_next_token()) {
        auto const& value = source.consume_a_token();
        if (value.is_block()) {
            auto const& source_block = value.block();
            Vector<ComponentValue> block_values;
            TokenStream source_block_contents { source_block.value };
            if (!expand_variables(element, pseudo_element, property_name, dependencies, source_block_contents, block_values))
                return false;
            dest.empend(SimpleBlock { source_block.token, move(block_values) });
            continue;
        }
        if (!value.is_function()) {
            dest.empend(value.token());
            continue;
        }
        if (!value.function().name.equals_ignoring_ascii_case("var"sv)) {
            auto const& source_function = value.function();
            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.value };
            if (!expand_variables(element, pseudo_element, property_name, dependencies, source_function_contents, function_values))
                return false;
            dest.empend(Function { source_function.name, move(function_values) });
            continue;
        }

        TokenStream var_contents { value.function().value };
        var_contents.discard_whitespace();
        if (!var_contents.has_next_token())
            return false;

        auto const& custom_property_name_token = var_contents.consume_a_token();
        if (!custom_property_name_token.is(Token::Type::Ident))
            return false;
        auto custom_property_name = custom_property_name_token.token().ident();
        if (!custom_property_name.bytes_as_string_view().starts_with("--"sv))
            return false;

        // Detect dependency cycles. https://www.w3.org/TR/css-variables-1/#cycles
        // We do not do this by the spec, since we are not keeping a graph of var dependencies around,
        // but rebuilding it every time.
        if (custom_property_name == property_name)
            return false;
        auto parent = get_dependency_node(property_name);
        auto child = get_dependency_node(custom_property_name);
        parent->add_child(child);
        if (parent->has_cycles())
            return false;

        if (auto custom_property_value = get_custom_property(element, pseudo_element, custom_property_name)) {
            VERIFY(custom_property_value->is_unresolved());
            TokenStream custom_property_tokens { custom_property_value->as_unresolved().values() };
            if (!expand_variables(element, pseudo_element, custom_property_name, dependencies, custom_property_tokens, dest))
                return false;
            continue;
        }

        // Use the provided fallback value, if any.
        var_contents.discard_whitespace();
        if (var_contents.has_next_token()) {
            auto const& comma_token = var_contents.consume_a_token();
            if (!comma_token.is(Token::Type::Comma))
                return false;
            var_contents.discard_whitespace();
            if (!expand_variables(element, pseudo_element, property_name, dependencies, var_contents, dest))
                return false;
        }
    }
    return true;
}

bool Parser::expand_unresolved_values(DOM::Element& element, FlyString const& property_name, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
{
    auto property = property_id_from_string(property_name);

    while (source.has_next_token()) {
        auto const& value = source.consume_a_token();
        if (value.is_function()) {
            if (value.function().name.equals_ignoring_ascii_case("attr"sv)) {
                if (!substitute_attr_function(element, property_name, value.function(), dest))
                    return false;
                continue;
            }

            if (property.has_value()) {
                if (auto maybe_calc_value = parse_calculated_value(value); maybe_calc_value && maybe_calc_value->is_math()) {
                    // FIXME: Run the actual simplification algorithm
                    auto& calc_value = maybe_calc_value->as_math();
                    if (property_accepts_type(*property, ValueType::Angle) && calc_value.resolves_to_angle()) {
                        auto resolved_value = calc_value.resolve_angle();
                        dest.empend(Token::create_dimension(resolved_value->to_degrees(), "deg"_fly_string));
                        continue;
                    }
                    if (property_accepts_type(*property, ValueType::Frequency) && calc_value.resolves_to_frequency()) {
                        auto resolved_value = calc_value.resolve_frequency();
                        dest.empend(Token::create_dimension(resolved_value->to_hertz(), "hz"_fly_string));
                        continue;
                    }
                    if (property_accepts_type(*property, ValueType::Length) && calc_value.resolves_to_length()) {
                        // FIXME: In order to resolve lengths, we need to know the font metrics in case a font-relative unit
                        //  is used. So... we can't do that until style is computed?
                        //  This might be easier once we have calc-simplification implemented.
                    }
                    if (property_accepts_type(*property, ValueType::Percentage) && calc_value.resolves_to_percentage()) {
                        auto resolved_value = calc_value.resolve_percentage();
                        dest.empend(Token::create_percentage(resolved_value.value().value()));
                        continue;
                    }
                    if (property_accepts_type(*property, ValueType::Time) && calc_value.resolves_to_time()) {
                        auto resolved_value = calc_value.resolve_time();
                        dest.empend(Token::create_dimension(resolved_value->to_seconds(), "s"_fly_string));
                        continue;
                    }
                    if (property_accepts_type(*property, ValueType::Number) && calc_value.resolves_to_number()) {
                        auto resolved_value = calc_value.resolve_number();
                        dest.empend(Token::create_number(resolved_value.value(), Number::Type::Number));
                        continue;
                    }
                    if (property_accepts_type(*property, ValueType::Integer) && calc_value.resolves_to_number()) {
                        auto resolved_value = calc_value.resolve_integer();
                        dest.empend(Token::create_number(resolved_value.value(), Number::Type::Integer));
                        continue;
                    }
                }
            }

            auto const& source_function = value.function();
            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.value };
            if (!expand_unresolved_values(element, property_name, source_function_contents, function_values))
                return false;
            dest.empend(Function { source_function.name, move(function_values) });
            continue;
        }
        if (value.is_block()) {
            auto const& source_block = value.block();
            TokenStream source_block_values { source_block.value };
            Vector<ComponentValue> block_values;
            if (!expand_unresolved_values(element, property_name, source_block_values, block_values))
                return false;
            dest.empend(SimpleBlock { source_block.token, move(block_values) });
            continue;
        }
        dest.empend(value.token());
    }

    return true;
}

// https://drafts.csswg.org/css-values-5/#attr-substitution
bool Parser::substitute_attr_function(DOM::Element& element, FlyString const& property_name, Function const& attr_function, Vector<ComponentValue>& dest)
{
    // First, parse the arguments to attr():
    // attr() = attr( <q-name> <attr-type>? , <declaration-value>?)
    // <attr-type> = string | url | ident | color | number | percentage | length | angle | time | frequency | flex | <dimension-unit>
    TokenStream attr_contents { attr_function.value };
    attr_contents.discard_whitespace();
    if (!attr_contents.has_next_token())
        return false;

    // - Attribute name
    // FIXME: Support optional attribute namespace
    if (!attr_contents.next_token().is(Token::Type::Ident))
        return false;
    auto attribute_name = attr_contents.consume_a_token().token().ident();
    attr_contents.discard_whitespace();

    // - Attribute type (optional)
    auto attribute_type = "string"_fly_string;
    if (attr_contents.next_token().is(Token::Type::Ident)) {
        attribute_type = attr_contents.consume_a_token().token().ident();
        attr_contents.discard_whitespace();
    }

    // - Comma, then fallback values (optional)
    bool has_fallback_values = false;
    if (attr_contents.has_next_token()) {
        if (!attr_contents.next_token().is(Token::Type::Comma))
            return false;
        (void)attr_contents.consume_a_token(); // Comma
        has_fallback_values = true;
    }

    // Then, run the substitution algorithm:

    // 1. If the attr() function has a substitution value, replace the attr() function by the substitution value.
    // https://drafts.csswg.org/css-values-5/#attr-types
    if (element.has_attribute(attribute_name)) {
        auto attribute_value = element.get_attribute_value(attribute_name);
        if (attribute_type.equals_ignoring_ascii_case("angle"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Angle::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("color"_fly_string)) {
            // Parse a component value from the attribute’s value.
            // If the result is a <hex-color> or a named color ident, the substitution value is that result as a <color>.
            // Otherwise there is no substitution value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            if (component_value.has_value()) {
                if ((component_value->is(Token::Type::Hash)
                        && Color::from_string(MUST(String::formatted("#{}", component_value->token().hash_value()))).has_value())
                    || (component_value->is(Token::Type::Ident)
                        && Color::from_string(component_value->token().ident()).has_value())) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("flex"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Flex::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("frequency"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Frequency::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("ident"_fly_string)) {
            // The substitution value is a CSS <custom-ident>, whose value is the literal value of the attribute,
            // with leading and trailing ASCII whitespace stripped. (No CSS parsing of the value is performed.)
            // If the attribute value, after trimming, is the empty string, there is instead no substitution value.
            // If the <custom-ident>’s value is a CSS-wide keyword or `default`, there is instead no substitution value.
            auto substitution_value = MUST(attribute_value.trim(Infra::ASCII_WHITESPACE));
            if (!substitution_value.is_empty()
                && !substitution_value.equals_ignoring_ascii_case("default"sv)
                && !is_css_wide_keyword(substitution_value)) {
                dest.empend(Token::create_ident(substitution_value));
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("length"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Length::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("number"_fly_string)) {
            // Parse a component value from the attribute’s value.
            // If the result is a <number-token>, the result is the substitution value.
            // Otherwise, there is no substitution value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            if (component_value.has_value() && component_value->is(Token::Type::Number)) {
                dest.append(component_value.release_value());
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("percentage"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            // If the result is a <percentage-token>, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Percentage)) {
                dest.append(component_value.release_value());
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("string"_fly_string)) {
            // The substitution value is a CSS string, whose value is the literal value of the attribute.
            // (No CSS parsing or "cleanup" of the value is performed.)
            // No value triggers fallback.
            dest.empend(Token::create_string(attribute_value));
            return true;
        } else if (attribute_type.equals_ignoring_ascii_case("time"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            // If the result is a <dimension-token> whose unit matches the given type, the result is the substitution value.
            // Otherwise, there is no substitution value.
            if (component_value.has_value() && component_value->is(Token::Type::Dimension)) {
                if (Time::unit_from_name(component_value->token().dimension_unit()).has_value()) {
                    dest.append(component_value.release_value());
                    return true;
                }
            }
        } else if (attribute_type.equals_ignoring_ascii_case("url"_fly_string)) {
            // The substitution value is a CSS <url> value, whose url is the literal value of the attribute.
            // (No CSS parsing or "cleanup" of the value is performed.)
            // No value triggers fallback.
            dest.empend(Token::create_url(attribute_value));
            return true;
        } else {
            // Dimension units
            // Parse a component value from the attribute’s value.
            // If the result is a <number-token>, the substitution value is a dimension with the result’s value, and the given unit.
            // Otherwise, there is no substitution value.
            auto component_value = Parser::Parser::create(m_context, attribute_value).parse_as_component_value();
            if (component_value.has_value() && component_value->is(Token::Type::Number)) {
                if (attribute_value == "%"sv) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto angle_unit = Angle::unit_from_name(attribute_type); angle_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto flex_unit = Flex::unit_from_name(attribute_type); flex_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto frequency_unit = Frequency::unit_from_name(attribute_type); frequency_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto length_unit = Length::unit_from_name(attribute_type); length_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else if (auto time_unit = Time::unit_from_name(attribute_type); time_unit.has_value()) {
                    dest.empend(Token::create_dimension(component_value->token().number_value(), attribute_type));
                    return true;
                } else {
                    // Not a dimension unit.
                    return false;
                }
            }
        }
    }

    // 2. Otherwise, if the attr() function has a fallback value as its last argument, replace the attr() function by the fallback value.
    //    If there are any var() or attr() references in the fallback, substitute them as well.
    if (has_fallback_values)
        return expand_unresolved_values(element, property_name, attr_contents, dest);

    if (attribute_type.equals_ignoring_ascii_case("string"_fly_string)) {
        // If the <attr-type> argument is string, defaults to the empty string if omitted
        dest.empend(Token::create_string({}));
        return true;
    }

    // 3. Otherwise, the property containing the attr() function is invalid at computed-value time.
    return false;
}

// https://drafts.csswg.org/css-fonts/#typedef-opentype-tag
RefPtr<StringStyleValue> Parser::parse_opentype_tag_value(TokenStream<ComponentValue>& tokens)
{
    // <opentype-tag> = <string>
    // The <opentype-tag> is a case-sensitive OpenType feature tag.
    // As specified in the OpenType specification [OPENTYPE], feature tags contain four ASCII characters.
    // Tag strings longer or shorter than four characters, or containing characters outside the U+20–7E codepoint range are invalid.

    auto transaction = tokens.begin_transaction();
    auto string_value = parse_string_value(tokens);
    if (string_value == nullptr)
        return nullptr;

    auto string = string_value->string_value().bytes_as_string_view();
    if (string.length() != 4)
        return nullptr;
    for (char c : string) {
        if (c < 0x20 || c > 0x7E)
            return nullptr;
    }

    transaction.commit();
    return string_value;
}

Parser::ContextType Parser::context_type_for_at_rule(FlyString const& name)
{
    if (name == "media")
        return ContextType::AtMedia;
    if (name == "font-face")
        return ContextType::AtFontFace;
    if (name == "keyframes")
        return ContextType::AtKeyframes;
    if (name == "supports")
        return ContextType::AtSupports;
    if (name == "layer")
        return ContextType::AtLayer;
    if (name == "property")
        return ContextType::AtProperty;
    return ContextType::Unknown;
}

}
