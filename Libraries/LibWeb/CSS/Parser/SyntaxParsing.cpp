/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/Parser/TokenStream.h>

namespace Web::CSS::Parser {

static OwnPtr<SyntaxNode> parse_syntax_single_component(TokenStream<ComponentValue>& tokens)
{
    // <syntax-single-component> = '<' <syntax-type-name> '>' | <ident>
    // <syntax-type-name> = angle | color | custom-ident | image | integer
    //                    | length | length-percentage | number
    //                    | percentage | resolution | string | time
    //                    | url | transform-function

    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    // <ident>
    if (tokens.next_token().is(Token::Type::Ident)) {
        auto ident = tokens.consume_a_token().token().ident();
        transaction.commit();
        return IdentSyntaxNode::create(move(ident));
    }

    // '<' <syntax-type-name> '>'
    if (tokens.next_token().is_delim('<')) {
        tokens.discard_a_token(); // '<'
        auto const& type_name = tokens.consume_a_token();
        auto const& end_token = tokens.consume_a_token();

        if (end_token.is_delim('>')
            && type_name.is(Token::Type::Ident)
            && first_is_one_of(type_name.token().ident(), "angle"sv,
                "color"sv,
                "custom-ident"sv,
                "image"sv,
                "integer"sv,
                "length"sv,
                "length-percentage"sv,
                "number"sv,
                "percentage"sv,
                "resolution"sv,
                "string"sv,
                "time"sv,
                "url"sv,
                "transform-function"sv)) {
            transaction.commit();
            return TypeSyntaxNode::create(type_name.token().ident());
        }
    }

    return nullptr;
}

static Optional<char> parse_syntax_multiplier(TokenStream<ComponentValue>& tokens)
{
    // <syntax-multiplier> = [ '#' | '+' ]
    auto transaction = tokens.begin_transaction();

    auto delim = tokens.consume_a_token();
    if (delim.is_delim('#') || delim.is_delim('+')) {
        transaction.commit();
        return delim.token().delim();
    }

    return {};
}

static OwnPtr<SyntaxNode> parse_syntax_component(TokenStream<ComponentValue>& tokens)
{
    // <syntax-component> = <syntax-single-component> <syntax-multiplier>?
    //                    | '<' transform-list '>'

    auto transaction = tokens.begin_transaction();

    tokens.discard_whitespace();

    // '<' transform-list '>'
    if (tokens.next_token().is_delim('<')) {
        auto transform_list_transaction = transaction.create_child();
        tokens.discard_a_token(); // '<'
        auto& ident_token = tokens.consume_a_token();
        auto& end_token = tokens.consume_a_token();

        if (ident_token.is_ident("transform-list"sv) && end_token.is_delim('>')) {
            transform_list_transaction.commit();
            return TypeSyntaxNode::create("transform-list"_fly_string);
        }
    }

    // <syntax-single-component> <syntax-multiplier>?
    auto syntax_single_component = parse_syntax_single_component(tokens);
    if (!syntax_single_component)
        return nullptr;

    auto multiplier = parse_syntax_multiplier(tokens);
    if (!multiplier.has_value()) {
        transaction.commit();
        return syntax_single_component.release_nonnull();
    }

    switch (multiplier.value()) {
    case '#':
        transaction.commit();
        return CommaSeparatedMultiplierSyntaxNode::create(syntax_single_component.release_nonnull());
    case '+':
        transaction.commit();
        return MultiplierSyntaxNode::create(syntax_single_component.release_nonnull());
    default:
        return nullptr;
    }
}

static Optional<char> parse_syntax_combinator(TokenStream<ComponentValue>& tokens)
{
    // <syntax-combinator> = '|'
    auto transaction = tokens.begin_transaction();
    tokens.discard_whitespace();

    auto delim = tokens.consume_a_token();
    if (delim.is_delim('|')) {
        transaction.commit();
        return delim.token().delim();
    }

    return {};
}

// https://drafts.csswg.org/css-values-5/#typedef-syntax
OwnPtr<SyntaxNode> parse_as_syntax(Vector<ComponentValue> const& component_values)
{
    // <syntax> = '*' | <syntax-component> [ <syntax-combinator> <syntax-component> ]* | <syntax-string>
    // <syntax-component> = <syntax-single-component> <syntax-multiplier>?
    //                    | '<' transform-list '>'
    // <syntax-single-component> = '<' <syntax-type-name> '>' | <ident>
    // <syntax-type-name> = angle | color | custom-ident | image | integer
    //                    | length | length-percentage | number
    //                    | percentage | resolution | string | time
    //                    | url | transform-function
    // <syntax-combinator> = '|'
    // <syntax-multiplier> = [ '#' | '+' ]
    //
    // <syntax-string> = <string>
    // FIXME: Eventually, extend this to also parse *any* CSS grammar, not just for the <syntax> type.

    TokenStream tokens { component_values };
    tokens.discard_whitespace();

    // '*'
    if (tokens.next_token().is_delim('*')) {
        tokens.discard_a_token(); // '*'
        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return nullptr;
        return UniversalSyntaxNode::create();
    }

    // <syntax-string> = <string>
    // A <syntax-string> is a <string> whose value successfully parses as a <syntax>, and represents the same value as
    // that <syntax> would.
    // NB: For now, this is the only time a string is allowed in a <syntax>.
    if (tokens.next_token().is(Token::Type::String)) {
        auto string = tokens.consume_a_token().token().string();
        tokens.discard_whitespace();
        if (tokens.has_next_token())
            return nullptr;

        auto child_component_values = Parser::create(ParsingParams {}, string).parse_as_list_of_component_values();
        return parse_as_syntax(child_component_values);
    }

    // <syntax-component> [ <syntax-combinator> <syntax-component> ]*
    auto first = parse_syntax_component(tokens);
    if (!first)
        return nullptr;
    Vector<NonnullOwnPtr<SyntaxNode>> syntax_components;
    syntax_components.append(first.release_nonnull());

    tokens.discard_whitespace();
    while (tokens.has_next_token()) {
        auto combinator = parse_syntax_combinator(tokens);
        tokens.discard_whitespace();
        auto component = parse_syntax_component(tokens);
        tokens.discard_whitespace();
        if (!combinator.has_value() || !component) {
            dbgln("Failed parsing syntax portion, combinator = `{}`, component = `{}`", combinator, component);
            return nullptr;
        }

        // FIXME: Make this logic smarter once we have more than one type of combinator.
        // For now, assume we're always making an AlternativesSyntaxNode.
        VERIFY(combinator == '|');

        syntax_components.append(component.release_nonnull());
    }

    if (syntax_components.size() == 1)
        return syntax_components.take_first();
    return AlternativesSyntaxNode::create(move(syntax_components));
}

}
