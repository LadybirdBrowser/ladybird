/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
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

#include <AK/Debug.h>
#include <LibURL/Parser.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLImageElement.h>

static void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(CSS_PARSER_DEBUG, "Parse error (CSS) {}", location);
}

namespace Web::CSS::Parser {

ParsingParams::ParsingParams(ParsingMode mode)
    : mode(mode)
{
}

ParsingParams::ParsingParams(JS::Realm& realm, ParsingMode mode)
    : realm(realm)
    , mode(mode)
{
}

ParsingParams::ParsingParams(JS::Realm& realm, URL::URL url, ParsingMode mode)
    : realm(realm)
    , url(move(url))
    , mode(mode)
{
}

ParsingParams::ParsingParams(DOM::Document const& document, URL::URL url, ParsingMode mode)
    : realm(const_cast<JS::Realm&>(document.realm()))
    , document(&document)
    , url(move(url))
    , mode(mode)
{
}

ParsingParams::ParsingParams(DOM::Document const& document, ParsingMode mode)
    : realm(const_cast<JS::Realm&>(document.realm()))
    , document(&document)
    , url(document.url())
    , mode(mode)
{
}

Parser Parser::create(ParsingParams const& context, StringView input, StringView encoding)
{
    auto tokens = Tokenizer::tokenize(input, encoding);
    return Parser { context, move(tokens) };
}

Parser::Parser(ParsingParams const& context, Vector<Token> tokens)
    : m_document(context.document)
    , m_realm(context.realm)
    , m_url(context.url)
    , m_parsing_mode(context.mode)
    , m_tokens(move(tokens))
    , m_token_stream(m_tokens)
{
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
    GC::RootVector<CSSRule*> rules(realm().heap());
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

    auto rule_list = CSSRuleList::create(realm(), rules);
    auto media_list = MediaList::create(realm(), {});
    return CSSStyleSheet::create(realm(), rule_list, media_list, move(location));
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
    auto maybe_condition = parse_boolean_expression(token_stream, MatchResult::False, [this](auto& tokens) { return parse_supports_feature(tokens); });
    m_rule_context.take_last();
    token_stream.discard_whitespace();
    if (maybe_condition && !token_stream.has_next_token())
        return Supports::create(maybe_condition.release_nonnull());

    return {};
}

// https://drafts.csswg.org/css-values-5/#typedef-boolean-expr
OwnPtr<BooleanExpression> Parser::parse_boolean_expression(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed, ParseTest parse_test)
{
    // <boolean-expr[ <test> ]> = not <boolean-expr-group> | <boolean-expr-group>
    //                            [ [ and <boolean-expr-group> ]*
    //                            | [ or <boolean-expr-group> ]* ]

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto const& peeked_token = tokens.next_token();
    // `not <boolean-expr-group>`
    if (peeked_token.is_ident("not"sv)) {
        tokens.discard_a_token();
        tokens.discard_whitespace();

        if (auto child = parse_boolean_expression_group(tokens, result_for_general_enclosed, parse_test)) {
            transaction.commit();
            return BooleanNotExpression::create(child.release_nonnull());
        }
        return {};
    }

    // `<boolean-expr-group>
    //   [ [ and <boolean-expr-group> ]*
    //   | [ or <boolean-expr-group> ]* ]`
    Vector<NonnullOwnPtr<BooleanExpression>> children;
    enum class Combinator : u8 {
        And,
        Or,
    };
    Optional<Combinator> combinator;
    auto as_combinator = [](auto& token) -> Optional<Combinator> {
        if (!token.is(Token::Type::Ident))
            return {};
        auto ident = token.token().ident();
        if (ident.equals_ignoring_ascii_case("and"sv))
            return Combinator::And;
        if (ident.equals_ignoring_ascii_case("or"sv))
            return Combinator::Or;
        return {};
    };

    while (tokens.has_next_token()) {
        if (!children.is_empty()) {
            // Expect `and` or `or` here
            auto maybe_combinator = as_combinator(tokens.consume_a_token());
            if (!maybe_combinator.has_value())
                return {};
            if (!combinator.has_value()) {
                combinator = maybe_combinator.value();
            } else if (maybe_combinator != combinator) {
                return {};
            }
        }

        tokens.discard_whitespace();

        if (auto child = parse_boolean_expression_group(tokens, result_for_general_enclosed, parse_test)) {
            children.append(child.release_nonnull());
        } else {
            return {};
        }

        tokens.discard_whitespace();
    }

    if (children.is_empty())
        return {};

    transaction.commit();
    if (children.size() == 1)
        return children.take_first();

    VERIFY(combinator.has_value());
    switch (*combinator) {
    case Combinator::And:
        return BooleanAndExpression::create(move(children));
    case Combinator::Or:
        return BooleanOrExpression::create(move(children));
    }
    VERIFY_NOT_REACHED();
}

OwnPtr<BooleanExpression> Parser::parse_boolean_expression_group(TokenStream<ComponentValue>& tokens, MatchResult result_for_general_enclosed, ParseTest parse_test)
{
    // <boolean-expr-group> = <test> | ( <boolean-expr[ <test> ]> ) | <general-enclosed>

    // `( <boolean-expr[ <test> ]> )`
    auto const& first_token = tokens.next_token();
    if (first_token.is_block() && first_token.block().is_paren()) {
        auto transaction = tokens.begin_transaction();
        tokens.discard_a_token();
        tokens.discard_whitespace();

        TokenStream child_tokens { first_token.block().value };
        if (auto expression = parse_boolean_expression(child_tokens, result_for_general_enclosed, parse_test)) {
            if (child_tokens.has_next_token())
                return {};
            transaction.commit();
            return BooleanExpressionInParens::create(expression.release_nonnull());
        }
    }

    // `<test>`
    if (auto test = parse_test(tokens))
        return test.release_nonnull();

    // `<general-enclosed>`
    if (auto general_enclosed = parse_general_enclosed(tokens, result_for_general_enclosed))
        return general_enclosed.release_nonnull();

    return {};
}

// https://drafts.csswg.org/css-conditional-5/#typedef-supports-feature
OwnPtr<BooleanExpression> Parser::parse_supports_feature(TokenStream<ComponentValue>& tokens)
{
    // <supports-feature> = <supports-selector-fn> | <supports-font-tech-fn>
    //                    | <supports-font-format-fn> | <supports-decl>
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();

    // `<supports-decl> = ( <declaration> )`
    if (first_token.is_block() && first_token.block().is_paren()) {
        TokenStream block_tokens { first_token.block().value };
        // FIXME: Parsing and then converting back to a string is weird.
        if (auto declaration = consume_a_declaration(block_tokens); declaration.has_value()) {
            transaction.commit();
            auto supports_declaration = Supports::Declaration::create(
                declaration->to_string(),
                convert_to_style_property(*declaration).has_value());

            return BooleanExpressionInParens::create(supports_declaration.release_nonnull<BooleanExpression>());
        }
    }

    // `<supports-selector-fn> = selector( <complex-selector> )`
    if (first_token.is_function("selector"sv)) {
        // FIXME: Parsing and then converting back to a string is weird.
        StringBuilder builder;
        for (auto const& item : first_token.function().value)
            builder.append(item.to_string());
        transaction.commit();
        TokenStream selector_tokens { first_token.function().value };
        auto maybe_selector = parse_complex_selector(selector_tokens, SelectorType::Standalone);
        // A CSS processor is considered to support a CSS selector if it accepts that all aspects of that selector,
        // recursively, (rather than considering any of its syntax to be unknown or invalid) and that selector doesn’t
        // contain unknown -webkit- pseudo-elements.
        // https://drafts.csswg.org/css-conditional-4/#dfn-support-selector
        bool matches = !maybe_selector.is_error() && !maybe_selector.value()->contains_unknown_webkit_pseudo_element();
        return Supports::Selector::create(builder.to_string_without_validation(), matches);
    }

    // `<supports-font-tech-fn> = font-tech( <font-tech> )`
    if (first_token.is_function("font-tech"sv)) {
        TokenStream tech_tokens { first_token.function().value };
        tech_tokens.discard_whitespace();
        auto tech_token = tech_tokens.consume_a_token();
        tech_tokens.discard_whitespace();
        if (tech_tokens.has_next_token() || !tech_token.is(Token::Type::Ident))
            return {};

        transaction.commit();
        auto tech_name = tech_token.token().ident();
        bool matches = font_tech_is_supported(tech_name);
        return Supports::FontTech::create(move(tech_name), matches);
    }

    // `<supports-font-format-fn> = font-format( <font-format> )`
    if (first_token.is_function("font-format"sv)) {
        TokenStream format_tokens { first_token.function().value };
        format_tokens.discard_whitespace();
        auto format_token = format_tokens.consume_a_token();
        format_tokens.discard_whitespace();
        if (format_tokens.has_next_token() || !format_token.is(Token::Type::Ident))
            return {};

        transaction.commit();
        auto format_name = format_token.token().ident();
        bool matches = font_format_is_supported(format_name);
        return Supports::FontFormat::create(move(format_name), matches);
    }

    return {};
}

// https://www.w3.org/TR/mediaqueries-4/#typedef-general-enclosed
OwnPtr<GeneralEnclosed> Parser::parse_general_enclosed(TokenStream<ComponentValue>& tokens, MatchResult result)
{
    // FIXME: <general-enclosed> syntax changed in MediaQueries-5
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();
    auto const& first_token = tokens.consume_a_token();

    // `[ <function-token> <any-value>? ) ]`
    if (first_token.is_function()) {
        transaction.commit();
        return GeneralEnclosed::create(first_token.to_string(), result);
    }

    // `( <any-value>? )`
    if (first_token.is_block() && first_token.block().is_paren()) {
        transaction.commit();
        return GeneralEnclosed::create(first_token.to_string(), result);
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
template<>
ComponentValue Parser::consume_a_component_value(TokenStream<Token>& input)
{
    // To consume a component value from a token stream input:

    // Process input:
    for (;;) {
        auto const& token = input.next_token();

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

template<>
void Parser::consume_a_component_value_and_do_nothing<ComponentValue>(TokenStream<ComponentValue>& tokens)
{
    // AD-HOC: To avoid unnecessary allocations, we explicitly define a "do nothing" variant that discards the result immediately.
    // Note: This overload is called once tokens have already been converted into component values,
    //       so we do not need to do the work in the more general overload.
    tokens.discard_a_token();
}

// 5.4.7. Consume a component value
// https://drafts.csswg.org/css-syntax/#consume-component-value
template<>
void Parser::consume_a_component_value_and_do_nothing(TokenStream<Token>& input)
{
    // AD-HOC: To avoid unnecessary allocations, we explicitly define a "do nothing" variant that discards the result immediately.
    // To consume a component value from a token stream input:

    // Process input:
    for (;;) {
        auto const& token = input.next_token();

        // <{-token>
        // <[-token>
        // <(-token>
        if (token.is(Token::Type::OpenCurly) || token.is(Token::Type::OpenSquare) || token.is(Token::Type::OpenParen)) {
            // Consume a simple block from input and return the result.
            consume_a_simple_block_and_do_nothing(input);
            return;
        }

        // <function-token>
        if (token.is(Token::Type::Function)) {
            // Consume a function from input and return the result.
            consume_a_function_and_do_nothing(input);
            return;
        }

        // anything else
        {
            // Consume a token from input and return the result.
            input.discard_a_token();
            return;
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
SimpleBlock Parser::consume_a_simple_block(TokenStream<Token>& input)
{
    // To consume a simple block from a token stream input:

    // Assert: the next token of input is <{-token>, <[-token>, or <(-token>.
    auto const& next = input.next_token();
    VERIFY(next.is(Token::Type::OpenCurly) || next.is(Token::Type::OpenSquare) || next.is(Token::Type::OpenParen));

    // Let ending token be the mirror variant of the next token. (E.g. if it was called with <[-token>, the ending token is <]-token>.)
    auto ending_token = input.next_token().mirror_variant();

    // Let block be a new simple block with its associated token set to the next token and with its value initially set to an empty list.
    SimpleBlock block {
        .token = input.next_token(),
        .value = {},
    };

    // Discard a token from input.
    input.discard_a_token();

    // Process input:
    for (;;) {
        auto const& token = input.next_token();

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
            block.value.append(consume_a_component_value(input));
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-simple-block
void Parser::consume_a_simple_block_and_do_nothing(TokenStream<Token>& input)
{
    // AD-HOC: To avoid unnecessary allocations, we explicitly define a "do nothing" variant that discards the result immediately.
    // To consume a simple block from a token stream input:

    // Assert: the next token of input is <{-token>, <[-token>, or <(-token>.
    auto const& next = input.next_token();
    VERIFY(next.is(Token::Type::OpenCurly) || next.is(Token::Type::OpenSquare) || next.is(Token::Type::OpenParen));

    // Let ending token be the mirror variant of the next token. (E.g. if it was called with <[-token>, the ending token is <]-token>.)
    auto ending_token = input.next_token().mirror_variant();

    // Let block be a new simple block with its associated token set to the next token and with its value initially set to an empty list.

    // Discard a token from input.
    input.discard_a_token();

    // Process input:
    for (;;) {
        auto const& token = input.next_token();

        // <eof-token>
        // ending token
        if (token.is(Token::Type::EndOfFile) || token.is(ending_token)) {
            // Discard a token from input. Return block.
            input.discard_a_token();
            return;
        }

        // anything else
        {
            // Consume a component value from input and append the result to block’s value.
            consume_a_component_value_and_do_nothing(input);
        }
    }
}

// https://drafts.csswg.org/css-syntax/#consume-function
Function Parser::consume_a_function(TokenStream<Token>& input)
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
        auto const& token = input.next_token();

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

// https://drafts.csswg.org/css-syntax/#consume-function
void Parser::consume_a_function_and_do_nothing(TokenStream<Token>& input)
{
    // AD-HOC: To avoid unnecessary allocations, we explicitly define a "do nothing" variant that discards the result immediately.
    // To consume a function from a token stream input:

    // Assert: The next token is a <function-token>.
    VERIFY(input.next_token().is(Token::Type::Function));

    // Consume a token from input, and let function be a new function with its name equal the returned token’s value,
    // and a value set to an empty list.
    input.discard_a_token();

    // Process input:
    for (;;) {
        auto const& token = input.next_token();

        // <eof-token>
        // <)-token>
        if (token.is(Token::Type::EndOfFile) || token.is(Token::Type::CloseParen)) {
            // Discard a token from input. Return function.
            input.discard_a_token();
            return;
        }

        // anything else
        {
            // Consume a component value from input and append the result to function’s value.
            consume_a_component_value_and_do_nothing(input);
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
        auto const& token = input.next_token();

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
            consume_a_component_value_and_do_nothing(input);
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

Parser::PropertiesAndCustomProperties Parser::parse_as_style_attribute()
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

    auto properties = extract_properties(declarations_and_at_rules);
    properties.properties = expand_shorthands(properties.properties);
    return properties;
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

GC::Ref<CSSStyleProperties> Parser::convert_to_style_declaration(Vector<Declaration> const& declarations)
{
    PropertiesAndCustomProperties properties;
    PropertiesAndCustomProperties& dest = properties;
    for (auto const& declaration : declarations) {
        extract_property(declaration, dest);
    }
    return CSSStyleProperties::create(realm(), move(properties.properties), move(properties.custom_properties));
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

Optional<LengthOrCalculated> Parser::parse_source_size_value(TokenStream<ComponentValue>& tokens)
{
    if (tokens.next_token().is_ident("auto"sv)) {
        tokens.discard_a_token(); // auto
        return LengthOrCalculated { Length::make_auto() };
    }

    return parse_length(tokens);
}

bool Parser::context_allows_quirky_length() const
{
    if (!in_quirks_mode())
        return false;

    // https://drafts.csswg.org/css-values-4/#deprecated-quirky-length
    // "When CSS is being parsed in quirks mode, <quirky-length> is a type of <length> that is only valid in certain properties:"
    // (NOTE: List skipped for brevity; quirks data is assigned in Properties.json)
    // "It is not valid in properties that include or reference these properties, such as the background shorthand,
    // or inside functional notations such as calc(), except that they must be allowed in rect() in the clip property."

    // So, it must be allowed in the top-level ValueParsingContext, and then not disallowed by any child contexts.

    Optional<PropertyID> top_level_property;
    if (!m_value_context.is_empty()) {
        top_level_property = m_value_context.first().visit(
            [](PropertyID const& property_id) -> Optional<PropertyID> { return property_id; },
            [](auto const&) -> Optional<PropertyID> { return OptionalNone {}; });
    }

    bool unitless_length_allowed = top_level_property.has_value() && property_has_quirk(top_level_property.value(), Quirk::UnitlessLength);
    for (auto i = 1u; i < m_value_context.size() && unitless_length_allowed; i++) {
        unitless_length_allowed = m_value_context[i].visit(
            [](PropertyID const& property_id) { return property_has_quirk(property_id, Quirk::UnitlessLength); },
            [top_level_property](Parser::FunctionContext const& function_context) {
                return function_context.name == "rect"sv && top_level_property == PropertyID::Clip;
            });
    }

    return unitless_length_allowed;
}

Vector<ParsedFontFace::Source> Parser::parse_as_font_face_src()
{
    return parse_font_face_src(m_token_stream);
}

Vector<ComponentValue> Parser::parse_as_list_of_component_values()
{
    return parse_a_list_of_component_values(m_token_stream);
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

// https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute
LengthOrCalculated Parser::parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img)
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
        TokenStream token_stream { unparsed_size };
        auto media_condition = parse_media_condition(token_stream);
        auto const* context_window = window();
        if (!media_condition || (context_window && media_condition->evaluate(context_window) == MatchResult::False)) {
            continue;
        }

        // 5. If size is not auto, then return size. Otherwise, continue.
        if (!size_is_auto())
            return size.value();
    }

    // 4. Return 100vw.
    return Length(100, Length::Type::Vw);
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

template Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<Token>&, Optional<URL::URL>);
template Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<ComponentValue>&, Optional<URL::URL>);

template Vector<Rule> Parser::parse_a_stylesheets_contents(TokenStream<Token>& input);
template Vector<Rule> Parser::parse_a_stylesheets_contents(TokenStream<ComponentValue>& input);

template RefPtr<Supports> Parser::parse_a_supports(TokenStream<ComponentValue>&);
template RefPtr<Supports> Parser::parse_a_supports(TokenStream<Token>&);

template Vector<Rule> Parser::consume_a_stylesheets_contents(TokenStream<Token>&);
template Vector<Rule> Parser::consume_a_stylesheets_contents(TokenStream<ComponentValue>&);

template Optional<AtRule> Parser::consume_an_at_rule(TokenStream<Token>&, Nested);
template Optional<AtRule> Parser::consume_an_at_rule(TokenStream<ComponentValue>&, Nested);

template Variant<Empty, QualifiedRule, Parser::InvalidRuleError> Parser::consume_a_qualified_rule(TokenStream<Token>&, Optional<Token::Type>, Nested);
template Variant<Empty, QualifiedRule, Parser::InvalidRuleError> Parser::consume_a_qualified_rule(TokenStream<ComponentValue>&, Optional<Token::Type>, Nested);

template Vector<RuleOrListOfDeclarations> Parser::consume_a_block(TokenStream<Token>&);
template Vector<RuleOrListOfDeclarations> Parser::consume_a_block(TokenStream<ComponentValue>&);

template Vector<RuleOrListOfDeclarations> Parser::consume_a_blocks_contents(TokenStream<Token>&);
template Vector<RuleOrListOfDeclarations> Parser::consume_a_blocks_contents(TokenStream<ComponentValue>&);

template Vector<ComponentValue> Parser::consume_a_list_of_component_values(TokenStream<ComponentValue>&, Optional<Token::Type>, Nested);
template Vector<ComponentValue> Parser::consume_a_list_of_component_values(TokenStream<Token>&, Optional<Token::Type>, Nested);

template Optional<Declaration> Parser::consume_a_declaration(TokenStream<Token>&, Nested);
template Optional<Declaration> Parser::consume_a_declaration(TokenStream<ComponentValue>&, Nested);

template void Parser::consume_the_remnants_of_a_bad_declaration(TokenStream<Token>&, Nested);
template void Parser::consume_the_remnants_of_a_bad_declaration(TokenStream<ComponentValue>&, Nested);

template Optional<Rule> Parser::parse_a_rule(TokenStream<Token>&);
template Optional<Rule> Parser::parse_a_rule(TokenStream<ComponentValue>&);

template Vector<RuleOrListOfDeclarations> Parser::parse_a_blocks_contents(TokenStream<Token>&);
template Vector<RuleOrListOfDeclarations> Parser::parse_a_blocks_contents(TokenStream<ComponentValue>&);

template Optional<Declaration> Parser::parse_a_declaration(TokenStream<Token>&);
template Optional<Declaration> Parser::parse_a_declaration(TokenStream<ComponentValue>&);

template Optional<ComponentValue> Parser::parse_a_component_value(TokenStream<Token>&);
template Optional<ComponentValue> Parser::parse_a_component_value(TokenStream<ComponentValue>&);

template Vector<ComponentValue> Parser::parse_a_list_of_component_values(TokenStream<Token>&);
template Vector<ComponentValue> Parser::parse_a_list_of_component_values(TokenStream<ComponentValue>&);

template Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<ComponentValue>&);
template Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<Token>&);

DOM::Document const* Parser::document() const
{
    return m_document;
}

HTML::Window const* Parser::window() const
{
    if (!m_document)
        return nullptr;
    return m_document->window();
}

JS::Realm& Parser::realm() const
{
    VERIFY(m_realm);
    return *m_realm;
}

bool Parser::in_quirks_mode() const
{
    return m_document ? m_document->in_quirks_mode() : false;
}

bool Parser::is_parsing_svg_presentation_attribute() const
{
    return m_parsing_mode == ParsingMode::SVGPresentationAttribute;
}

// https://www.w3.org/TR/css-values-4/#relative-urls
// FIXME: URLs shouldn't be completed during parsing, but when used.
Optional<URL::URL> Parser::complete_url(StringView relative_url) const
{
    if (!m_url.is_valid())
        return URL::Parser::basic_parse(relative_url);
    return m_url.complete_url(relative_url);
}

}
