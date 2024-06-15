/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tommy van der Vorst <tommy@pixelspark.nl>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/SourceLocation.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframeRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSMediaRule.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSSupportsRule.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/EdgeRect.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/Block.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/DeclarationOrAtRule.h>
#include <LibWeb/CSS/Parser/Function.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Rule.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/IdentifierStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/InheritStyleValue.h>
#include <LibWeb/CSS/StyleValues/InitialStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RevertStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnsetStyleValue.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>

static void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(CSS_PARSER_DEBUG, "Parse error (CSS) {}", location);
}

namespace Web::CSS::Parser {

static bool contains_single_none_ident(TokenStream<ComponentValue>& tokens)
{
    if (tokens.remaining_token_count() > 1)
        return false;
    if (auto token = tokens.peek_token(); token.is_ident("none"sv))
        return true;
    return false;
}

ErrorOr<Parser> Parser::create(ParsingContext const& context, StringView input, StringView encoding)
{
    auto tokens = TRY(Tokenizer::tokenize(input, encoding));
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

// 5.3.3. Parse a stylesheet
// https://www.w3.org/TR/css-syntax-3/#parse-stylesheet
template<typename T>
Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<T>& tokens, Optional<URL::URL> location)
{
    // To parse a stylesheet from an input given an optional url location:

    // 1. If input is a byte stream for stylesheet, decode bytes from input, and set input to the result.
    // 2. Normalize input, and set input to the result.
    // NOTE: These are done automatically when creating the Parser.

    // 3. Create a new stylesheet, with its location set to location (or null, if location was not passed).
    ParsedStyleSheet style_sheet;
    style_sheet.location = move(location);

    // 4. Consume a list of rules from input, with the top-level flag set, and set the stylesheet’s value to the result.
    style_sheet.rules = consume_a_list_of_rules(tokens, TopLevel::Yes);

    // 5. Return the stylesheet.
    return style_sheet;
}

// https://www.w3.org/TR/css-syntax-3/#parse-a-css-stylesheet
CSSStyleSheet* Parser::parse_as_css_stylesheet(Optional<URL::URL> location)
{
    // To parse a CSS stylesheet, first parse a stylesheet.
    auto style_sheet = parse_a_stylesheet(m_token_stream, {});

    // Interpret all of the resulting top-level qualified rules as style rules, defined below.
    JS::MarkedVector<CSSRule*> rules(m_context.realm().heap());
    for (auto& raw_rule : style_sheet.rules) {
        auto* rule = convert_to_rule(raw_rule);
        // If any style rule is invalid, or any at-rule is not recognized or is invalid according to its grammar or context, it’s a parse error. Discard that rule.
        if (rule)
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
    auto maybe_condition = parse_supports_condition(token_stream);
    token_stream.skip_whitespace();
    if (maybe_condition && !token_stream.has_next_token())
        return Supports::create(m_context.realm(), maybe_condition.release_nonnull());

    return {};
}

OwnPtr<Supports::Condition> Parser::parse_supports_condition(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.skip_whitespace();

    auto const& peeked_token = tokens.peek_token();
    // `not <supports-in-parens>`
    if (peeked_token.is_ident("not"sv)) {
        tokens.next_token();
        tokens.skip_whitespace();
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
            auto maybe_combination = as_condition_type(tokens.next_token());
            if (!maybe_combination.has_value())
                return {};
            if (!condition_type.has_value()) {
                condition_type = maybe_combination.value();
            } else if (maybe_combination != condition_type) {
                return {};
            }
        }

        tokens.skip_whitespace();

        if (auto in_parens = parse_supports_in_parens(tokens); in_parens.has_value()) {
            children.append(in_parens.release_value());
        } else {
            return {};
        }

        tokens.skip_whitespace();
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
    auto const& first_token = tokens.peek_token();
    if (first_token.is_block() && first_token.block().is_paren()) {
        auto transaction = tokens.begin_transaction();
        tokens.next_token();
        tokens.skip_whitespace();

        TokenStream child_tokens { first_token.block().values() };
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
    tokens.skip_whitespace();
    auto const& first_token = tokens.next_token();

    // `<supports-decl>`
    if (first_token.is_block() && first_token.block().is_paren()) {
        TokenStream block_tokens { first_token.block().values() };
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
        for (auto const& item : first_token.function().values())
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
    tokens.skip_whitespace();
    auto const& first_token = tokens.next_token();

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

// 5.4.1. Consume a list of rules
// https://www.w3.org/TR/css-syntax-3/#consume-list-of-rules
template<typename T>
Vector<NonnullRefPtr<Rule>> Parser::consume_a_list_of_rules(TokenStream<T>& tokens, TopLevel top_level)
{
    // To consume a list of rules, given a top-level flag:

    // Create an initially empty list of rules.
    Vector<NonnullRefPtr<Rule>> rules;

    // Repeatedly consume the next input token:
    for (;;) {
        auto& token = tokens.next_token();

        // <whitespace-token>
        if (token.is(Token::Type::Whitespace)) {
            // Do nothing.
            continue;
        }

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // Return the list of rules.
            return rules;
        }

        // <CDO-token>
        // <CDC-token>
        if (token.is(Token::Type::CDO) || token.is(Token::Type::CDC)) {
            // If the top-level flag is set, do nothing.
            if (top_level == TopLevel::Yes)
                continue;

            // Otherwise, reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume a qualified rule. If anything is returned, append it to the list of rules.
            if (auto maybe_qualified = consume_a_qualified_rule(tokens))
                rules.append(maybe_qualified.release_nonnull());

            continue;
        }

        // <at-keyword-token>
        if (token.is(Token::Type::AtKeyword)) {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume an at-rule, and append the returned value to the list of rules.
            rules.append(consume_an_at_rule(tokens));

            continue;
        }

        // anything else
        {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume a qualified rule. If anything is returned, append it to the list of rules.
            if (auto maybe_qualified = consume_a_qualified_rule(tokens))
                rules.append(maybe_qualified.release_nonnull());

            continue;
        }
    }
}

// 5.4.2. Consume an at-rule
// https://www.w3.org/TR/css-syntax-3/#consume-at-rule
template<typename T>
NonnullRefPtr<Rule> Parser::consume_an_at_rule(TokenStream<T>& tokens)
{
    // To consume an at-rule:

    // Consume the next input token.
    auto& name_ident = tokens.next_token();
    VERIFY(name_ident.is(Token::Type::AtKeyword));

    // Create a new at-rule with its name set to the value of the current input token, its prelude initially set to an empty list, and its value initially set to nothing.
    // NOTE: We create the Rule fully initialized when we return it instead.
    auto at_rule_name = ((Token)name_ident).at_keyword();
    Vector<ComponentValue> prelude;
    RefPtr<Block> block;

    // Repeatedly consume the next input token:
    for (;;) {
        auto& token = tokens.next_token();

        // <semicolon-token>
        if (token.is(Token::Type::Semicolon)) {
            // Return the at-rule.
            return Rule::make_at_rule(move(at_rule_name), move(prelude), move(block));
        }

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // This is a parse error. Return the at-rule.
            log_parse_error();
            return Rule::make_at_rule(move(at_rule_name), move(prelude), move(block));
        }

        // <{-token>
        if (token.is(Token::Type::OpenCurly)) {
            // Consume a simple block and assign it to the at-rule’s block. Return the at-rule.
            block = consume_a_simple_block(tokens);
            return Rule::make_at_rule(move(at_rule_name), move(prelude), move(block));
        }

        // simple block with an associated token of <{-token>
        if constexpr (IsSame<T, ComponentValue>) {
            ComponentValue const& component_value = token;
            if (component_value.is_block() && component_value.block().is_curly()) {
                // Assign the block to the at-rule’s block. Return the at-rule.
                block = component_value.block();
                return Rule::make_at_rule(move(at_rule_name), move(prelude), move(block));
            }
        }

        // anything else
        {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();
            // Consume a component value. Append the returned value to the at-rule’s prelude.
            prelude.append(consume_a_component_value(tokens));
        }
    }
}

// 5.4.3. Consume a qualified rule
// https://www.w3.org/TR/css-syntax-3/#consume-qualified-rule
template<typename T>
RefPtr<Rule> Parser::consume_a_qualified_rule(TokenStream<T>& tokens)
{
    // To consume a qualified rule:

    // Create a new qualified rule with its prelude initially set to an empty list, and its value initially set to nothing.
    // NOTE: We create the Rule fully initialized when we return it instead.
    Vector<ComponentValue> prelude;
    RefPtr<Block> block;

    // Repeatedly consume the next input token:
    for (;;) {
        auto& token = tokens.next_token();

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // This is a parse error. Return nothing.
            log_parse_error();
            return {};
        }

        // <{-token>
        if (token.is(Token::Type::OpenCurly)) {
            // Consume a simple block and assign it to the qualified rule’s block. Return the qualified rule.
            block = consume_a_simple_block(tokens);
            return Rule::make_qualified_rule(move(prelude), move(block));
        }

        // simple block with an associated token of <{-token>
        if constexpr (IsSame<T, ComponentValue>) {
            ComponentValue const& component_value = token;
            if (component_value.is_block() && component_value.block().is_curly()) {
                // Assign the block to the qualified rule’s block. Return the qualified rule.
                block = component_value.block();
                return Rule::make_qualified_rule(move(prelude), move(block));
            }
        }

        // anything else
        {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume a component value. Append the returned value to the qualified rule’s prelude.
            prelude.append(consume_a_component_value(tokens));
        }
    }
}

// 5.4.4. Consume a style block’s contents
// https://www.w3.org/TR/css-syntax-3/#consume-a-style-blocks-contents
template<typename T>
Vector<DeclarationOrAtRule> Parser::consume_a_style_blocks_contents(TokenStream<T>& tokens)
{
    // To consume a style block’s contents:
    // Create an initially empty list of declarations decls, and an initially empty list of rules rules.
    Vector<DeclarationOrAtRule> declarations;
    Vector<DeclarationOrAtRule> rules;

    // Repeatedly consume the next input token:
    for (;;) {
        auto& token = tokens.next_token();

        // <whitespace-token>
        // <semicolon-token>
        if (token.is(Token::Type::Whitespace) || token.is(Token::Type::Semicolon)) {
            // Do nothing.
            continue;
        }

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // Extend decls with rules, then return decls.
            declarations.extend(move(rules));
            return declarations;
        }

        // <at-keyword-token>
        if (token.is(Token::Type::AtKeyword)) {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume an at-rule, and append the result to rules.
            rules.empend(consume_an_at_rule(tokens));
            continue;
        }

        // <ident-token>
        if (token.is(Token::Type::Ident)) {
            // Initialize a temporary list initially filled with the current input token.
            Vector<ComponentValue> temporary_list;
            temporary_list.append(token);

            // As long as the next input token is anything other than a <semicolon-token> or <EOF-token>,
            // consume a component value and append it to the temporary list.
            for (;;) {
                auto& next_input_token = tokens.peek_token();
                if (next_input_token.is(Token::Type::Semicolon) || next_input_token.is(Token::Type::EndOfFile))
                    break;
                temporary_list.append(consume_a_component_value(tokens));
            }

            // Consume a declaration from the temporary list. If anything was returned, append it to decls.
            auto token_stream = TokenStream(temporary_list);
            if (auto maybe_declaration = consume_a_declaration(token_stream); maybe_declaration.has_value())
                declarations.empend(maybe_declaration.release_value());

            continue;
        }

        // <delim-token> with a value of "&" (U+0026 AMPERSAND)
        if (token.is_delim('&')) {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume a qualified rule. If anything was returned, append it to rules.
            if (auto qualified_rule = consume_a_qualified_rule(tokens))
                rules.empend(qualified_rule);

            continue;
        }

        // anything else
        {
            // This is a parse error.
            log_parse_error();

            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // As long as the next input token is anything other than a <semicolon-token> or <EOF-token>,
            // consume a component value and throw away the returned value.
            for (;;) {
                auto& peek = tokens.peek_token();
                if (peek.is(Token::Type::Semicolon) || peek.is(Token::Type::EndOfFile))
                    break;
                (void)consume_a_component_value(tokens);
            }
        }
    }
}

template<>
ComponentValue Parser::consume_a_component_value(TokenStream<ComponentValue>& tokens)
{
    // Note: This overload is called once tokens have already been converted into component values,
    //       so we do not need to do the work in the more general overload.
    return tokens.next_token();
}

// 5.4.7. Consume a component value
// https://www.w3.org/TR/css-syntax-3/#consume-component-value
template<typename T>
ComponentValue Parser::consume_a_component_value(TokenStream<T>& tokens)
{
    // To consume a component value:

    // Consume the next input token.
    auto& token = tokens.next_token();

    // If the current input token is a <{-token>, <[-token>, or <(-token>, consume a simple block and return it.
    if (token.is(Token::Type::OpenCurly) || token.is(Token::Type::OpenSquare) || token.is(Token::Type::OpenParen))
        return ComponentValue(consume_a_simple_block(tokens));

    // Otherwise, if the current input token is a <function-token>, consume a function and return it.
    if (token.is(Token::Type::Function))
        return ComponentValue(consume_a_function(tokens));

    // Otherwise, return the current input token.
    return ComponentValue(token);
}

// 5.4.8. Consume a simple block
// https://www.w3.org/TR/css-syntax-3/#consume-simple-block
template<typename T>
NonnullRefPtr<Block> Parser::consume_a_simple_block(TokenStream<T>& tokens)
{
    // Note: This algorithm assumes that the current input token has already been checked
    // to be an <{-token>, <[-token>, or <(-token>.

    // To consume a simple block:

    // The ending token is the mirror variant of the current input token.
    // (E.g. if it was called with <[-token>, the ending token is <]-token>.)
    auto ending_token = ((Token)tokens.current_token()).mirror_variant();

    // Create a simple block with its associated token set to the current input token
    // and with its value initially set to an empty list.
    // NOTE: We create the Block fully initialized when we return it instead.
    Token block_token = tokens.current_token();
    Vector<ComponentValue> block_values;

    // Repeatedly consume the next input token and process it as follows:
    for (;;) {
        auto& token = tokens.next_token();

        // ending token
        if (token.is(ending_token)) {
            // Return the block.
            return Block::create(move(block_token), move(block_values));
        }
        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // This is a parse error. Return the block.
            log_parse_error();
            return Block::create(move(block_token), move(block_values));
        }

        // anything else
        {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume a component value and append it to the value of the block.
            block_values.empend(consume_a_component_value(tokens));
        }
    }
}

// 5.4.9. Consume a function
// https://www.w3.org/TR/css-syntax-3/#consume-function
template<typename T>
NonnullRefPtr<Function> Parser::consume_a_function(TokenStream<T>& tokens)
{
    // Note: This algorithm assumes that the current input token has already been checked to be a <function-token>.
    auto name_ident = tokens.current_token();
    VERIFY(name_ident.is(Token::Type::Function));

    // To consume a function:

    // Create a function with its name equal to the value of the current input token
    // and with its value initially set to an empty list.
    // NOTE: We create the Function fully initialized when we return it instead.
    auto function_name = ((Token)name_ident).function();
    Vector<ComponentValue> function_values;

    // Repeatedly consume the next input token and process it as follows:
    for (;;) {
        auto& token = tokens.next_token();

        // <)-token>
        if (token.is(Token::Type::CloseParen)) {
            // Return the function.
            return Function::create(move(function_name), move(function_values));
        }

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // This is a parse error. Return the function.
            log_parse_error();
            return Function::create(move(function_name), move(function_values));
        }

        // anything else
        {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume a component value and append the returned value to the function’s value.
            function_values.append(consume_a_component_value(tokens));
        }
    }
}

// 5.4.6. Consume a declaration
// https://www.w3.org/TR/css-syntax-3/#consume-declaration
template<typename T>
Optional<Declaration> Parser::consume_a_declaration(TokenStream<T>& tokens)
{
    // Note: This algorithm assumes that the next input token has already been checked to
    // be an <ident-token>.
    // NOTE: This is not true in our implementation! For convenience, we both skip whitespace
    //       and gracefully handle the first token not being an <ident-token>.

    // To consume a declaration:

    // Consume the next input token.
    auto transaction = tokens.begin_transaction();
    tokens.skip_whitespace();
    auto& token = tokens.next_token();

    // NOTE: Not to spec, handle the case where the input token *isn't* an <ident-token>.
    if (!token.is(Token::Type::Ident))
        return {};

    // Create a new declaration with its name set to the value of the current input token
    // and its value initially set to the empty list.
    // NOTE: We create a fully-initialized Declaration just before returning it instead.
    auto declaration_name = ((Token)token).ident();
    Vector<ComponentValue> declaration_values;
    Important declaration_important = Important::No;

    // 1. While the next input token is a <whitespace-token>, consume the next input token.
    tokens.skip_whitespace();

    // 2. If the next input token is anything other than a <colon-token>, this is a parse error.
    // Return nothing.
    auto& maybe_colon = tokens.peek_token();
    if (!maybe_colon.is(Token::Type::Colon)) {
        log_parse_error();
        return {};
    }
    // Otherwise, consume the next input token.
    tokens.next_token();

    // 3. While the next input token is a <whitespace-token>, consume the next input token.
    tokens.skip_whitespace();

    // 4. As long as the next input token is anything other than an <EOF-token>, consume a
    //    component value and append it to the declaration’s value.
    for (;;) {
        if (tokens.peek_token().is(Token::Type::EndOfFile)) {
            break;
        }
        declaration_values.append(consume_a_component_value(tokens));
    }

    // 5. If the last two non-<whitespace-token>s in the declaration’s value are a <delim-token>
    //    with the value "!" followed by an <ident-token> with a value that is an ASCII case-insensitive
    //    match for "important", remove them from the declaration’s value and set the declaration’s
    //    important flag to true.
    if (declaration_values.size() >= 2) {
        // Walk backwards from the end until we find "important"
        Optional<size_t> important_index;
        for (size_t i = declaration_values.size() - 1; i > 0; i--) {
            auto value = declaration_values[i];
            if (value.is_ident("important"sv)) {
                important_index = i;
                break;
            }
            if (value.is(Token::Type::Whitespace))
                continue;
            break;
        }

        // Walk backwards from important until we find "!"
        if (important_index.has_value()) {
            Optional<size_t> bang_index;
            for (size_t i = important_index.value() - 1; i > 0; i--) {
                auto value = declaration_values[i];
                if (value.is_delim('!')) {
                    bang_index = i;
                    break;
                }
                if (value.is(Token::Type::Whitespace))
                    continue;
                break;
            }

            if (bang_index.has_value()) {
                declaration_values.remove(important_index.value());
                declaration_values.remove(bang_index.value());
                declaration_important = Important::Yes;
            }
        }
    }

    // 6. While the last token in the declaration’s value is a <whitespace-token>, remove that token.
    while (!declaration_values.is_empty()) {
        auto maybe_whitespace = declaration_values.last();
        if (!(maybe_whitespace.is(Token::Type::Whitespace))) {
            break;
        }
        declaration_values.take_last();
    }

    // 7. Return the declaration.
    transaction.commit();
    return Declaration { move(declaration_name), move(declaration_values), declaration_important };
}

// 5.4.5. Consume a list of declarations
// https://www.w3.org/TR/css-syntax-3/#consume-list-of-declarations
template<typename T>
Vector<DeclarationOrAtRule> Parser::consume_a_list_of_declarations(TokenStream<T>& tokens)
{
    // To consume a list of declarations:

    // Create an initially empty list of declarations.
    Vector<DeclarationOrAtRule> list_of_declarations;

    // Repeatedly consume the next input token:
    for (;;) {
        auto& token = tokens.next_token();

        // <whitespace-token>
        // <semicolon-token>
        if (token.is(Token::Type::Whitespace) || token.is(Token::Type::Semicolon)) {
            // Do nothing.
            continue;
        }

        // <EOF-token>
        if (token.is(Token::Type::EndOfFile)) {
            // Return the list of declarations.
            return list_of_declarations;
        }

        // <at-keyword-token>
        if (token.is(Token::Type::AtKeyword)) {
            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // Consume an at-rule. Append the returned rule to the list of declarations.
            list_of_declarations.empend(consume_an_at_rule(tokens));
            continue;
        }

        // <ident-token>
        if (token.is(Token::Type::Ident)) {
            // Initialize a temporary list initially filled with the current input token.
            Vector<ComponentValue> temporary_list;
            temporary_list.append(token);

            // As long as the next input token is anything other than a <semicolon-token> or <EOF-token>,
            // consume a component value and append it to the temporary list.
            for (;;) {
                auto& peek = tokens.peek_token();
                if (peek.is(Token::Type::Semicolon) || peek.is(Token::Type::EndOfFile))
                    break;
                temporary_list.append(consume_a_component_value(tokens));
            }

            // Consume a declaration from the temporary list. If anything was returned, append it to the list of declarations.
            auto token_stream = TokenStream(temporary_list);
            if (auto maybe_declaration = consume_a_declaration(token_stream); maybe_declaration.has_value())
                list_of_declarations.empend(maybe_declaration.value());

            continue;
        }

        // anything else
        {
            // This is a parse error.
            log_parse_error();

            // Reconsume the current input token.
            tokens.reconsume_current_input_token();

            // As long as the next input token is anything other than a <semicolon-token> or <EOF-token>,
            // consume a component value and throw away the returned value.
            for (;;) {
                auto& peek = tokens.peek_token();
                if (peek.is(Token::Type::Semicolon) || peek.is(Token::Type::EndOfFile))
                    break;
                dbgln_if(CSS_PARSER_DEBUG, "Discarding token: '{}'", peek.to_debug_string());
                (void)consume_a_component_value(tokens);
            }
        }
    }
}

CSSRule* Parser::parse_as_css_rule()
{
    auto maybe_rule = parse_a_rule(m_token_stream);
    if (maybe_rule)
        return convert_to_rule(maybe_rule.release_nonnull());
    return {};
}

// 5.3.5. Parse a rule
// https://www.w3.org/TR/css-syntax-3/#parse-rule
template<typename T>
RefPtr<Rule> Parser::parse_a_rule(TokenStream<T>& tokens)
{
    // To parse a rule from input:
    RefPtr<Rule> rule;

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. While the next input token from input is a <whitespace-token>, consume the next input token from input.
    tokens.skip_whitespace();

    // 3. If the next input token from input is an <EOF-token>, return a syntax error.
    auto& token = tokens.peek_token();
    if (token.is(Token::Type::EndOfFile)) {
        return {};
    }
    // Otherwise, if the next input token from input is an <at-keyword-token>, consume an at-rule from input, and let rule be the return value.
    else if (token.is(Token::Type::AtKeyword)) {
        rule = consume_an_at_rule(m_token_stream);
    }
    // Otherwise, consume a qualified rule from input and let rule be the return value. If nothing was returned, return a syntax error.
    else {
        auto qualified_rule = consume_a_qualified_rule(tokens);
        if (!qualified_rule)
            return {};

        rule = qualified_rule;
    }

    // 4. While the next input token from input is a <whitespace-token>, consume the next input token from input.
    tokens.skip_whitespace();

    // 5. If the next input token from input is an <EOF-token>, return rule. Otherwise, return a syntax error.
    if (tokens.peek_token().is(Token::Type::EndOfFile))
        return rule;
    return {};
}

// 5.3.4. Parse a list of rules
// https://www.w3.org/TR/css-syntax-3/#parse-list-of-rules
template<typename T>
Vector<NonnullRefPtr<Rule>> Parser::parse_a_list_of_rules(TokenStream<T>& tokens)
{
    // To parse a list of rules from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Consume a list of rules from the input, with the top-level flag unset.
    auto list_of_rules = consume_a_list_of_rules(tokens, TopLevel::No);

    // 3. Return the returned list.
    return list_of_rules;
}
template Vector<NonnullRefPtr<Rule>> Parser::parse_a_list_of_rules(TokenStream<Token>& tokens);
template Vector<NonnullRefPtr<Rule>> Parser::parse_a_list_of_rules(TokenStream<ComponentValue>& tokens);

Optional<StyleProperty> Parser::parse_as_supports_condition()
{
    auto maybe_declaration = parse_a_declaration(m_token_stream);
    if (maybe_declaration.has_value())
        return convert_to_style_property(maybe_declaration.release_value());
    return {};
}

// 5.3.6. Parse a declaration
// https://www.w3.org/TR/css-syntax-3/#parse-a-declaration
template<typename T>
Optional<Declaration> Parser::parse_a_declaration(TokenStream<T>& tokens)
{
    // To parse a declaration from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. While the next input token from input is a <whitespace-token>, consume the next input token.
    tokens.skip_whitespace();

    // 3. If the next input token from input is not an <ident-token>, return a syntax error.
    auto& token = tokens.peek_token();
    if (!token.is(Token::Type::Ident)) {
        return {};
    }

    // 4. Consume a declaration from input. If anything was returned, return it. Otherwise, return a syntax error.
    if (auto declaration = consume_a_declaration(tokens); declaration.has_value())
        return declaration.release_value();
    return {};
}

// 5.3.7. Parse a style block’s contents
// https://www.w3.org/TR/css-syntax-3/#parse-style-blocks-contents
template<typename T>
Vector<DeclarationOrAtRule> Parser::parse_a_style_blocks_contents(TokenStream<T>& tokens)
{
    // To parse a style block’s contents from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Consume a style block’s contents from input, and return the result.
    return consume_a_style_blocks_contents(tokens);
}

// 5.3.8. Parse a list of declarations
// https://www.w3.org/TR/css-syntax-3/#parse-list-of-declarations
template<typename T>
Vector<DeclarationOrAtRule> Parser::parse_a_list_of_declarations(TokenStream<T>& tokens)
{
    // To parse a list of declarations from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Consume a list of declarations from input, and return the result.
    return consume_a_list_of_declarations(tokens);
}

Optional<ComponentValue> Parser::parse_as_component_value()
{
    return parse_a_component_value(m_token_stream);
}

// 5.3.9. Parse a component value
// https://www.w3.org/TR/css-syntax-3/#parse-component-value
template<typename T>
Optional<ComponentValue> Parser::parse_a_component_value(TokenStream<T>& tokens)
{
    // To parse a component value from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. While the next input token from input is a <whitespace-token>, consume the next input token from input.
    tokens.skip_whitespace();

    // 3. If the next input token from input is an <EOF-token>, return a syntax error.
    if (tokens.peek_token().is(Token::Type::EndOfFile))
        return {};

    // 4. Consume a component value from input and let value be the return value.
    auto value = consume_a_component_value(tokens);

    // 5. While the next input token from input is a <whitespace-token>, consume the next input token.
    tokens.skip_whitespace();

    // 6. If the next input token from input is an <EOF-token>, return value. Otherwise, return a syntax error.
    if (tokens.peek_token().is(Token::Type::EndOfFile))
        return value;
    return {};
}

// 5.3.10. Parse a list of component values
// https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values
template<typename T>
Vector<ComponentValue> Parser::parse_a_list_of_component_values(TokenStream<T>& tokens)
{
    // To parse a list of component values from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Repeatedly consume a component value from input until an <EOF-token> is returned, appending the returned values (except the final <EOF-token>) into a list. Return the list.
    Vector<ComponentValue> component_values;

    for (;;) {
        if (tokens.peek_token().is(Token::Type::EndOfFile)) {
            break;
        }

        component_values.append(consume_a_component_value(tokens));
    }

    return component_values;
}

// 5.3.11. Parse a comma-separated list of component values
// https://www.w3.org/TR/css-syntax-3/#parse-comma-separated-list-of-component-values
template<typename T>
Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<T>& tokens)
{
    // To parse a comma-separated list of component values from input:

    // 1. Normalize input, and set input to the result.
    // Note: This is done when initializing the Parser.

    // 2. Let list of cvls be an initially empty list of component value lists.
    Vector<Vector<ComponentValue>> list_of_component_value_lists;

    // 3. Repeatedly consume a component value from input until an <EOF-token> or <comma-token> is returned,
    //    appending the returned values (except the final <EOF-token> or <comma-token>) into a list.
    //    Append the list to list of cvls.
    //    If it was a <comma-token> that was returned, repeat this step.
    Vector<ComponentValue> current_list;
    for (;;) {
        auto component_value = consume_a_component_value(tokens);

        if (component_value.is(Token::Type::EndOfFile)) {
            list_of_component_value_lists.append(move(current_list));
            break;
        }
        if (component_value.is(Token::Type::Comma)) {
            list_of_component_value_lists.append(move(current_list));
            current_list = {};
            continue;
        }

        current_list.append(component_value);
    }

    // 4. Return list of cvls.
    return list_of_component_value_lists;
}
template Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<ComponentValue>&);
template Vector<Vector<ComponentValue>> Parser::parse_a_comma_separated_list_of_component_values(TokenStream<Token>&);

ElementInlineCSSStyleDeclaration* Parser::parse_as_style_attribute(DOM::Element& element)
{
    auto declarations_and_at_rules = parse_a_list_of_declarations(m_token_stream);
    auto [properties, custom_properties] = extract_properties(declarations_and_at_rules);
    return ElementInlineCSSStyleDeclaration::create(element, move(properties), move(custom_properties));
}

Optional<URL::URL> Parser::parse_url_function(ComponentValue const& component_value)
{
    // FIXME: Handle list of media queries. https://www.w3.org/TR/css-cascade-3/#conditional-import

    auto convert_string_to_url = [&](StringView url_string) -> Optional<URL::URL> {
        auto url = m_context.complete_url(url_string);
        if (url.is_valid())
            return url;
        return {};
    };

    if (component_value.is(Token::Type::Url)) {
        auto url_string = component_value.token().url();
        return convert_string_to_url(url_string);
    }
    if (component_value.is_function("url"sv)) {
        auto const& function_values = component_value.function().values();
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

RefPtr<StyleValue> Parser::parse_url_value(TokenStream<ComponentValue>& tokens)
{
    auto url = parse_url_function(tokens.peek_token());
    if (!url.has_value())
        return nullptr;
    (void)tokens.next_token();
    return URLStyleValue::create(*url);
}

RefPtr<StyleValue> Parser::parse_basic_shape_function(ComponentValue const& component_value)
{
    if (!component_value.is_function())
        return nullptr;

    auto function_name = component_value.function().name().bytes_as_string_view();

    // FIXME: Implement other shapes. See: https://www.w3.org/TR/css-shapes-1/#basic-shape-functions
    if (!function_name.equals_ignoring_ascii_case("polygon"sv))
        return nullptr;

    // polygon() = polygon( <'fill-rule'>? , [<length-percentage> <length-percentage>]# )
    // FIXME: Parse the fill-rule.
    auto arguments_tokens = TokenStream { component_value.function().values() };
    auto arguments = parse_a_comma_separated_list_of_component_values(arguments_tokens);

    Vector<Polygon::Point> points;
    for (auto& argument : arguments) {
        TokenStream argument_tokens { argument };

        argument_tokens.skip_whitespace();
        auto x_pos = parse_length_percentage(argument_tokens);
        if (!x_pos.has_value())
            return nullptr;

        argument_tokens.skip_whitespace();
        auto y_pos = parse_length_percentage(argument_tokens);
        if (!y_pos.has_value())
            return nullptr;

        argument_tokens.skip_whitespace();
        if (argument_tokens.has_next_token())
            return nullptr;

        points.append(Polygon::Point { *x_pos, *y_pos });
    }

    return BasicShapeStyleValue::create(Polygon { FillRule::Nonzero, move(points) });
}

RefPtr<StyleValue> Parser::parse_basic_shape_value(TokenStream<ComponentValue>& tokens)
{
    auto basic_shape = parse_basic_shape_function(tokens.peek_token());
    if (!basic_shape)
        return nullptr;
    (void)tokens.next_token();
    return basic_shape;
}

CSSRule* Parser::convert_to_rule(NonnullRefPtr<Rule> rule)
{
    if (rule->is_at_rule()) {
        if (has_ignored_vendor_prefix(rule->at_rule_name()))
            return {};
        if (rule->at_rule_name().equals_ignoring_ascii_case("font-face"sv)) {
            if (!rule->block() || !rule->block()->is_curly()) {
                dbgln_if(CSS_PARSER_DEBUG, "@font-face rule is malformed.");
                return {};
            }
            TokenStream tokens { rule->block()->values() };
            return parse_font_face_rule(tokens);
        }
        if (rule->at_rule_name().equals_ignoring_ascii_case("import"sv) && !rule->prelude().is_empty()) {
            Optional<URL::URL> url;
            for (auto const& token : rule->prelude()) {
                if (token.is(Token::Type::Whitespace))
                    continue;

                if (token.is(Token::Type::String)) {
                    url = m_context.complete_url(token.token().string());
                } else {
                    url = parse_url_function(token);
                }

                // FIXME: Handle list of media queries. https://www.w3.org/TR/css-cascade-3/#conditional-import
                if (url.has_value())
                    break;
            }

            if (url.has_value())
                return CSSImportRule::create(url.value(), const_cast<DOM::Document&>(*m_context.document()));
            dbgln_if(CSS_PARSER_DEBUG, "Unable to parse url from @import rule");
            return {};
        }
        if (rule->at_rule_name().equals_ignoring_ascii_case("media"sv))
            return convert_to_media_rule(rule);
        if (rule->at_rule_name().equals_ignoring_ascii_case("supports"sv)) {
            auto supports_tokens = TokenStream { rule->prelude() };
            auto supports = parse_a_supports(supports_tokens);
            if (!supports) {
                if constexpr (CSS_PARSER_DEBUG) {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @supports rule invalid; discarding.");
                    supports_tokens.dump_all_tokens();
                }
                return {};
            }

            if (!rule->block())
                return {};
            auto child_tokens = TokenStream { rule->block()->values() };
            auto parser_rules = parse_a_list_of_rules(child_tokens);
            JS::MarkedVector<CSSRule*> child_rules(m_context.realm().heap());
            for (auto& raw_rule : parser_rules) {
                if (auto* child_rule = convert_to_rule(raw_rule))
                    child_rules.append(child_rule);
            }

            auto rule_list = CSSRuleList::create(m_context.realm(), child_rules);
            return CSSSupportsRule::create(m_context.realm(), supports.release_nonnull(), rule_list);
        }
        if (rule->at_rule_name().equals_ignoring_ascii_case("keyframes"sv)) {
            auto prelude_stream = TokenStream { rule->prelude() };
            prelude_stream.skip_whitespace();
            auto token = prelude_stream.next_token();
            if (!token.is_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes has invalid prelude, prelude = {}; discarding.", rule->prelude());
                return {};
            }

            auto name_token = token.token();
            prelude_stream.skip_whitespace();

            if (prelude_stream.has_next_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes has invalid prelude, prelude = {}; discarding.", rule->prelude());
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

            if (!rule->block())
                return {};

            auto child_tokens = TokenStream { rule->block()->values() };

            JS::MarkedVector<CSSRule*> keyframes(m_context.realm().heap());
            while (child_tokens.has_next_token()) {
                child_tokens.skip_whitespace();
                // keyframe-selector = <keyframe-keyword> | <percentage>
                // keyframe-keyword = "from" | "to"
                // selector = <keyframe-selector>#
                // keyframes-block = "{" <declaration-list>? "}"
                // keyframe-rule = <selector> <keyframes-block>

                auto selectors = Vector<CSS::Percentage> {};
                while (child_tokens.has_next_token()) {
                    child_tokens.skip_whitespace();
                    if (!child_tokens.has_next_token())
                        break;
                    auto tok = child_tokens.next_token();
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
                        child_tokens.skip_whitespace();
                        if (child_tokens.next_token().is(Token::Type::Comma))
                            continue;
                    }

                    child_tokens.reconsume_current_input_token();
                    break;
                }

                if (!child_tokens.has_next_token())
                    break;

                child_tokens.skip_whitespace();
                auto token = child_tokens.next_token();
                if (token.is_block()) {
                    auto block_tokens = token.block().values();
                    auto block_stream = TokenStream { block_tokens };

                    auto block_declarations = parse_a_list_of_declarations(block_stream);
                    auto style = convert_to_style_declaration(block_declarations);
                    for (auto& selector : selectors) {
                        auto keyframe_rule = CSSKeyframeRule::create(m_context.realm(), selector, *style);
                        keyframes.append(keyframe_rule);
                    }
                } else {
                    dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @keyframes rule has invalid block: {}; discarding.", token.to_debug_string());
                }
            }

            return CSSKeyframesRule::create(m_context.realm(), name, CSSRuleList::create(m_context.realm(), move(keyframes)));
        }
        if (rule->at_rule_name().equals_ignoring_ascii_case("namespace"sv)) {
            // https://drafts.csswg.org/css-namespaces/#syntax
            auto token_stream = TokenStream { rule->prelude() };
            token_stream.skip_whitespace();

            auto token = token_stream.next_token();
            Optional<FlyString> prefix = {};
            if (token.is(Token::Type::Ident)) {
                prefix = token.token().ident();
                token_stream.skip_whitespace();
                token = token_stream.next_token();
            }

            FlyString namespace_uri;
            if (token.is(Token::Type::String)) {
                namespace_uri = token.token().string();
            } else if (auto url = parse_url_function(token); url.has_value()) {
                namespace_uri = MUST(url.value().to_string());
            } else {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @namespace rule invalid; discarding.");
                return {};
            }

            token_stream.skip_whitespace();
            if (token_stream.has_next_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @namespace rule invalid; discarding.");
                return {};
            }

            return CSSNamespaceRule::create(m_context.realm(), prefix, namespace_uri);
        }

        // FIXME: More at rules!
        dbgln_if(CSS_PARSER_DEBUG, "Unrecognized CSS at-rule: @{}", rule->at_rule_name());
        return {};
    }

    auto prelude_stream = TokenStream(rule->prelude());
    auto selectors = parse_a_selector_list(prelude_stream, SelectorType::Standalone);

    if (selectors.is_error()) {
        if (selectors.error() == ParseError::SyntaxError) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: style rule selectors invalid; discarding.");
            if constexpr (CSS_PARSER_DEBUG) {
                prelude_stream.dump_all_tokens();
            }
        }
        return {};
    }

    if (selectors.value().is_empty()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: empty selector; discarding.");
        return {};
    }

    if (!rule->block()->is_curly())
        return {};

    auto stream = TokenStream(rule->block()->values());
    auto declarations_and_at_rules = parse_a_style_blocks_contents(stream);

    auto* declaration = convert_to_style_declaration(declarations_and_at_rules);
    if (!declaration) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: style rule declaration invalid; discarding.");
        return {};
    }

    return CSSStyleRule::create(m_context.realm(), move(selectors.value()), *declaration);
}

auto Parser::extract_properties(Vector<DeclarationOrAtRule> const& declarations_and_at_rules) -> PropertiesAndCustomProperties
{
    PropertiesAndCustomProperties result;
    for (auto const& declaration_or_at_rule : declarations_and_at_rules) {
        if (declaration_or_at_rule.is_at_rule()) {
            dbgln_if(CSS_PARSER_DEBUG, "!!! CSS at-rule is not allowed here!");
            continue;
        }

        auto const& declaration = declaration_or_at_rule.declaration();

        if (auto maybe_property = convert_to_style_property(declaration); maybe_property.has_value()) {
            auto property = maybe_property.release_value();
            if (property.property_id == PropertyID::Custom) {
                result.custom_properties.set(property.custom_name, property);
            } else {
                result.properties.append(move(property));
            }
        }
    }
    return result;
}

PropertyOwningCSSStyleDeclaration* Parser::convert_to_style_declaration(Vector<DeclarationOrAtRule> const& declarations_and_at_rules)
{
    auto [properties, custom_properties] = extract_properties(declarations_and_at_rules);
    return PropertyOwningCSSStyleDeclaration::create(m_context.realm(), move(properties), move(custom_properties));
}

Optional<StyleProperty> Parser::convert_to_style_property(Declaration const& declaration)
{
    auto const& property_name = declaration.name();
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

    auto value_token_stream = TokenStream(declaration.values());
    auto value = parse_css_value(property_id.value(), value_token_stream);
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
        return StyleProperty { declaration.importance(), property_id.value(), value.release_value(), declaration.name() };

    return StyleProperty { declaration.importance(), property_id.value(), value.release_value(), {} };
}

RefPtr<StyleValue> Parser::parse_builtin_value(ComponentValue const& component_value)
{
    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();
        if (ident.equals_ignoring_ascii_case("inherit"sv))
            return InheritStyleValue::the();
        if (ident.equals_ignoring_ascii_case("initial"sv))
            return InitialStyleValue::the();
        if (ident.equals_ignoring_ascii_case("unset"sv))
            return UnsetStyleValue::the();
        if (ident.equals_ignoring_ascii_case("revert"sv))
            return RevertStyleValue::the();
        // FIXME: Implement `revert-layer` from CSS-CASCADE-5.
    }

    return nullptr;
}

RefPtr<CalculatedStyleValue> Parser::parse_calculated_value(ComponentValue const& component_value)
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

    return CalculatedStyleValue::create(function_node.release_nonnull(), function_type.release_value());
}

OwnPtr<CalculationNode> Parser::parse_a_calc_function_node(Function const& function)
{
    if (function.name().equals_ignoring_ascii_case("calc"sv))
        return parse_a_calculation(function.values());

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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

Optional<ResolutionOrCalculated> Parser::parse_resolution(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    auto& token = tokens.next_token();

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
    if (tokens.peek_token().is_ident("auto"sv)) {
        (void)tokens.next_token(); // auto
        return LengthOrCalculated { Length::make_auto() };
    }

    return parse_length(tokens);
}

Optional<Ratio> Parser::parse_ratio(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    tokens.skip_whitespace();

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
    auto maybe_numerator = read_number_value(tokens.next_token());
    if (!maybe_numerator.has_value() || maybe_numerator.value() < 0)
        return {};
    auto numerator = maybe_numerator.value();

    {
        auto two_value_transaction = tokens.begin_transaction();
        tokens.skip_whitespace();
        auto solidus = tokens.next_token();
        tokens.skip_whitespace();
        auto maybe_denominator = read_number_value(tokens.next_token());

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
    tokens.skip_whitespace();

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
    auto const& u = tokens.next_token();
    if (!u.is_ident("u"sv)) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> does not start with 'u'");
        return {};
    }

    auto const& second_token = tokens.next_token();

    //  u '+' <ident-token> '?'* |
    //  u '+' '?'+
    if (second_token.is_delim('+')) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().representation());

        auto const& third_token = tokens.next_token();
        if (third_token.is(Token::Type::Ident) || third_token.is_delim('?')) {
            string_builder.append(third_token.token().representation());
            while (tokens.peek_token().is_delim('?'))
                string_builder.append(tokens.next_token().token().representation());
            if (is_ending_token(tokens.peek_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        }
    }

    //  u <dimension-token> '?'*
    if (second_token.is(Token::Type::Dimension)) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().representation());
        while (tokens.peek_token().is_delim('?'))
            string_builder.append(tokens.next_token().token().representation());
        if (is_ending_token(tokens.peek_token()))
            return create_unicode_range(string_builder.string_view(), local_transaction);
    }

    //  u <number-token> '?'* |
    //  u <number-token> <dimension-token> |
    //  u <number-token> <number-token>
    if (second_token.is(Token::Type::Number)) {
        auto local_transaction = tokens.begin_transaction();
        StringBuilder string_builder;
        string_builder.append(second_token.token().representation());

        if (is_ending_token(tokens.peek_token()))
            return create_unicode_range(string_builder.string_view(), local_transaction);

        auto const& third_token = tokens.next_token();
        if (third_token.is_delim('?')) {
            string_builder.append(third_token.token().representation());
            while (tokens.peek_token().is_delim('?'))
                string_builder.append(tokens.next_token().token().representation());
            if (is_ending_token(tokens.peek_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        } else if (third_token.is(Token::Type::Dimension)) {
            string_builder.append(third_token.token().representation());
            if (is_ending_token(tokens.peek_token()))
                return create_unicode_range(string_builder.string_view(), local_transaction);
        } else if (third_token.is(Token::Type::Number)) {
            string_builder.append(third_token.token().representation());
            if (is_ending_token(tokens.peek_token()))
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
    auto hex_digits = lexer.consume_while(is_ascii_hex_digit);
    auto question_marks = lexer.consume_while([](auto it) { return it == '?'; });
    //    If zero code points were consumed, or more than six code points were consumed,
    //    this is an invalid <urange>, and this algorithm must exit.
    size_t consumed_code_points = hex_digits.length() + question_marks.length();
    if (consumed_code_points == 0 || consumed_code_points > 6) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: <urange> start value had {} digits/?s, expected between 1 and 6.", consumed_code_points);
        return {};
    }
    StringView start_value_code_points { hex_digits.characters_without_null_termination(), consumed_code_points };

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

RefPtr<StyleValue> Parser::parse_dimension_value(TokenStream<ComponentValue>& tokens)
{
    auto dimension = parse_dimension(tokens.peek_token());
    if (!dimension.has_value())
        return nullptr;
    (void)tokens.next_token(); // dimension

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

RefPtr<StyleValue> Parser::parse_integer_value(TokenStream<ComponentValue>& tokens)
{
    auto peek_token = tokens.peek_token();
    if (peek_token.is(Token::Type::Number) && peek_token.token().number().is_integer()) {
        (void)tokens.next_token();
        return IntegerStyleValue::create(peek_token.token().number().integer_value());
    }

    return nullptr;
}

RefPtr<StyleValue> Parser::parse_number_value(TokenStream<ComponentValue>& tokens)
{
    auto peek_token = tokens.peek_token();
    if (peek_token.is(Token::Type::Number)) {
        (void)tokens.next_token();
        return NumberStyleValue::create(peek_token.token().number().value());
    }

    return nullptr;
}

RefPtr<StyleValue> Parser::parse_number_or_percentage_value(TokenStream<ComponentValue>& tokens)
{
    auto peek_token = tokens.peek_token();
    if (peek_token.is(Token::Type::Number)) {
        (void)tokens.next_token();
        return NumberStyleValue::create(peek_token.token().number().value());
    }
    if (peek_token.is(Token::Type::Percentage)) {
        (void)tokens.next_token();
        return PercentageStyleValue::create(Percentage(peek_token.token().percentage()));
    }

    return nullptr;
}

RefPtr<StyleValue> Parser::parse_identifier_value(TokenStream<ComponentValue>& tokens)
{
    auto peek_token = tokens.peek_token();
    if (peek_token.is(Token::Type::Ident)) {
        auto value_id = value_id_from_string(peek_token.token().ident());
        if (value_id.has_value()) {
            (void)tokens.next_token(); // ident
            return IdentifierStyleValue::create(value_id.value());
        }
    }

    return nullptr;
}

Optional<Color> Parser::parse_rgb_or_hsl_color(StringView function_name, Vector<ComponentValue> const& component_values)
{
    Token params[4];
    bool legacy_syntax = false;
    auto tokens = TokenStream { component_values };

    tokens.skip_whitespace();
    auto const& component1 = tokens.next_token();

    if (!component1.is(Token::Type::Number)
        && !component1.is(Token::Type::Percentage)
        && !component1.is(Token::Type::Dimension))
        return {};
    params[0] = component1.token();

    tokens.skip_whitespace();
    if (tokens.peek_token().is(Token::Type::Comma)) {
        legacy_syntax = true;
        tokens.next_token();
    }

    tokens.skip_whitespace();
    auto const& component2 = tokens.next_token();
    if (!component2.is(Token::Type::Number) && !component2.is(Token::Type::Percentage))
        return {};
    params[1] = component2.token();

    tokens.skip_whitespace();
    if (legacy_syntax && !tokens.next_token().is(Token::Type::Comma))
        return {};

    tokens.skip_whitespace();
    auto const& component3 = tokens.next_token();
    if (!component3.is(Token::Type::Number) && !component3.is(Token::Type::Percentage))
        return {};
    params[2] = component3.token();

    tokens.skip_whitespace();
    auto const& alpha_separator = tokens.peek_token();
    bool has_comma = alpha_separator.is(Token::Type::Comma);
    bool has_slash = alpha_separator.is_delim('/');
    if (legacy_syntax ? has_comma : has_slash) {
        tokens.next_token();

        tokens.skip_whitespace();
        auto const& component4 = tokens.next_token();
        if (!component4.is(Token::Type::Number) && !component4.is(Token::Type::Percentage))
            return {};
        params[3] = component4.token();
    }

    tokens.skip_whitespace();
    if (tokens.has_next_token())
        return {};

    if (function_name.equals_ignoring_ascii_case("rgb"sv)
        || function_name.equals_ignoring_ascii_case("rgba"sv)) {

        // https://www.w3.org/TR/css-color-4/#rgb-functions

        u8 a_val = 255;
        if (params[3].is(Token::Type::Number))
            a_val = clamp(lround(params[3].number_value() * 255.0), 0, 255);
        else if (params[3].is(Token::Type::Percentage))
            a_val = clamp(lround(params[3].percentage() * 2.55), 0, 255);

        if (params[0].is(Token::Type::Number)
            && params[1].is(Token::Type::Number)
            && params[2].is(Token::Type::Number)) {

            u8 r_val = clamp(llroundf(params[0].number_value()), 0, 255);
            u8 g_val = clamp(llroundf(params[1].number_value()), 0, 255);
            u8 b_val = clamp(llroundf(params[2].number_value()), 0, 255);

            return Color(r_val, g_val, b_val, a_val);
        }

        if (params[0].is(Token::Type::Percentage)
            && params[1].is(Token::Type::Percentage)
            && params[2].is(Token::Type::Percentage)) {

            u8 r_val = lround(clamp(params[0].percentage() * 2.55, 0, 255));
            u8 g_val = lround(clamp(params[1].percentage() * 2.55, 0, 255));
            u8 b_val = lround(clamp(params[2].percentage() * 2.55, 0, 255));

            return Color(r_val, g_val, b_val, a_val);
        }
    } else if (function_name.equals_ignoring_ascii_case("hsl"sv)
        || function_name.equals_ignoring_ascii_case("hsla"sv)) {

        // https://www.w3.org/TR/css-color-4/#the-hsl-notation

        auto a_val = 1.0;
        if (params[3].is(Token::Type::Number))
            a_val = params[3].number_value();
        else if (params[3].is(Token::Type::Percentage))
            a_val = params[3].percentage() / 100.0;

        if (params[0].is(Token::Type::Dimension)
            && params[1].is(Token::Type::Percentage)
            && params[2].is(Token::Type::Percentage)) {

            auto numeric_value = params[0].dimension_value();
            auto unit_string = params[0].dimension_unit();
            auto angle_type = Angle::unit_from_name(unit_string);

            if (!angle_type.has_value())
                return {};

            auto angle = Angle { numeric_value, angle_type.release_value() };

            float h_val = fmod(angle.to_degrees(), 360.0);
            float s_val = params[1].percentage() / 100.0;
            float l_val = params[2].percentage() / 100.0;

            return Color::from_hsla(h_val, s_val, l_val, a_val);
        }

        if (params[0].is(Token::Type::Number)
            && params[1].is(Token::Type::Percentage)
            && params[2].is(Token::Type::Percentage)) {

            float h_val = fmod(params[0].number_value(), 360.0);
            float s_val = params[1].percentage() / 100.0;
            float l_val = params[2].percentage() / 100.0;

            return Color::from_hsla(h_val, s_val, l_val, a_val);
        }
    }

    return {};
}

// https://www.w3.org/TR/CSS2/visufx.html#value-def-shape
RefPtr<StyleValue> Parser::parse_rect_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto function_token = tokens.next_token();
    if (!function_token.is_function("rect"sv))
        return nullptr;

    Vector<Length, 4> params;
    auto argument_tokens = TokenStream { function_token.function().values() };

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
        argument_tokens.skip_whitespace();

        // <top>, <right>, <bottom>, and <left> may either have a <length> value or 'auto'.
        // Negative lengths are permitted.
        if (argument_tokens.peek_token().is_ident("auto"sv)) {
            (void)argument_tokens.next_token(); // `auto`
            params.append(Length::make_auto());
        } else {
            auto maybe_length = parse_length(argument_tokens);
            if (!maybe_length.has_value())
                return nullptr;
            // FIXME: Support calculated lengths
            params.append(maybe_length.value().value());
        }
        argument_tokens.skip_whitespace();

        // The last side, should be no more tokens following it.
        if (static_cast<Side>(side) == Side::Left) {
            if (argument_tokens.has_next_token())
                return nullptr;
            break;
        }

        bool next_is_comma = argument_tokens.peek_token().is(Token::Type::Comma);

        // Authors should separate offset values with commas. User agents must support separation
        // with commas, but may also support separation without commas (but not a combination),
        // because a previous revision of this specification was ambiguous in this respect.
        if (comma_requirement == CommaRequirement::Unknown)
            comma_requirement = next_is_comma ? CommaRequirement::RequiresCommas : CommaRequirement::RequiresNoCommas;

        if (comma_requirement == CommaRequirement::RequiresCommas) {
            if (next_is_comma)
                argument_tokens.next_token();
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

Optional<Color> Parser::parse_color(ComponentValue const& component_value)
{
    // https://www.w3.org/TR/css-color-4/
    if (component_value.is(Token::Type::Ident)) {
        auto ident = component_value.token().ident();

        auto color = Color::from_string(ident);
        if (color.has_value())
            return color;

    } else if (component_value.is(Token::Type::Hash)) {
        auto color = Color::from_string(MUST(String::formatted("#{}", component_value.token().hash_value())));
        if (color.has_value())
            return color;
        return {};

    } else if (component_value.is_function()) {
        auto const& function = component_value.function();
        auto const& values = function.values();

        return parse_rgb_or_hsl_color(function.name(), values);
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
        return Color::from_string(MUST(String::formatted("#{}", serialization)));
    }

    return {};
}

RefPtr<StyleValue> Parser::parse_color_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto component_value = tokens.next_token();

    if (auto color = parse_color(component_value); color.has_value()) {
        transaction.commit();
        return ColorStyleValue::create(color.value());
    }

    if (component_value.is(Token::Type::Ident)) {
        auto ident = value_id_from_string(component_value.token().ident());
        if (ident.has_value() && IdentifierStyleValue::is_color(ident.value())) {
            transaction.commit();
            return IdentifierStyleValue::create(ident.value());
        }
    }

    return nullptr;
}

RefPtr<StyleValue> Parser::parse_ratio_value(TokenStream<ComponentValue>& tokens)
{
    if (auto ratio = parse_ratio(tokens); ratio.has_value())
        return RatioStyleValue::create(ratio.release_value());
    return nullptr;
}

RefPtr<StyleValue> Parser::parse_string_value(TokenStream<ComponentValue>& tokens)
{
    auto peek = tokens.peek_token();
    if (peek.is(Token::Type::String)) {
        (void)tokens.next_token();
        return StringStyleValue::create(peek.token().string().to_string());
    }

    return nullptr;
}

RefPtr<StyleValue> Parser::parse_image_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto& token = tokens.next_token();

    if (auto url = parse_url_function(token); url.has_value()) {
        transaction.commit();
        return ImageStyleValue::create(url.value());
    }
    if (auto linear_gradient = parse_linear_gradient_function(token)) {
        transaction.commit();
        return linear_gradient;
    }
    if (auto conic_gradient = parse_conic_gradient_function(token)) {
        transaction.commit();
        return conic_gradient;
    }
    if (auto radial_gradient = parse_radial_gradient_function(token)) {
        transaction.commit();
        return radial_gradient;
    }
    return nullptr;
}

// https://svgwg.org/svg2-draft/painting.html#SpecifyingPaint
RefPtr<StyleValue> Parser::parse_paint_value(TokenStream<ComponentValue>& tokens)
{
    // `<paint> = none | <color> | <url> [none | <color>]? | context-fill | context-stroke`

    auto parse_color_or_none = [&]() -> Optional<RefPtr<StyleValue>> {
        if (auto color = parse_color_value(tokens))
            return color;

        // NOTE: <color> also accepts identifiers, so we do this identifier check last.
        if (tokens.peek_token().is(Token::Type::Ident)) {
            auto maybe_ident = value_id_from_string(tokens.peek_token().token().ident());
            if (maybe_ident.has_value()) {
                // FIXME: Accept `context-fill` and `context-stroke`
                switch (*maybe_ident) {
                case ValueID::None:
                    (void)tokens.next_token();
                    return IdentifierStyleValue::create(*maybe_ident);
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
        tokens.skip_whitespace();
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
        auto ident = value_id_from_string(token.token().ident());
        if (!ident.has_value())
            return {};
        return value_id_to_position_edge(*ident);
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

        tokens.skip_whitespace();
        auto const& token = tokens.next_token();

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

        tokens.skip_whitespace();

        // Parse out two position edges
        auto maybe_first_edge = parse_position_edge(tokens.next_token());
        if (!maybe_first_edge.has_value())
            return nullptr;

        auto first_edge = maybe_first_edge.release_value();
        tokens.skip_whitespace();

        auto maybe_second_edge = parse_position_edge(tokens.next_token());
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
            tokens.skip_whitespace();
            auto const& token = tokens.next_token();

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
            tokens.skip_whitespace();

            auto maybe_position = parse_position_edge(tokens.next_token());
            if (!maybe_position.has_value())
                return {};

            tokens.skip_whitespace();

            auto maybe_length = parse_length_percentage(tokens.next_token());
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
            tokens.skip_whitespace();

            auto maybe_position = parse_position_edge(tokens.next_token());
            if (!maybe_position.has_value())
                return {};

            tokens.skip_whitespace();

            auto maybe_length = parse_length_percentage(tokens.peek_token());
            if (maybe_length.has_value()) {
                // 'center' cannot be followed by a <length-percentage>
                if (maybe_position.value() == PositionEdge::Center && maybe_length.has_value())
                    return {};
                tokens.next_token();
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
RefPtr<StyleValue> Parser::parse_comma_separated_value_list(TokenStream<ComponentValue>& tokens, ParseFunction parse_one_value)
{
    auto first = parse_one_value(tokens);
    if (!first || !tokens.has_next_token())
        return first;

    StyleValueVector values;
    values.append(first.release_nonnull());

    while (tokens.has_next_token()) {
        if (!tokens.next_token().is(Token::Type::Comma))
            return nullptr;

        if (auto maybe_value = parse_one_value(tokens)) {
            values.append(maybe_value.release_nonnull());
            continue;
        }
        return nullptr;
    }

    return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
}

RefPtr<StyleValue> Parser::parse_simple_comma_separated_value_list(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    return parse_comma_separated_value_list(tokens, [this, property_id](auto& tokens) -> RefPtr<StyleValue> {
        if (auto value = parse_css_value_for_property(property_id, tokens))
            return value;
        tokens.reconsume_current_input_token();
        return nullptr;
    });
}

static void remove_property(Vector<PropertyID>& properties, PropertyID property_to_remove)
{
    properties.remove_first_matching([&](auto it) { return it == property_to_remove; });
}

// https://www.w3.org/TR/css-sizing-4/#aspect-ratio
RefPtr<StyleValue> Parser::parse_aspect_ratio_value(TokenStream<ComponentValue>& tokens)
{
    // `auto || <ratio>`
    RefPtr<StyleValue> auto_value;
    RefPtr<StyleValue> ratio_value;

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

        if (maybe_value->is_identifier() && maybe_value->as_identifier().id() == ValueID::Auto) {
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

RefPtr<StyleValue> Parser::parse_background_value(TokenStream<ComponentValue>& tokens)
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
    RefPtr<StyleValue> background_color;

    auto initial_background_image = property_initial_value(m_context.realm(), PropertyID::BackgroundImage);
    auto initial_background_position = property_initial_value(m_context.realm(), PropertyID::BackgroundPosition);
    auto initial_background_size = property_initial_value(m_context.realm(), PropertyID::BackgroundSize);
    auto initial_background_repeat = property_initial_value(m_context.realm(), PropertyID::BackgroundRepeat);
    auto initial_background_attachment = property_initial_value(m_context.realm(), PropertyID::BackgroundAttachment);
    auto initial_background_clip = property_initial_value(m_context.realm(), PropertyID::BackgroundClip);
    auto initial_background_origin = property_initial_value(m_context.realm(), PropertyID::BackgroundOrigin);
    auto initial_background_color = property_initial_value(m_context.realm(), PropertyID::BackgroundColor);

    // Per-layer values
    RefPtr<StyleValue> background_image;
    RefPtr<StyleValue> background_position;
    RefPtr<StyleValue> background_size;
    RefPtr<StyleValue> background_repeat;
    RefPtr<StyleValue> background_attachment;
    RefPtr<StyleValue> background_clip;
    RefPtr<StyleValue> background_origin;

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
        if (tokens.peek_token().is(Token::Type::Comma)) {
            has_multiple_layers = true;
            if (!background_layer_is_valid(false))
                return nullptr;
            complete_background_layer();
            (void)tokens.next_token();
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
            auto& maybe_slash = tokens.next_token();
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
    if (value->is_calculated())
        return LengthPercentage { value->as_calculated() };
    return {};
}

RefPtr<StyleValue> Parser::parse_single_background_position_x_or_y_value(TokenStream<ComponentValue>& tokens, PropertyID property)
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

    if (value->is_identifier()) {
        auto identifier = value->to_identifier();
        if (identifier == ValueID::Center) {
            transaction.commit();
            return EdgeStyleValue::create(relative_edge, Percentage { 50 });
        }
        if (auto edge = value_id_to_position_edge(identifier); edge.has_value()) {
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

RefPtr<StyleValue> Parser::parse_single_background_repeat_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto is_directional_repeat = [](StyleValue const& value) -> bool {
        auto value_id = value.to_identifier();
        return value_id == ValueID::RepeatX || value_id == ValueID::RepeatY;
    };

    auto as_repeat = [](ValueID identifier) -> Optional<Repeat> {
        switch (identifier) {
        case ValueID::NoRepeat:
            return Repeat::NoRepeat;
        case ValueID::Repeat:
            return Repeat::Repeat;
        case ValueID::Round:
            return Repeat::Round;
        case ValueID::Space:
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
        auto value_id = x_value->to_identifier();
        transaction.commit();
        return BackgroundRepeatStyleValue::create(
            value_id == ValueID::RepeatX ? Repeat::Repeat : Repeat::NoRepeat,
            value_id == ValueID::RepeatX ? Repeat::NoRepeat : Repeat::Repeat);
    }

    auto x_repeat = as_repeat(x_value->to_identifier());
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

    auto y_repeat = as_repeat(y_value->to_identifier());
    if (!y_repeat.has_value())
        return nullptr;

    transaction.commit();
    return BackgroundRepeatStyleValue::create(x_repeat.value(), y_repeat.value());
}

RefPtr<StyleValue> Parser::parse_single_background_size_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto get_length_percentage = [](StyleValue& style_value) -> Optional<LengthPercentage> {
        if (style_value.has_auto())
            return LengthPercentage { Length::make_auto() };
        if (style_value.is_percentage())
            return LengthPercentage { style_value.as_percentage().percentage() };
        if (style_value.is_length())
            return LengthPercentage { style_value.as_length().length() };
        if (style_value.is_calculated())
            return LengthPercentage { style_value.as_calculated() };
        return {};
    };

    auto maybe_x_value = parse_css_value_for_property(PropertyID::BackgroundSize, tokens);
    if (!maybe_x_value)
        return nullptr;
    auto x_value = maybe_x_value.release_nonnull();

    if (x_value->to_identifier() == ValueID::Cover || x_value->to_identifier() == ValueID::Contain) {
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

RefPtr<StyleValue> Parser::parse_border_value(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    RefPtr<StyleValue> border_width;
    RefPtr<StyleValue> border_color;
    RefPtr<StyleValue> border_style;

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

RefPtr<StyleValue> Parser::parse_border_radius_value(TokenStream<ComponentValue>& tokens)
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

RefPtr<StyleValue> Parser::parse_border_radius_shorthand_value(TokenStream<ComponentValue>& tokens)
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
        if (tokens.peek_token().is_delim('/')) {
            if (reading_vertical || horizontal_radii.is_empty())
                return nullptr;

            reading_vertical = true;
            (void)tokens.next_token(); // `/`
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

RefPtr<StyleValue> Parser::parse_shadow_value(TokenStream<ComponentValue>& tokens, AllowInsetKeyword allow_inset_keyword)
{
    // "none"
    if (contains_single_none_ident(tokens))
        return parse_identifier_value(tokens);

    return parse_comma_separated_value_list(tokens, [this, allow_inset_keyword](auto& tokens) {
        return parse_single_shadow_value(tokens, allow_inset_keyword);
    });
}

RefPtr<StyleValue> Parser::parse_single_shadow_value(TokenStream<ComponentValue>& tokens, AllowInsetKeyword allow_inset_keyword)
{
    auto transaction = tokens.begin_transaction();

    Optional<Color> color;
    RefPtr<StyleValue> offset_x;
    RefPtr<StyleValue> offset_y;
    RefPtr<StyleValue> blur_radius;
    RefPtr<StyleValue> spread_distance;
    Optional<ShadowPlacement> placement;

    auto possibly_dynamic_length = [&](ComponentValue const& token) -> RefPtr<StyleValue> {
        auto tokens = TokenStream<ComponentValue>::of_single_token(token);
        auto maybe_length = parse_length(tokens);
        if (!maybe_length.has_value())
            return nullptr;
        return maybe_length->as_style_value();
    };

    while (tokens.has_next_token()) {
        auto const& token = tokens.peek_token();

        if (auto maybe_color = parse_color(token); maybe_color.has_value()) {
            if (color.has_value())
                return nullptr;
            color = maybe_color.release_value();
            tokens.next_token();
            continue;
        }

        if (auto maybe_offset_x = possibly_dynamic_length(token); maybe_offset_x) {
            // horizontal offset
            if (offset_x)
                return nullptr;
            offset_x = maybe_offset_x;
            tokens.next_token();

            // vertical offset
            if (!tokens.has_next_token())
                return nullptr;
            auto maybe_offset_y = possibly_dynamic_length(tokens.peek_token());
            if (!maybe_offset_y)
                return nullptr;
            offset_y = maybe_offset_y;
            tokens.next_token();

            // blur radius (optional)
            if (!tokens.has_next_token())
                break;
            auto maybe_blur_radius = possibly_dynamic_length(tokens.peek_token());
            if (!maybe_blur_radius)
                continue;
            blur_radius = maybe_blur_radius;
            tokens.next_token();

            // spread distance (optional)
            if (!tokens.has_next_token())
                break;
            auto maybe_spread_distance = possibly_dynamic_length(tokens.peek_token());
            if (!maybe_spread_distance)
                continue;
            spread_distance = maybe_spread_distance;
            tokens.next_token();

            continue;
        }

        if (allow_inset_keyword == AllowInsetKeyword::Yes && token.is_ident("inset"sv)) {
            if (placement.has_value())
                return nullptr;
            placement = ShadowPlacement::Inner;
            tokens.next_token();
            continue;
        }

        if (token.is(Token::Type::Comma))
            break;

        return nullptr;
    }

    // FIXME: If color is absent, default to `currentColor`
    if (!color.has_value())
        color = Color::NamedColor::Black;

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
    return ShadowStyleValue::create(color.release_value(), offset_x.release_nonnull(), offset_y.release_nonnull(), blur_radius.release_nonnull(), spread_distance.release_nonnull(), placement.release_value());
}

RefPtr<StyleValue> Parser::parse_content_value(TokenStream<ComponentValue>& tokens)
{
    // FIXME: `content` accepts several kinds of function() type, which we don't handle in property_accepts_value() yet.

    auto is_single_value_identifier = [](ValueID identifier) -> bool {
        switch (identifier) {
        case ValueID::None:
        case ValueID::Normal:
            return true;
        default:
            return false;
        }
    };

    if (tokens.remaining_token_count() == 1) {
        auto transaction = tokens.begin_transaction();
        if (auto identifier = parse_identifier_value(tokens)) {
            if (is_single_value_identifier(identifier->to_identifier())) {
                transaction.commit();
                return identifier;
            }
        }
    }

    auto transaction = tokens.begin_transaction();

    StyleValueVector content_values;
    StyleValueVector alt_text_values;
    bool in_alt_text = false;

    while (tokens.has_next_token()) {
        auto& next = tokens.peek_token();
        if (next.is_delim('/')) {
            if (in_alt_text || content_values.is_empty())
                return nullptr;
            in_alt_text = true;
            (void)tokens.next_token();
            continue;
        }

        if (auto style_value = parse_css_value_for_property(PropertyID::Content, tokens)) {
            if (is_single_value_identifier(style_value->to_identifier()))
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

// https://www.w3.org/TR/css-display-3/#the-display-properties
RefPtr<StyleValue> Parser::parse_display_value(TokenStream<ComponentValue>& tokens)
{
    auto parse_single_component_display = [this](TokenStream<ComponentValue>& tokens) -> Optional<Display> {
        auto transaction = tokens.begin_transaction();
        if (auto identifier_value = parse_identifier_value(tokens)) {
            auto identifier = identifier_value->to_identifier();
            if (identifier == ValueID::ListItem) {
                transaction.commit();
                return Display::from_short(Display::Short::ListItem);
            }

            if (auto display_outside = value_id_to_display_outside(identifier); display_outside.has_value()) {
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

            if (auto display_inside = value_id_to_display_inside(identifier); display_inside.has_value()) {
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

            if (auto display_internal = value_id_to_display_internal(identifier); display_internal.has_value()) {
                transaction.commit();
                return Display { display_internal.value() };
            }

            if (auto display_box = value_id_to_display_box(identifier); display_box.has_value()) {
                transaction.commit();
                switch (display_box.value()) {
                case DisplayBox::Contents:
                    return Display::from_short(Display::Short::Contents);
                case DisplayBox::None:
                    return Display::from_short(Display::Short::None);
                }
            }

            if (auto display_legacy = value_id_to_display_legacy(identifier); display_legacy.has_value()) {
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
            if (auto value = parse_identifier_value(tokens)) {
                auto identifier = value->to_identifier();
                if (identifier == ValueID::ListItem) {
                    if (list_item == Display::ListItem::Yes)
                        return {};
                    list_item = Display::ListItem::Yes;
                    continue;
                }
                if (auto inside_value = value_id_to_display_inside(identifier); inside_value.has_value()) {
                    if (inside.has_value())
                        return {};
                    inside = inside_value.value();
                    continue;
                }
                if (auto outside_value = value_id_to_display_outside(identifier); outside_value.has_value()) {
                    if (outside.has_value())
                        return {};
                    outside = outside_value.value();
                    continue;
                }
            }

            // Not a display value, abort.
            dbgln_if(CSS_PARSER_DEBUG, "Unrecognized display value: `{}`", tokens.peek_token().to_string());
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

RefPtr<StyleValue> Parser::parse_filter_value_list_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    if (contains_single_none_ident(tokens)) {
        transaction.commit();
        return parse_identifier_value(tokens);
    }

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
        return static_cast<Filter::Color::Operation>(filter);
    };

    auto parse_number_percentage = [&](auto& token) -> Optional<NumberPercentage> {
        if (token.is(Token::Type::Percentage))
            return NumberPercentage(Percentage(token.token().percentage()));
        if (token.is(Token::Type::Number))
            return NumberPercentage(Number(Number::Type::Number, token.token().number_value()));
        return {};
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

    auto parse_filter_function = [&](auto filter_token, auto function_values) -> Optional<FilterFunction> {
        TokenStream tokens { function_values };
        tokens.skip_whitespace();

        auto if_no_more_tokens_return = [&](auto filter) -> Optional<FilterFunction> {
            tokens.skip_whitespace();
            if (tokens.has_next_token())
                return {};
            return filter;
        };

        if (filter_token == FilterToken::Blur) {
            // blur( <length>? )
            if (!tokens.has_next_token())
                return Filter::Blur {};
            auto blur_radius = parse_length(tokens);
            tokens.skip_whitespace();
            if (!blur_radius.has_value())
                return {};
            // FIXME: Support calculated radius
            return if_no_more_tokens_return(Filter::Blur { blur_radius->value() });
        } else if (filter_token == FilterToken::DropShadow) {
            if (!tokens.has_next_token())
                return {};
            // drop-shadow( [ <color>? && <length>{2,3} ] )
            // Note: The following code is a little awkward to allow the color to be before or after the lengths.
            Optional<LengthOrCalculated> maybe_radius = {};
            auto maybe_color = parse_color(tokens.peek_token());
            if (maybe_color.has_value())
                (void)tokens.next_token();
            auto x_offset = parse_length(tokens);
            tokens.skip_whitespace();
            if (!x_offset.has_value() || !tokens.has_next_token()) {
                return {};
            }
            auto y_offset = parse_length(tokens);
            if (!y_offset.has_value()) {
                return {};
            }
            if (tokens.has_next_token()) {
                maybe_radius = parse_length(tokens);
                if (!maybe_color.has_value() && (!maybe_radius.has_value() || tokens.has_next_token())) {
                    maybe_color = parse_color(tokens.next_token());
                    tokens.skip_whitespace();
                    if (!maybe_color.has_value()) {
                        return {};
                    }
                } else if (!maybe_radius.has_value()) {
                    return {};
                }
            }
            // FIXME: Support calculated offsets and radius
            return if_no_more_tokens_return(Filter::DropShadow { x_offset->value(), y_offset->value(), maybe_radius.map([](auto& it) { return it.value(); }), maybe_color });
        } else if (filter_token == FilterToken::HueRotate) {
            // hue-rotate( [ <angle> | <zero> ]? )
            if (!tokens.has_next_token())
                return Filter::HueRotate {};
            auto& token = tokens.next_token();
            if (token.is(Token::Type::Number)) {
                // hue-rotate(0)
                auto number = token.token().number();
                if (number.is_integer() && number.integer_value() == 0)
                    return if_no_more_tokens_return(Filter::HueRotate { Filter::HueRotate::Zero {} });
                return {};
            }
            if (!token.is(Token::Type::Dimension))
                return {};
            auto angle_value = token.token().dimension_value();
            auto angle_unit_name = token.token().dimension_unit();
            auto angle_unit = Angle::unit_from_name(angle_unit_name);
            if (!angle_unit.has_value())
                return {};
            Angle angle { angle_value, angle_unit.release_value() };
            return if_no_more_tokens_return(Filter::HueRotate { angle });
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
                return Filter::Color { filter_token_to_operation(filter_token) };
            auto amount = parse_number_percentage(tokens.next_token());
            if (!amount.has_value())
                return {};
            return if_no_more_tokens_return(Filter::Color { filter_token_to_operation(filter_token), *amount });
        }
    };

    Vector<FilterFunction> filter_value_list {};

    while (tokens.has_next_token()) {
        tokens.skip_whitespace();
        if (!tokens.has_next_token())
            break;
        auto& token = tokens.next_token();
        if (!token.is_function())
            return nullptr;
        auto filter_token = parse_filter_function_name(token.function().name());
        if (!filter_token.has_value())
            return nullptr;
        auto filter_function = parse_filter_function(*filter_token, token.function().values());
        if (!filter_function.has_value())
            return nullptr;
        filter_value_list.append(*filter_function);
    }

    if (filter_value_list.is_empty())
        return nullptr;

    transaction.commit();
    return FilterValueListStyleValue::create(move(filter_value_list));
}

RefPtr<StyleValue> Parser::parse_flex_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto make_flex_shorthand = [&](NonnullRefPtr<StyleValue> flex_grow, NonnullRefPtr<StyleValue> flex_shrink, NonnullRefPtr<StyleValue> flex_basis) {
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
            if (value->is_identifier() && value->to_identifier() == ValueID::None) {
                auto zero = NumberStyleValue::create(0);
                return make_flex_shorthand(zero, zero, IdentifierStyleValue::create(ValueID::Auto));
            }
            break;
        }
        default:
            VERIFY_NOT_REACHED();
        }

        return nullptr;
    }

    RefPtr<StyleValue> flex_grow;
    RefPtr<StyleValue> flex_shrink;
    RefPtr<StyleValue> flex_basis;

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

RefPtr<StyleValue> Parser::parse_flex_flow_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<StyleValue> flex_direction;
    RefPtr<StyleValue> flex_wrap;

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

static bool is_generic_font_family(ValueID identifier)
{
    switch (identifier) {
    case ValueID::Cursive:
    case ValueID::Fantasy:
    case ValueID::Monospace:
    case ValueID::Serif:
    case ValueID::SansSerif:
    case ValueID::UiMonospace:
    case ValueID::UiRounded:
    case ValueID::UiSerif:
    case ValueID::UiSansSerif:
        return true;
    default:
        return false;
    }
}

RefPtr<StyleValue> Parser::parse_font_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<StyleValue> font_stretch;
    RefPtr<StyleValue> font_style;
    RefPtr<StyleValue> font_weight;
    RefPtr<StyleValue> font_size;
    RefPtr<StyleValue> line_height;
    RefPtr<StyleValue> font_families;
    RefPtr<StyleValue> font_variant;

    // FIXME: Handle system fonts. (caption, icon, menu, message-box, small-caption, status-bar)

    // Several sub-properties can be "normal", and appear in any order: style, variant, weight, stretch
    // So, we have to handle that separately.
    int normal_count = 0;

    auto remaining_longhands = Vector { PropertyID::FontSize, PropertyID::FontStretch, PropertyID::FontStyle, PropertyID::FontVariant, PropertyID::FontWeight };
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        auto& peek_token = tokens.peek_token();
        if (peek_token.is_ident("normal"sv)) {
            normal_count++;
            (void)tokens.next_token();
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
            if (tokens.peek_token().is_delim('/')) {
                (void)tokens.next_token();
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
        case PropertyID::FontStretch: {
            VERIFY(!font_stretch);
            font_stretch = value.release_nonnull();
            continue;
        }
        case PropertyID::FontStyle: {
            // FIXME: Handle angle parameter to `oblique`: https://www.w3.org/TR/css-fonts-4/#font-style-prop
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
    int unset_value_count = (font_style ? 0 : 1) + (font_weight ? 0 : 1) + (font_variant ? 0 : 1) + (font_stretch ? 0 : 1);
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
    if (!font_stretch)
        font_stretch = property_initial_value(m_context.realm(), PropertyID::FontStretch);
    if (!line_height)
        line_height = property_initial_value(m_context.realm(), PropertyID::LineHeight);

    transaction.commit();
    return ShorthandStyleValue::create(PropertyID::Font,
        { PropertyID::FontStyle, PropertyID::FontVariant, PropertyID::FontWeight, PropertyID::FontStretch, PropertyID::FontSize, PropertyID::LineHeight, PropertyID::FontFamily },
        { font_style.release_nonnull(), font_variant.release_nonnull(), font_weight.release_nonnull(), font_stretch.release_nonnull(), font_size.release_nonnull(), line_height.release_nonnull(), font_families.release_nonnull() });
}

RefPtr<StyleValue> Parser::parse_font_family_value(TokenStream<ComponentValue>& tokens)
{
    auto next_is_comma_or_eof = [&]() -> bool {
        return !tokens.has_next_token() || tokens.peek_token().is(Token::Type::Comma);
    };

    // Note: Font-family names can either be a quoted string, or a keyword, or a series of custom-idents.
    // eg, these are equivalent:
    //     font-family: my cool     font\!, serif;
    //     font-family: "my cool font!", serif;
    StyleValueVector font_families;
    Vector<String> current_name_parts;
    while (tokens.has_next_token()) {
        auto const& peek = tokens.peek_token();

        if (peek.is(Token::Type::String)) {
            // `font-family: my cool "font";` is invalid.
            if (!current_name_parts.is_empty())
                return nullptr;
            (void)tokens.next_token(); // String
            if (!next_is_comma_or_eof())
                return nullptr;
            font_families.append(StringStyleValue::create(peek.token().string().to_string()));
            (void)tokens.next_token(); // Comma
            continue;
        }

        if (peek.is(Token::Type::Ident)) {
            // If this is a valid identifier, it's NOT a custom-ident and can't be part of a larger name.

            // CSS-wide keywords are not allowed
            if (auto builtin = parse_builtin_value(peek))
                return nullptr;

            auto maybe_ident = value_id_from_string(peek.token().ident());
            // Can't have a generic-font-name as a token in an unquoted font name.
            if (maybe_ident.has_value() && is_generic_font_family(maybe_ident.value())) {
                if (!current_name_parts.is_empty())
                    return nullptr;
                (void)tokens.next_token(); // Ident
                if (!next_is_comma_or_eof())
                    return nullptr;
                font_families.append(IdentifierStyleValue::create(maybe_ident.value()));
                (void)tokens.next_token(); // Comma
                continue;
            }
            current_name_parts.append(tokens.next_token().token().ident().to_string());
            continue;
        }

        if (peek.is(Token::Type::Comma)) {
            if (current_name_parts.is_empty())
                return nullptr;
            (void)tokens.next_token(); // Comma
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

CSSRule* Parser::parse_font_face_rule(TokenStream<ComponentValue>& tokens)
{
    auto declarations_and_at_rules = parse_a_list_of_declarations(tokens);

    Optional<FlyString> font_family;
    Vector<ParsedFontFace::Source> src;
    Vector<Gfx::UnicodeRange> unicode_range;
    Optional<int> weight;
    Optional<int> slope;

    for (auto& declaration_or_at_rule : declarations_and_at_rules) {
        if (declaration_or_at_rule.is_at_rule()) {
            dbgln_if(CSS_PARSER_DEBUG, "CSSParser: CSS at-rules are not allowed in @font-face; discarding.");
            continue;
        }

        auto const& declaration = declaration_or_at_rule.declaration();
        if (declaration.name().equals_ignoring_ascii_case("font-weight"sv)) {
            TokenStream token_stream { declaration.values() };
            if (auto value = parse_css_value(CSS::PropertyID::FontWeight, token_stream); !value.is_error()) {
                weight = value.value()->to_font_weight();
            }
            continue;
        }
        if (declaration.name().equals_ignoring_ascii_case("font-style"sv)) {
            TokenStream token_stream { declaration.values() };
            if (auto value = parse_css_value(CSS::PropertyID::FontStyle, token_stream); !value.is_error()) {
                slope = value.value()->to_font_slope();
            }
            continue;
        }
        if (declaration.name().equals_ignoring_ascii_case("font-family"sv)) {
            // FIXME: This is very similar to, but different from, the logic in parse_font_family_value().
            //        Ideally they could share code.
            Vector<FlyString> font_family_parts;
            bool had_syntax_error = false;
            for (size_t i = 0; i < declaration.values().size(); ++i) {
                auto const& part = declaration.values()[i];
                if (part.is(Token::Type::Whitespace))
                    continue;
                if (part.is(Token::Type::String)) {
                    if (!font_family_parts.is_empty()) {
                        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face font-family format invalid; discarding.");
                        had_syntax_error = true;
                        break;
                    }
                    font_family_parts.append(part.token().string());
                    continue;
                }
                if (part.is(Token::Type::Ident)) {
                    if (is_css_wide_keyword(part.token().ident())) {
                        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face font-family format invalid; discarding.");
                        had_syntax_error = true;
                        break;
                    }
                    auto value_id = value_id_from_string(part.token().ident());
                    if (value_id.has_value() && is_generic_font_family(value_id.value())) {
                        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face font-family format invalid; discarding.");
                        had_syntax_error = true;
                        break;
                    }
                    font_family_parts.append(part.token().ident());
                    continue;
                }

                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face font-family format invalid; discarding.");
                had_syntax_error = true;
                break;
            }
            if (had_syntax_error || font_family_parts.is_empty())
                continue;

            font_family = String::join(' ', font_family_parts).release_value_but_fixme_should_propagate_errors();
            continue;
        }
        if (declaration.name().equals_ignoring_ascii_case("src"sv)) {
            TokenStream token_stream { declaration.values() };
            Vector<ParsedFontFace::Source> supported_sources = parse_font_face_src(token_stream);
            if (!supported_sources.is_empty())
                src = move(supported_sources);
            continue;
        }
        if (declaration.name().equals_ignoring_ascii_case("unicode-range"sv)) {
            TokenStream token_stream { declaration.values() };
            auto unicode_ranges = parse_unicode_ranges(token_stream);
            if (unicode_ranges.is_empty())
                continue;

            unicode_range = move(unicode_ranges);
            continue;
        }

        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Unrecognized descriptor '{}' in @font-face; discarding.", declaration.name());
    }

    if (!font_family.has_value()) {
        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: Failed to parse @font-face: no font-family!");
        return {};
    }

    if (unicode_range.is_empty()) {
        unicode_range.empend(0x0u, 0x10FFFFu);
    }

    return CSSFontFaceRule::create(m_context.realm(), ParsedFontFace { font_family.release_value(), weight, slope, move(src), move(unicode_range) });
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
        source_tokens.skip_whitespace();
        auto const& first = source_tokens.next_token();

        // <url> [ format(<font-format>)]?
        // FIXME: Implement optional tech() function from CSS-Fonts-4.
        if (auto maybe_url = parse_url_function(first); maybe_url.has_value()) {
            auto url = maybe_url.release_value();
            if (!url.is_valid()) {
                continue;
            }

            Optional<FlyString> format;

            source_tokens.skip_whitespace();
            if (!source_tokens.has_next_token()) {
                supported_sources.empend(move(url), format);
                continue;
            }

            auto maybe_function = source_tokens.next_token();
            if (!maybe_function.is_function()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (token after `url()` that isn't a function: {}); discarding.", maybe_function.to_debug_string());
                return {};
            }

            auto const& function = maybe_function.function();
            if (function.name().equals_ignoring_ascii_case("format"sv)) {
                TokenStream format_tokens { function.values() };
                format_tokens.skip_whitespace();
                auto const& format_name_token = format_tokens.next_token();
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
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (unrecognized function token `{}`); discarding.", function.name());
                return {};
            }

            source_tokens.skip_whitespace();
            if (source_tokens.has_next_token()) {
                dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (extra token `{}`); discarding.", source_tokens.peek_token().to_debug_string());
                return {};
            }

            supported_sources.empend(move(url), format);
            continue;
        }

        if (first.is_function("local"sv)) {
            if (first.function().values().is_empty()) {
                continue;
            }
            supported_sources.empend(first.function().values().first().to_string(), Optional<FlyString> {});
            continue;
        }

        dbgln_if(CSS_PARSER_DEBUG, "CSSParser: @font-face src invalid (failed to parse url from: {}); discarding.", first.to_debug_string());
        return {};
    }

    return supported_sources;
}

RefPtr<StyleValue> Parser::parse_list_style_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<StyleValue> list_position;
    RefPtr<StyleValue> list_image;
    RefPtr<StyleValue> list_type;
    int found_nones = 0;

    Vector<PropertyID> remaining_longhands { PropertyID::ListStyleImage, PropertyID::ListStylePosition, PropertyID::ListStyleType };

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        if (auto peek = tokens.peek_token(); peek.is_ident("none"sv)) {
            (void)tokens.next_token();
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
        auto none = IdentifierStyleValue::create(ValueID::None);
        list_image = none;
        list_type = none;

    } else if (found_nones == 1) {
        if (list_image && list_type)
            return nullptr;
        auto none = IdentifierStyleValue::create(ValueID::None);
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

RefPtr<StyleValue> Parser::parse_math_depth_value(TokenStream<ComponentValue>& tokens)
{
    // https://w3c.github.io/mathml-core/#propdef-math-depth
    // auto-add | add(<integer>) | <integer>
    auto transaction = tokens.begin_transaction();

    auto token = tokens.next_token();
    if (tokens.has_next_token())
        return nullptr;

    // auto-add
    if (token.is_ident("auto-add"sv)) {
        transaction.commit();
        return MathDepthStyleValue::create_auto_add();
    }

    // FIXME: Make it easier to parse "thing that might be <bar> or literally anything that resolves to it" and get rid of this
    auto parse_something_that_resolves_to_integer = [this](ComponentValue& token) -> RefPtr<StyleValue> {
        if (token.is(Token::Type::Number) && token.token().number().is_integer())
            return IntegerStyleValue::create(token.token().to_integer());
        if (auto value = parse_calculated_value(token); value && value->resolves_to_number())
            return value;
        return nullptr;
    };

    // add(<integer>)
    if (token.is_function("add"sv)) {
        auto add_tokens = TokenStream { token.function().values() };
        add_tokens.skip_whitespace();
        auto integer_token = add_tokens.next_token();
        add_tokens.skip_whitespace();
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

RefPtr<StyleValue> Parser::parse_overflow_value(TokenStream<ComponentValue>& tokens)
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

RefPtr<StyleValue> Parser::parse_place_content_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_align_content_value = parse_css_value_for_property(PropertyID::AlignContent, tokens);
    if (!maybe_align_content_value)
        return nullptr;

    if (!tokens.has_next_token()) {
        if (!property_accepts_identifier(PropertyID::JustifyContent, maybe_align_content_value->to_identifier()))
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

RefPtr<StyleValue> Parser::parse_place_items_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_align_items_value = parse_css_value_for_property(PropertyID::AlignItems, tokens);
    if (!maybe_align_items_value)
        return nullptr;

    if (!tokens.has_next_token()) {
        if (!property_accepts_identifier(PropertyID::JustifyItems, maybe_align_items_value->to_identifier()))
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

RefPtr<StyleValue> Parser::parse_place_self_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();
    auto maybe_align_self_value = parse_css_value_for_property(PropertyID::AlignSelf, tokens);
    if (!maybe_align_self_value)
        return nullptr;

    if (!tokens.has_next_token()) {
        if (!property_accepts_identifier(PropertyID::JustifySelf, maybe_align_self_value->to_identifier()))
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

RefPtr<StyleValue> Parser::parse_quotes_value(TokenStream<ComponentValue>& tokens)
{
    // https://www.w3.org/TR/css-content-3/#quotes-property
    // auto | none | [ <string> <string> ]+
    auto transaction = tokens.begin_transaction();

    if (tokens.remaining_token_count() == 1) {
        auto identifier = parse_identifier_value(tokens);
        if (identifier && property_accepts_identifier(PropertyID::Quotes, identifier->to_identifier())) {
            transaction.commit();
            return identifier;
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

RefPtr<StyleValue> Parser::parse_text_decoration_value(TokenStream<ComponentValue>& tokens)
{
    RefPtr<StyleValue> decoration_line;
    RefPtr<StyleValue> decoration_thickness;
    RefPtr<StyleValue> decoration_style;
    RefPtr<StyleValue> decoration_color;

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

RefPtr<StyleValue> Parser::parse_text_decoration_line_value(TokenStream<ComponentValue>& tokens)
{
    StyleValueVector style_values;

    while (tokens.has_next_token()) {
        auto maybe_value = parse_css_value_for_property(PropertyID::TextDecorationLine, tokens);
        if (!maybe_value)
            break;
        auto value = maybe_value.release_nonnull();

        if (auto maybe_line = value_id_to_text_decoration_line(value->to_identifier()); maybe_line.has_value()) {
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

RefPtr<StyleValue> Parser::parse_easing_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    tokens.skip_whitespace();

    auto const& part = tokens.next_token();

    if (part.is(Token::Type::Ident)) {
        auto name = part.token().ident();
        auto maybe_simple_easing = [&] -> RefPtr<EasingStyleValue> {
            if (name == "linear"sv)
                return EasingStyleValue::create(EasingStyleValue::Linear {});
            if (name == "ease"sv)
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease());
            if (name == "ease-in"sv)
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_in());
            if (name == "ease-out"sv)
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_out());
            if (name == "ease-in-out"sv)
                return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease_in_out());
            if (name == "step-start"sv)
                return EasingStyleValue::create(EasingStyleValue::Steps::step_start());
            if (name == "step-end"sv)
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

    TokenStream argument_tokens { part.function().values() };
    auto comma_separated_arguments = parse_a_comma_separated_list_of_component_values(argument_tokens);

    // Remove whitespace
    for (auto& argument : comma_separated_arguments)
        argument.remove_all_matching([](auto& value) { return value.is(Token::Type::Whitespace); });

    auto name = part.function().name();
    if (name == "linear"sv) {
        Vector<EasingStyleValue::Linear::Stop> stops;
        for (auto const& argument : comma_separated_arguments) {
            if (argument.is_empty() || argument.size() > 2)
                return nullptr;

            Optional<double> offset;
            Optional<double> position;

            for (auto const& part : argument) {
                if (part.is(Token::Type::Number)) {
                    if (offset.has_value())
                        return nullptr;
                    offset = part.token().number_value();
                } else if (part.is(Token::Type::Percentage)) {
                    if (position.has_value())
                        return nullptr;
                    position = part.token().percentage();
                } else {
                    return nullptr;
                };
            }

            if (!offset.has_value())
                return nullptr;

            stops.append({ offset.value(), move(position) });
        }

        if (stops.is_empty())
            return nullptr;

        transaction.commit();
        return EasingStyleValue::create(EasingStyleValue::Linear { move(stops) });
    }

    if (name == "cubic-bezier") {
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

    if (name == "steps") {
        if (comma_separated_arguments.is_empty() || comma_separated_arguments.size() > 2)
            return nullptr;

        for (auto const& argument : comma_separated_arguments) {
            if (argument.size() != 1)
                return nullptr;
        }

        EasingStyleValue::Steps steps;

        auto intervals_argument = comma_separated_arguments[0][0];
        if (!intervals_argument.is(Token::Type::Number))
            return nullptr;
        if (!intervals_argument.token().number().is_integer())
            return nullptr;
        auto intervals = intervals_argument.token().to_integer();

        if (comma_separated_arguments.size() == 2) {
            TokenStream identifier_stream { comma_separated_arguments[1] };
            auto ident = parse_identifier_value(identifier_stream);
            if (!ident)
                return nullptr;
            switch (ident->to_identifier()) {
            case ValueID::JumpStart:
                steps.position = EasingStyleValue::Steps::Position::JumpStart;
                break;
            case ValueID::JumpEnd:
                steps.position = EasingStyleValue::Steps::Position::JumpEnd;
                break;
            case ValueID::JumpBoth:
                steps.position = EasingStyleValue::Steps::Position::JumpBoth;
                break;
            case ValueID::JumpNone:
                steps.position = EasingStyleValue::Steps::Position::JumpNone;
                break;
            case ValueID::Start:
                steps.position = EasingStyleValue::Steps::Position::Start;
                break;
            case ValueID::End:
                steps.position = EasingStyleValue::Steps::Position::End;
                break;
            default:
                return nullptr;
            }
        }

        // Perform extra validation
        // https://drafts.csswg.org/css-easing/#funcdef-step-easing-function-steps
        // The first parameter specifies the number of intervals in the function. It must be a positive integer greater than 0
        // unless the second parameter is jump-none in which case it must be a positive integer greater than 1.
        if (steps.position == EasingStyleValue::Steps::Position::JumpNone) {
            if (intervals < 1)
                return nullptr;
        } else if (intervals < 0) {
            return nullptr;
        }

        steps.number_of_intervals = intervals;
        transaction.commit();
        return EasingStyleValue::create(steps);
    }

    return nullptr;
}

// https://www.w3.org/TR/css-transforms-1/#transform-property
RefPtr<StyleValue> Parser::parse_transform_value(TokenStream<ComponentValue>& tokens)
{
    // <transform> = none | <transform-list>
    // <transform-list> = <transform-function>+

    if (contains_single_none_ident(tokens)) {
        tokens.next_token(); // none
        return IdentifierStyleValue::create(ValueID::None);
    }

    StyleValueVector transformations;
    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto const& part = tokens.next_token();
        if (!part.is_function())
            return nullptr;
        auto maybe_function = transform_function_from_string(part.function().name());
        if (!maybe_function.has_value())
            return nullptr;
        auto function = maybe_function.release_value();
        auto function_metadata = transform_function_metadata(function);

        auto function_tokens = TokenStream { part.function().values() };
        auto arguments = parse_a_comma_separated_list_of_component_values(function_tokens);

        if (arguments.size() > function_metadata.parameters.size()) {
            dbgln_if(CSS_PARSER_DEBUG, "Too many arguments to {}. max: {}", part.function().name(), function_metadata.parameters.size());
            return nullptr;
        }

        if (arguments.size() < function_metadata.parameters.size() && function_metadata.parameters[arguments.size()].required) {
            dbgln_if(CSS_PARSER_DEBUG, "Required parameter at position {} is missing", arguments.size());
            return nullptr;
        }

        StyleValueVector values;
        for (auto argument_index = 0u; argument_index < arguments.size(); ++argument_index) {
            TokenStream argument_tokens { arguments[argument_index] };
            argument_tokens.skip_whitespace();

            auto const& value = argument_tokens.next_token();
            RefPtr<CalculatedStyleValue> maybe_calc_value = parse_calculated_value(value);

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
                    (void)argument_tokens.next_token(); // calc()
                    values.append(maybe_calc_value.release_nonnull());
                } else {
                    // FIXME: Remove this reconsume once all parsing functions are TokenStream-based.
                    argument_tokens.reconsume_current_input_token();

                    if (function_metadata.parameters[argument_index].type == TransformFunctionParameterType::LengthNone) {
                        auto ident_transaction = argument_tokens.begin_transaction();
                        auto identifier_value = parse_identifier_value(argument_tokens);
                        if (identifier_value && identifier_value->to_identifier() == ValueID::None) {
                            values.append(identifier_value.release_nonnull());
                            ident_transaction.commit();
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
                    auto number_or_percentage = parse_number_or_percentage_value(argument_tokens);
                    if (!number_or_percentage)
                        return nullptr;
                    values.append(number_or_percentage.release_nonnull());
                }
                break;
            }
            }

            argument_tokens.skip_whitespace();
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
RefPtr<StyleValue> Parser::parse_transform_origin_value(TokenStream<ComponentValue>& tokens)
{
    enum class Axis {
        None,
        X,
        Y,
    };

    struct AxisOffset {
        Axis axis;
        NonnullRefPtr<StyleValue> offset;
    };

    auto to_axis_offset = [](RefPtr<StyleValue> value) -> Optional<AxisOffset> {
        if (!value)
            return OptionalNone {};
        if (value->is_percentage())
            return AxisOffset { Axis::None, value->as_percentage() };
        if (value->is_length())
            return AxisOffset { Axis::None, value->as_length() };
        if (value->is_identifier()) {
            switch (value->to_identifier()) {
            case ValueID::Top:
                return AxisOffset { Axis::Y, PercentageStyleValue::create(Percentage(0)) };
            case ValueID::Left:
                return AxisOffset { Axis::X, PercentageStyleValue::create(Percentage(0)) };
            case ValueID::Center:
                return AxisOffset { Axis::None, PercentageStyleValue::create(Percentage(50)) };
            case ValueID::Bottom:
                return AxisOffset { Axis::Y, PercentageStyleValue::create(Percentage(100)) };
            case ValueID::Right:
                return AxisOffset { Axis::X, PercentageStyleValue::create(Percentage(100)) };
            default:
                return OptionalNone {};
            }
        }
        return OptionalNone {};
    };

    auto transaction = tokens.begin_transaction();

    auto make_list = [&transaction](NonnullRefPtr<StyleValue> const& x_value, NonnullRefPtr<StyleValue> const& y_value) -> NonnullRefPtr<StyleValueList> {
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

        RefPtr<StyleValue> x_value;
        RefPtr<StyleValue> y_value;

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

RefPtr<StyleValue> Parser::parse_transition_value(TokenStream<ComponentValue>& tokens)
{
    if (contains_single_none_ident(tokens)) {
        tokens.next_token(); // none
        return IdentifierStyleValue::create(ValueID::None);
    }

    Vector<TransitionStyleValue::Transition> transitions;
    auto transaction = tokens.begin_transaction();

    while (tokens.has_next_token()) {
        TransitionStyleValue::Transition transition;
        auto time_value_count = 0;

        while (tokens.has_next_token() && !tokens.peek_token().is(Token::Type::Comma)) {
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

            if (tokens.peek_token().is(Token::Type::Ident)) {
                if (transition.property_name) {
                    dbgln_if(CSS_PARSER_DEBUG, "Transition property has multiple property identifiers");
                    return {};
                }

                auto ident = tokens.next_token().token().ident();
                if (auto property = property_id_from_string(ident); property.has_value())
                    transition.property_name = CustomIdentStyleValue::create(ident);

                continue;
            }

            dbgln_if(CSS_PARSER_DEBUG, "Transition property has unexpected token \"{}\"", tokens.peek_token().to_string());
            return {};
        }

        if (!transition.property_name)
            transition.property_name = CustomIdentStyleValue::create("all"_fly_string);

        if (!transition.easing)
            transition.easing = EasingStyleValue::create(EasingStyleValue::CubicBezier::ease());

        transitions.append(move(transition));

        if (!tokens.peek_token().is(Token::Type::Comma))
            break;

        tokens.next_token();
    }

    transaction.commit();
    return TransitionStyleValue::create(move(transitions));
}

RefPtr<StyleValue> Parser::parse_as_css_value(PropertyID property_id)
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
    part_one_tokens.skip_whitespace();
    if (!part_one_tokens.has_next_token())
        return {};
    auto current_token = part_one_tokens.next_token();
    auto min_grid_size = parse_grid_size(current_token);

    TokenStream part_two_tokens { comma_separated_list[1] };
    part_two_tokens.skip_whitespace();
    if (!part_two_tokens.has_next_token())
        return {};
    current_token = part_two_tokens.next_token();
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
    part_one_tokens.skip_whitespace();
    if (!part_one_tokens.has_next_token())
        return {};
    auto& current_token = part_one_tokens.next_token();

    auto repeat_count = 0;
    if (current_token.is(Token::Type::Number) && current_token.token().number().is_integer() && current_token.token().number_value() > 0)
        repeat_count = current_token.token().number_value();
    else if (current_token.is_ident("auto-fill"sv))
        is_auto_fill = true;
    else if (current_token.is_ident("auto-fit"sv))
        is_auto_fit = true;

    // The second argument is a track list, which is repeated that number of times.
    TokenStream part_two_tokens { comma_separated_list[1] };
    part_two_tokens.skip_whitespace();
    if (!part_two_tokens.has_next_token())
        return {};

    Vector<Variant<ExplicitGridTrack, GridLineNames>> repeat_params;
    auto last_object_was_line_names = false;
    while (part_two_tokens.has_next_token()) {
        auto token = part_two_tokens.next_token();
        Vector<String> line_names;
        if (token.is_block()) {
            if (last_object_was_line_names)
                return {};
            last_object_was_line_names = true;
            if (!token.block().is_square())
                return {};
            TokenStream block_tokens { token.block().values() };
            while (block_tokens.has_next_token()) {
                auto current_block_token = block_tokens.next_token();
                line_names.append(current_block_token.token().ident().to_string());
                block_tokens.skip_whitespace();
            }
            repeat_params.append(GridLineNames { move(line_names) });
            part_two_tokens.skip_whitespace();
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
            part_two_tokens.skip_whitespace();
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
        if (function_token.name().equals_ignoring_ascii_case("repeat"sv)) {
            auto maybe_repeat = parse_repeat(function_token.values());
            if (maybe_repeat.has_value())
                return CSS::ExplicitGridTrack(maybe_repeat.value());
            else
                return {};
        } else if (function_token.name().equals_ignoring_ascii_case("minmax"sv)) {
            auto maybe_min_max_value = parse_min_max(function_token.values());
            if (maybe_min_max_value.has_value())
                return CSS::ExplicitGridTrack(maybe_min_max_value.value());
            else
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

RefPtr<StyleValue> Parser::parse_grid_track_size_list(TokenStream<ComponentValue>& tokens, bool allow_separate_line_name_blocks)
{
    auto transaction = tokens.begin_transaction();

    if (contains_single_none_ident(tokens)) {
        tokens.next_token();
        transaction.commit();
        return GridTrackSizeListStyleValue::make_none();
    }

    Vector<Variant<ExplicitGridTrack, GridLineNames>> track_list;
    auto last_object_was_line_names = false;
    while (tokens.has_next_token()) {
        auto token = tokens.next_token();
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
            TokenStream block_tokens { token.block().values() };
            block_tokens.skip_whitespace();
            while (block_tokens.has_next_token()) {
                auto current_block_token = block_tokens.next_token();
                line_names.append(current_block_token.token().ident().to_string());
                block_tokens.skip_whitespace();
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
        auto token = tokens.next_token();
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
        auto token = tokens.next_token();
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

RefPtr<StyleValue> Parser::parse_grid_auto_track_sizes(TokenStream<ComponentValue>& tokens)
{
    // https://www.w3.org/TR/css-grid-2/#auto-tracks
    // <track-size>+
    Vector<Variant<ExplicitGridTrack, GridLineNames>> track_list;
    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token()) {
        auto token = tokens.next_token();
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

RefPtr<StyleValue> Parser::parse_grid_track_placement(TokenStream<ComponentValue>& tokens)
{
    // FIXME: This shouldn't be needed. Right now, the below code returns a StyleValue even if no tokens are consumed!
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
    auto is_custom_ident = [](auto& token) -> bool {
        // The <custom-ident> additionally excludes the keywords span and auto.
        if (token.is(Token::Type::Ident) && !token.is_ident("span"sv) && !token.is_ident("auto"sv))
            return true;
        return false;
    };

    auto transaction = tokens.begin_transaction();

    // FIXME: Handle the single-token case inside the loop instead, so that we can more easily call this from
    //        `parse_grid_area_shorthand_value()` using a single TokenStream.
    if (tokens.remaining_token_count() == 1) {
        auto& token = tokens.next_token();
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
        if (is_custom_ident(token)) {
            transaction.commit();
            return GridTrackPlacementStyleValue::create(GridTrackPlacement::make_line({}, token.token().ident().to_string()));
        }
        return nullptr;
    }

    auto span_value = false;
    auto span_or_position_value = 0;
    String identifier_value;
    while (tokens.has_next_token()) {
        auto& token = tokens.peek_token();
        if (token.is_ident("auto"sv))
            return nullptr;
        if (token.is_ident("span"sv)) {
            if (span_value)
                return nullptr;
            (void)tokens.next_token(); // span
            span_value = true;
            continue;
        }
        if (is_valid_integer(token)) {
            if (span_or_position_value != 0)
                return nullptr;
            span_or_position_value = static_cast<int>(tokens.next_token().token().to_integer());
            continue;
        }
        if (is_custom_ident(token)) {
            if (!identifier_value.is_empty())
                return nullptr;
            identifier_value = tokens.next_token().token().ident().to_string();
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

RefPtr<StyleValue> Parser::parse_grid_track_placement_shorthand_value(PropertyID property_id, TokenStream<ComponentValue>& tokens)
{
    auto start_property = (property_id == PropertyID::GridColumn) ? PropertyID::GridColumnStart : PropertyID::GridRowStart;
    auto end_property = (property_id == PropertyID::GridColumn) ? PropertyID::GridColumnEnd : PropertyID::GridRowEnd;

    auto transaction = tokens.begin_transaction();
    auto current_token = tokens.next_token();

    Vector<ComponentValue> track_start_placement_tokens;
    while (true) {
        if (current_token.is_delim('/'))
            break;
        track_start_placement_tokens.append(current_token);
        if (!tokens.has_next_token())
            break;
        current_token = tokens.next_token();
    }

    Vector<ComponentValue> track_end_placement_tokens;
    if (tokens.has_next_token()) {
        current_token = tokens.next_token();
        while (true) {
            track_end_placement_tokens.append(current_token);
            if (!tokens.has_next_token())
                break;
            current_token = tokens.next_token();
        }
    }

    TokenStream track_start_placement_token_stream { track_start_placement_tokens };
    auto parsed_start_value = parse_grid_track_placement(track_start_placement_token_stream);
    if (parsed_start_value && track_end_placement_tokens.is_empty()) {
        transaction.commit();
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
RefPtr<StyleValue> Parser::parse_grid_track_size_list_shorthand_value(PropertyID property_id, TokenStream<ComponentValue>& tokens)
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
        auto& token = tokens.next_token();
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

RefPtr<StyleValue> Parser::parse_grid_area_shorthand_value(TokenStream<ComponentValue>& tokens)
{
    auto transaction = tokens.begin_transaction();

    auto parse_placement_tokens = [&](Vector<ComponentValue>& placement_tokens, bool check_for_delimiter = true) -> void {
        while (tokens.has_next_token()) {
            auto& current_token = tokens.next_token();
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

RefPtr<StyleValue> Parser::parse_grid_shorthand_value(TokenStream<ComponentValue>& tokens)
{
    // <'grid-template'> |
    // FIXME: <'grid-template-rows'> / [ auto-flow && dense? ] <'grid-auto-columns'>? |
    // FIXME: [ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>
    return parse_grid_track_size_list_shorthand_value(PropertyID::Grid, tokens);
}

// https://www.w3.org/TR/css-grid-1/#grid-template-areas-property
RefPtr<StyleValue> Parser::parse_grid_template_areas_value(TokenStream<ComponentValue>& tokens)
{
    // none | <string>+
    Vector<Vector<String>> grid_area_rows;

    if (contains_single_none_ident(tokens)) {
        (void)tokens.next_token(); // none
        return GridTemplateAreaStyleValue::create(move(grid_area_rows));
    }

    auto transaction = tokens.begin_transaction();
    while (tokens.has_next_token() && tokens.peek_token().is(Token::Type::String)) {
        Vector<String> grid_area_columns;
        auto const parts = MUST(tokens.next_token().token().string().to_string().split(' '));
        for (auto& part : parts) {
            grid_area_columns.append(part);
        }
        grid_area_rows.append(move(grid_area_columns));
    }
    transaction.commit();
    return GridTemplateAreaStyleValue::create(grid_area_rows);
}

static bool block_contains_var_or_attr(Block const& block);

static bool function_contains_var_or_attr(Function const& function)
{
    if (function.name().equals_ignoring_ascii_case("var"sv) || function.name().equals_ignoring_ascii_case("attr"sv))
        return true;
    for (auto const& token : function.values()) {
        if (token.is_function() && function_contains_var_or_attr(token.function()))
            return true;
        if (token.is_block() && block_contains_var_or_attr(token.block()))
            return true;
    }
    return false;
}

bool block_contains_var_or_attr(Block const& block)
{
    for (auto const& token : block.values()) {
        if (token.is_function() && function_contains_var_or_attr(token.function()))
            return true;
        if (token.is_block() && block_contains_var_or_attr(token.block()))
            return true;
    }
    return false;
}

Parser::ParseErrorOr<NonnullRefPtr<StyleValue>> Parser::parse_css_value(PropertyID property_id, TokenStream<ComponentValue>& unprocessed_tokens)
{
    m_context.set_current_property_id(property_id);
    Vector<ComponentValue> component_values;
    bool contains_var_or_attr = false;
    bool const property_accepts_custom_ident = property_accepts_type(property_id, ValueType::CustomIdent);

    while (unprocessed_tokens.has_next_token()) {
        auto const& token = unprocessed_tokens.next_token();

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
        return UnresolvedStyleValue::create(move(component_values), contains_var_or_attr);

    if (component_values.is_empty())
        return ParseError::SyntaxError;

    if (component_values.size() == 1) {
        if (auto parsed_value = parse_builtin_value(component_values.first()))
            return parsed_value.release_nonnull();
    }

    // Special-case property handling
    auto tokens = TokenStream { component_values };
    switch (property_id) {
    case PropertyID::AspectRatio:
        if (auto parsed_value = parse_aspect_ratio_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::BackdropFilter:
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
    case PropertyID::Content:
        if (auto parsed_value = parse_content_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Display:
        if (auto parsed_value = parse_display_value(tokens); parsed_value && !tokens.has_next_token())
            return parsed_value.release_nonnull();
        return ParseError::SyntaxError;
    case PropertyID::Flex:
        if (auto parsed_value = parse_flex_value(tokens); parsed_value && !tokens.has_next_token())
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

    // If there's only 1 ComponentValue, we can only produce a single StyleValue.
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

        // Some types (such as <ratio>) can be made from multiple ComponentValues, so if we only made 1 StyleValue, return it directly.
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

    HashMap<UnderlyingType<PropertyID>, Vector<ValueComparingNonnullRefPtr<StyleValue const>>> assigned_values;

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
        dbgln("No property (from {} properties) matched {}", unassigned_properties.size(), stream.peek_token().to_debug_string());
        for (auto id : unassigned_properties)
            dbgln("    {}", string_from_property_id(id));
        break;
    }

    for (auto& property : unassigned_properties)
        assigned_values.ensure(to_underlying(property)).append(property_initial_value(m_context.realm(), property));

    stream.skip_whitespace();
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

RefPtr<StyleValue> Parser::parse_css_value_for_property(PropertyID property_id, TokenStream<ComponentValue>& tokens)
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
    auto any_property_accepts_identifier = [](ReadonlySpan<PropertyID> property_ids, ValueID identifier) -> Optional<PropertyID> {
        for (auto const& property : property_ids) {
            if (property_accepts_identifier(property, identifier))
                return property;
        }
        return {};
    };

    auto& peek_token = tokens.peek_token();

    if (auto property = any_property_accepts_type(property_ids, ValueType::EasingFunction); property.has_value()) {
        if (auto maybe_easing_function = parse_easing_value(tokens))
            return PropertyAndValue { *property, maybe_easing_function };
    }

    if (peek_token.is(Token::Type::Ident)) {
        // NOTE: We do not try to parse "CSS-wide keywords" here. https://www.w3.org/TR/css-values-4/#common-keywords
        //       These are only valid on their own, and so should be parsed directly in `parse_css_value()`.
        auto ident = value_id_from_string(peek_token.token().ident());
        if (ident.has_value()) {
            if (auto property = any_property_accepts_identifier(property_ids, ident.value()); property.has_value()) {
                (void)tokens.next_token();
                return PropertyAndValue { *property, IdentifierStyleValue::create(ident.value()) };
            }
        }

        // Custom idents
        if (auto property = any_property_accepts_type(property_ids, ValueType::CustomIdent); property.has_value()) {
            (void)tokens.next_token();
            return PropertyAndValue { *property, CustomIdentStyleValue::create(peek_token.token().ident()) };
        }
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Color); property.has_value()) {
        if (auto maybe_color = parse_color_value(tokens))
            return PropertyAndValue { *property, maybe_color };
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
        if (property_accepting_integer.has_value()) {
            auto transaction = tokens.begin_transaction();
            if (auto integer = parse_integer_value(tokens); integer && property_accepts_integer(*property_accepting_integer, integer->as_integer().integer())) {
                transaction.commit();
                return PropertyAndValue { *property_accepting_integer, integer };
            }
        }
        if (property_accepting_number.has_value()) {
            auto transaction = tokens.begin_transaction();
            if (auto number = parse_number_value(tokens); number && property_accepts_number(*property_accepting_number, number->as_number().number())) {
                transaction.commit();
                return PropertyAndValue { *property_accepting_number, number };
            }
        }
    }

    if (peek_token.is(Token::Type::Percentage)) {
        auto percentage = Percentage(peek_token.token().percentage());
        if (auto property = any_property_accepts_type(property_ids, ValueType::Percentage); property.has_value() && property_accepts_percentage(*property, percentage)) {
            (void)tokens.next_token();
            return PropertyAndValue { *property, PercentageStyleValue::create(percentage) };
        }
    }

    if (auto property = any_property_accepts_type(property_ids, ValueType::Rect); property.has_value()) {
        if (auto maybe_rect = parse_rect_value(tokens))
            return PropertyAndValue { *property, maybe_rect };
    }

    if (peek_token.is(Token::Type::String)) {
        if (auto property = any_property_accepts_type(property_ids, ValueType::String); property.has_value())
            return PropertyAndValue { *property, StringStyleValue::create(tokens.next_token().token().string().to_string()) };
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
            auto token = tokens.next_token();
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
            (void)tokens.next_token();
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
            (void)tokens.next_token();
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
    virtual Optional<CalculatedStyleValue::ResolvedType> resolved_type() const override { VERIFY_NOT_REACHED(); }
    virtual Optional<CSSNumericType> determine_type(Web::CSS::PropertyID) const override { VERIFY_NOT_REACHED(); }
    virtual bool contains_percentage() const override { VERIFY_NOT_REACHED(); }
    virtual CalculatedStyleValue::CalculationResult resolve(Optional<Length::ResolutionContext const&>, CalculatedStyleValue::PercentageBasis const&) const override { VERIFY_NOT_REACHED(); }
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
LengthOrCalculated Parser::Parser::parse_as_sizes_attribute()
{
    // 1. Let unparsed sizes list be the result of parsing a comma-separated list of component values
    //    from the value of element's sizes attribute (or the empty string, if the attribute is absent).
    auto unparsed_sizes_list = parse_a_comma_separated_list_of_component_values(m_token_stream);

    // 2. Let size be null.
    Optional<LengthOrCalculated> size;

    // 3. For each unparsed size in unparsed sizes list:
    for (auto& unparsed_size : unparsed_sizes_list) {
        // 1. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        //    If unparsed size is now empty, that is a parse error; continue.
        while (!unparsed_size.is_empty() && unparsed_size.last().is_token() && unparsed_size.last().token().is(Token::Type::Whitespace))
            unparsed_size.take_last();
        if (unparsed_size.is_empty()) {
            log_parse_error();
            continue;
        }

        // 2. If the last component value in unparsed size is a valid non-negative <source-size-value>,
        //    let size be its value and remove the component value from unparsed size.
        //    FIXME: Any CSS function other than the math functions is invalid.
        //    Otherwise, there is a parse error; continue.
        auto last_value_stream = TokenStream<ComponentValue>::of_single_token(unparsed_size.last());
        if (auto source_size_value = parse_source_size_value(last_value_stream); source_size_value.has_value()) {
            size = source_size_value.value();
            unparsed_size.take_last();
        } else {
            log_parse_error();
            continue;
        }

        // 3. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        while (!unparsed_size.is_empty() && unparsed_size.last().is_token() && unparsed_size.last().token().is(Token::Type::Whitespace))
            unparsed_size.take_last();

        // If unparsed size is now empty, then return size.
        if (unparsed_size.is_empty())
            return size.value();

        // FIXME: If this was not the keyword auto and it was not the last item in unparsed sizes list, that is a parse error.

        // 4. Parse the remaining component values in unparsed size as a <media-condition>.
        //    If it does not parse correctly, or it does parse correctly but the <media-condition> evaluates to false, continue.
        TokenStream<ComponentValue> token_stream { unparsed_size };
        auto media_condition = parse_media_condition(token_stream, MediaCondition::AllowOr::Yes);
        auto context_window = m_context.window();
        if (context_window && media_condition && media_condition->evaluate(*context_window) == MatchResult::True) {
            return size.value();
        }

        // 5. If size is not auto, then return size.
        if (size.value().is_calculated() || !size.value().value().is_auto())
            return size.value();
    }

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
    for (auto& value : original_values) {
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
            // FIXME: Resolutions, once calc() supports them.
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
                        single_value = UnparsedCalculationNode::create(component_value);
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
            auto leaf_calculation = parse_a_calculation(component_value.block().values());
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

NonnullRefPtr<StyleValue> Parser::resolve_unresolved_style_value(ParsingContext const& context, DOM::Element& element, Optional<Selector::PseudoElement::Type> pseudo_element, PropertyID property_id, UnresolvedStyleValue const& unresolved)
{
    // Unresolved always contains a var() or attr(), unless it is a custom property's value, in which case we shouldn't be trying
    // to produce a different StyleValue from it.
    VERIFY(unresolved.contains_var_or_attr());

    // If the value is invalid, we fall back to `unset`: https://www.w3.org/TR/css-variables-1/#invalid-at-computed-value-time

    auto parser = MUST(Parser::create(context, ""sv));
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

NonnullRefPtr<StyleValue> Parser::resolve_unresolved_style_value(DOM::Element& element, Optional<Selector::PseudoElement::Type> pseudo_element, PropertyID property_id, UnresolvedStyleValue const& unresolved)
{
    TokenStream unresolved_values_without_variables_expanded { unresolved.values() };
    Vector<ComponentValue> values_with_variables_expanded;

    HashMap<FlyString, NonnullRefPtr<PropertyDependencyNode>> dependencies;
    if (!expand_variables(element, pseudo_element, string_from_property_id(property_id), dependencies, unresolved_values_without_variables_expanded, values_with_variables_expanded))
        return UnsetStyleValue::the();

    TokenStream unresolved_values_with_variables_expanded { values_with_variables_expanded };
    Vector<ComponentValue> expanded_values;
    if (!expand_unresolved_values(element, string_from_property_id(property_id), unresolved_values_with_variables_expanded, expanded_values))
        return UnsetStyleValue::the();

    auto expanded_value_tokens = TokenStream { expanded_values };
    if (auto parsed_value = parse_css_value(property_id, expanded_value_tokens); !parsed_value.is_error())
        return parsed_value.release_value();

    return UnsetStyleValue::the();
}

static RefPtr<StyleValue const> get_custom_property(DOM::Element const& element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, FlyString const& custom_property_name)
{
    if (pseudo_element.has_value()) {
        if (auto it = element.custom_properties(pseudo_element).find(custom_property_name); it != element.custom_properties(pseudo_element).end())
            return it->value.value;
    }

    for (auto const* current_element = &element; current_element; current_element = current_element->parent_element()) {
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
        auto const& value = source.next_token();
        if (value.is_block()) {
            auto const& source_block = value.block();
            Vector<ComponentValue> block_values;
            TokenStream source_block_contents { source_block.values() };
            if (!expand_variables(element, pseudo_element, property_name, dependencies, source_block_contents, block_values))
                return false;
            NonnullRefPtr<Block> block = Block::create(source_block.token(), move(block_values));
            dest.empend(block);
            continue;
        }
        if (!value.is_function()) {
            dest.empend(value);
            continue;
        }
        if (!value.function().name().equals_ignoring_ascii_case("var"sv)) {
            auto const& source_function = value.function();
            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.values() };
            if (!expand_variables(element, pseudo_element, property_name, dependencies, source_function_contents, function_values))
                return false;
            NonnullRefPtr<Function> function = Function::create(source_function.name(), move(function_values));
            dest.empend(function);
            continue;
        }

        TokenStream var_contents { value.function().values() };
        var_contents.skip_whitespace();
        if (!var_contents.has_next_token())
            return false;

        auto const& custom_property_name_token = var_contents.next_token();
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
        var_contents.skip_whitespace();
        if (var_contents.has_next_token()) {
            auto const& comma_token = var_contents.next_token();
            if (!comma_token.is(Token::Type::Comma))
                return false;
            var_contents.skip_whitespace();
            if (!expand_variables(element, pseudo_element, property_name, dependencies, var_contents, dest))
                return false;
        }
    }
    return true;
}

bool Parser::expand_unresolved_values(DOM::Element& element, FlyString const& property_name, TokenStream<ComponentValue>& source, Vector<ComponentValue>& dest)
{
    while (source.has_next_token()) {
        auto const& value = source.next_token();
        if (value.is_function()) {
            if (value.function().name().equals_ignoring_ascii_case("attr"sv)) {
                if (!substitute_attr_function(element, property_name, value.function(), dest))
                    return false;
                continue;
            }

            if (auto maybe_calc_value = parse_calculated_value(value); maybe_calc_value && maybe_calc_value->is_calculated()) {
                auto& calc_value = maybe_calc_value->as_calculated();
                if (calc_value.resolves_to_angle()) {
                    auto resolved_value = calc_value.resolve_angle();
                    dest.empend(Token::create_dimension(resolved_value->to_degrees(), "deg"_fly_string));
                    continue;
                }
                if (calc_value.resolves_to_frequency()) {
                    auto resolved_value = calc_value.resolve_frequency();
                    dest.empend(Token::create_dimension(resolved_value->to_hertz(), "hz"_fly_string));
                    continue;
                }
                if (calc_value.resolves_to_length()) {
                    // FIXME: In order to resolve lengths, we need to know the font metrics in case a font-relative unit
                    //  is used. So... we can't do that until style is computed?
                    //  This might be easier once we have calc-simplification implemented.
                }
                if (calc_value.resolves_to_percentage()) {
                    auto resolved_value = calc_value.resolve_percentage();
                    dest.empend(Token::create_percentage(resolved_value.value().value()));
                    continue;
                }
                if (calc_value.resolves_to_time()) {
                    auto resolved_value = calc_value.resolve_time();
                    dest.empend(Token::create_dimension(resolved_value->to_seconds(), "s"_fly_string));
                    continue;
                }
                if (calc_value.resolves_to_number()) {
                    auto resolved_value = calc_value.resolve_number();
                    dest.empend(Token::create_number(resolved_value.value()));
                    continue;
                }
            }

            auto const& source_function = value.function();
            Vector<ComponentValue> function_values;
            TokenStream source_function_contents { source_function.values() };
            if (!expand_unresolved_values(element, property_name, source_function_contents, function_values))
                return false;
            NonnullRefPtr<Function> function = Function::create(source_function.name(), move(function_values));
            dest.empend(function);
            continue;
        }
        if (value.is_block()) {
            auto const& source_block = value.block();
            TokenStream source_block_values { source_block.values() };
            Vector<ComponentValue> block_values;
            if (!expand_unresolved_values(element, property_name, source_block_values, block_values))
                return false;
            NonnullRefPtr<Block> block = Block::create(source_block.token(), move(block_values));
            dest.empend(move(block));
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
    TokenStream attr_contents { attr_function.values() };
    attr_contents.skip_whitespace();
    if (!attr_contents.has_next_token())
        return false;

    // - Attribute name
    // FIXME: Support optional attribute namespace
    if (!attr_contents.peek_token().is(Token::Type::Ident))
        return false;
    auto attribute_name = attr_contents.next_token().token().ident();
    attr_contents.skip_whitespace();

    // - Attribute type (optional)
    auto attribute_type = "string"_fly_string;
    if (attr_contents.peek_token().is(Token::Type::Ident)) {
        attribute_type = attr_contents.next_token().token().ident();
        attr_contents.skip_whitespace();
    }

    // - Comma, then fallback values (optional)
    bool has_fallback_values = false;
    if (attr_contents.has_next_token()) {
        if (!attr_contents.peek_token().is(Token::Type::Comma))
            return false;
        (void)attr_contents.next_token(); // Comma
        has_fallback_values = true;
    }

    // Then, run the substitution algorithm:

    // 1. If the attr() function has a substitution value, replace the attr() function by the substitution value.
    // https://drafts.csswg.org/css-values-5/#attr-types
    if (element.has_attribute(attribute_name)) {
        auto attribute_value = element.get_attribute_value(attribute_name);
        if (attribute_type.equals_ignoring_ascii_case("angle"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
            if (component_value.has_value() && component_value->is(Token::Type::Number)) {
                dest.append(component_value.release_value());
                return true;
            }
        } else if (attribute_type.equals_ignoring_ascii_case("percentage"_fly_string)) {
            // Parse a component value from the attribute’s value.
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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
            auto component_value = MUST(Parser::Parser::create(m_context, attribute_value)).parse_as_component_value();
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

}
