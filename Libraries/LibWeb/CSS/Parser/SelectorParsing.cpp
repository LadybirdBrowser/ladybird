/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/SelectorRustFFI.h>

namespace Web::CSS::Parser {

template<typename T>
Parser::ParseErrorOr<SelectorList> Parser::parse_a_selector_list(TokenStream<T>& tokens, SelectorType type, SelectorParsingMode mode)
{
    Utf16StringBuilder builder;
    while (tokens.has_next_token())
        builder.append(tokens.consume_a_token().original_source_text());
    auto input = builder.to_string();
    auto selectors = parse_selector_list_in_rust(input, m_declared_namespaces,
        type == SelectorType::Relative, mode == SelectorParsingMode::Forgiving);
    if (!selectors.has_value())
        return ParseError::SyntaxError;
    return selectors.release_value();
}
template Parser::ParseErrorOr<SelectorList> Parser::parse_a_selector_list(TokenStream<ComponentValue>&, SelectorType, SelectorParsingMode);
template Parser::ParseErrorOr<SelectorList> Parser::parse_a_selector_list(TokenStream<Token>&, SelectorType, SelectorParsingMode);

Optional<SelectorList> Parser::parse_as_selector(SelectorParsingMode mode)
{
    auto selectors = parse_a_selector_list(token_stream(), SelectorType::Standalone, mode);
    if (selectors.is_error())
        return {};
    return selectors.release_value();
}

Optional<SelectorList> Parser::parse_as_relative_selector(SelectorParsingMode mode)
{
    auto selectors = parse_a_selector_list(token_stream(), SelectorType::Relative, mode);
    if (selectors.is_error())
        return {};
    return selectors.release_value();
}

Optional<Selector::PseudoElementSelector> Parser::parse_as_pseudo_element_selector()
{
    Utf16StringBuilder builder;
    while (token_stream().has_next_token())
        builder.append(token_stream().consume_a_token().original_source_text());
    auto input = builder.to_string();
    auto input_view = input.utf16_view();
    auto parsed = SelectorFFI::rust_selector_parse_pseudo_element({
        .ascii = input_view.has_ascii_storage() ? reinterpret_cast<u8 const*>(input_view.ascii_span().data()) : nullptr,
        .utf16 = input_view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input_view.utf16_span().data()),
        .length = input_view.length_in_code_units(),
    });
    if (!parsed.selector)
        return {};
    auto selector = Selector::create(parsed.selector);
    auto pseudo_element = pseudo_element_from_ffi(parsed.pseudo_element);
    if (!pseudo_element.has_value())
        return {};
    return Selector::PseudoElementSelector { pseudo_element.release_value(), selector->serialize() };
}

Optional<PageSelectorList> Parser::parse_as_page_selector_list()
{
    auto selector_list = parse_a_page_selector_list(token_stream());
    if (!selector_list.is_error())
        return selector_list.release_value();
    return {};
}

template<typename T>
Parser::ParseErrorOr<PageSelectorList> Parser::parse_a_page_selector_list(TokenStream<T>& tokens)
{
    // https://drafts.csswg.org/css-page-3/#syntax-page-selector
    // <page-selector-list> = <page-selector>#
    // <page-selector> = [ <ident-token>? <pseudo-page>* ]!
    // <pseudo-page> = : [ left | right | first | blank ]
    PageSelectorList selector_list;
    tokens.discard_whitespace();
    while (tokens.has_next_token()) {
        Optional<Utf16FlyString> maybe_ident;
        if (tokens.next_token().is(Token::Type::Ident))
            maybe_ident = static_cast<Token>(tokens.consume_a_token()).ident();

        Vector<PagePseudoClass> pseudo_classes;
        while (tokens.next_token().is(Token::Type::Colon)) {
            tokens.discard_a_token();
            if (!tokens.next_token().is(Token::Type::Ident))
                return ParseError::SyntaxError;
            auto name = static_cast<Token>(tokens.consume_a_token()).ident();
            auto pseudo_class = page_pseudo_class_from_string(name);
            if (!pseudo_class.has_value())
                return ParseError::SyntaxError;
            pseudo_classes.append(*pseudo_class);
        }
        if (!maybe_ident.has_value() && pseudo_classes.is_empty())
            return ParseError::SyntaxError;
        selector_list.empend(move(maybe_ident), move(pseudo_classes));
        tokens.discard_whitespace();
        if (tokens.next_token().is(Token::Type::Comma)) {
            tokens.discard_a_token();
            tokens.discard_whitespace();
            if (!tokens.has_next_token())
                return ParseError::SyntaxError;
        } else if (tokens.has_next_token()) {
            return ParseError::SyntaxError;
        }
    }
    return selector_list;
}
template Parser::ParseErrorOr<PageSelectorList> Parser::parse_a_page_selector_list(TokenStream<ComponentValue>&);
template Parser::ParseErrorOr<PageSelectorList> Parser::parse_a_page_selector_list(TokenStream<Token>&);

}
