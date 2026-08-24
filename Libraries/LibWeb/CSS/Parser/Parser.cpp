/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
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

#include <AK/Debug.h>
#include <LibURL/Parser.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSFunctionDeclarations.h>
#include <LibWeb/CSS/CSSMarginRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/ErrorReporter.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/RustQueryParsing.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/Serialize.h>
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

enum class DeclarationValueNested : u8 {
    No,
    Yes,
};

static void consume_declaration_value(TokenStream<ComponentValue>& tokens, Optional<Token::Type> end_token_type, DeclarationValueNested nested, Parser::DisallowTopLevelCurlyBlocks disallow_top_level_curly_blocks)
{
    while (tokens.has_next_token()) {
        auto const& peek = tokens.next_token();
        if (peek.is_block() || peek.is_function()) {
            auto const& values = peek.is_block() ? peek.block().value : peek.function().value;
            TokenStream nested_tokens { values };
            consume_declaration_value(nested_tokens, end_token_type, DeclarationValueNested::Yes, disallow_top_level_curly_blocks);
            if (peek.is_block() && peek.block().is_curly() && nested == DeclarationValueNested::No && disallow_top_level_curly_blocks == Parser::DisallowTopLevelCurlyBlocks::Yes)
                break;
            if (nested_tokens.has_next_token())
                break;
            tokens.discard_a_token();
            continue;
        }
        if (!peek.is_token()) {
            tokens.discard_a_token();
            continue;
        }
        auto type = peek.token().type();
        bool valid = !first_is_one_of(type, Token::Type::Invalid, Token::Type::EndOfFile, Token::Type::BadString, Token::Type::BadUrl,
            Token::Type::Function, Token::Type::OpenCurly, Token::Type::OpenParen, Token::Type::OpenSquare,
            Token::Type::CloseCurly, Token::Type::CloseParen, Token::Type::CloseSquare);
        if (type == Token::Type::Semicolon)
            valid = nested == DeclarationValueNested::Yes;
        else if (type == Token::Type::Delim)
            valid = nested == DeclarationValueNested::Yes || peek.token().delim() != '!';
        else if (nested == DeclarationValueNested::No && end_token_type.has_value() && peek.is(*end_token_type))
            valid = false;
        if (!valid)
            break;
        tokens.discard_a_token();
    }
}

Optional<ReadonlySpan<ComponentValue>> Parser::parse_declaration_value_as_span(TokenStream<ComponentValue>& tokens, Optional<Token::Type> end_token_type, DisallowTopLevelCurlyBlocks disallow_top_level_curly_blocks)
{
    auto start = tokens.current_index();
    consume_declaration_value(tokens, end_token_type, DeclarationValueNested::No, disallow_top_level_curly_blocks);
    auto value = tokens.tokens_since(start);
    return value.is_empty() ? OptionalNone {} : Optional<ReadonlySpan<ComponentValue>> { value };
}

Optional<Vector<ComponentValue>> Parser::parse_declaration_value(TokenStream<ComponentValue>& tokens, Optional<Token::Type> end_token_type)
{
    auto value = parse_declaration_value_as_span(tokens, end_token_type);
    return value.has_value() ? Optional<Vector<ComponentValue>> { Vector<ComponentValue> { *value } } : OptionalNone {};
}

struct SyntaxDeclarationForVerification {
    Utf16FlyString name;
    Utf16String value;
    Important important;

    bool operator==(SyntaxDeclarationForVerification const&) const = default;
};

static void collect_syntax_declarations_for_verification(Rule const&, Vector<SyntaxDeclarationForVerification>&);

static void collect_syntax_declarations_for_verification(ReadonlySpan<RuleOrListOfDeclarations> items, Vector<SyntaxDeclarationForVerification>& declarations)
{
    for (auto const& item : items) {
        item.visit(
            [&](Rule const& rule) { collect_syntax_declarations_for_verification(rule, declarations); },
            [&](Vector<Declaration> const& declaration_list) {
                for (auto const& declaration : declaration_list) {
                    auto value = declaration.value_text;
                    if (!declaration.value.is_empty())
                        value = serialize_a_series_of_component_values(declaration.value);
                    declarations.append({ declaration.name, move(value), declaration.important });
                }
            });
    }
}

static void collect_syntax_declarations_for_verification(Rule const& rule, Vector<SyntaxDeclarationForVerification>& declarations)
{
    rule.visit(
        [&](AtRule const& at_rule) {
            collect_syntax_declarations_for_verification(at_rule.child_rules_and_lists_of_declarations, declarations);
        },
        [&](QualifiedRule const& qualified_rule) {
            collect_syntax_declarations_for_verification(Vector<RuleOrListOfDeclarations> { qualified_rule.declarations }, declarations);
            collect_syntax_declarations_for_verification(qualified_rule.child_rules, declarations);
        });
}

static void verify_rust_syntax_declarations(ReadonlySpan<RuleOrListOfDeclarations> rust_items, ReadonlySpan<RuleOrListOfDeclarations> cpp_items)
{
    Vector<SyntaxDeclarationForVerification> rust_declarations;
    Vector<SyntaxDeclarationForVerification> cpp_declarations;
    collect_syntax_declarations_for_verification(rust_items, rust_declarations);
    collect_syntax_declarations_for_verification(cpp_items, cpp_declarations);
    if (rust_declarations == cpp_declarations)
        return;
    warnln("Rust CSS syntax parser mismatch: Rust produced {} declarations, C++ produced {}", rust_declarations.size(), cpp_declarations.size());
    auto count = min(rust_declarations.size(), cpp_declarations.size());
    for (size_t index = 0; index < count; ++index) {
        if (rust_declarations[index] == cpp_declarations[index])
            continue;
        warnln("  first mismatch at {}: Rust '{}: {}' important={}, C++ '{}: {}' important={}",
            index,
            rust_declarations[index].name,
            rust_declarations[index].value,
            rust_declarations[index].important == Important::Yes,
            cpp_declarations[index].name,
            cpp_declarations[index].value,
            cpp_declarations[index].important == Important::Yes);
        break;
    }
}

static void verify_rust_syntax_declarations(ReadonlySpan<Rule> rust_rules, ReadonlySpan<Rule> cpp_rules)
{
    Vector<RuleOrListOfDeclarations> rust_items;
    Vector<RuleOrListOfDeclarations> cpp_items;
    for (auto const& rule : rust_rules)
        rust_items.append(rule);
    for (auto const& rule : cpp_rules)
        cpp_items.append(rule);
    verify_rust_syntax_declarations(rust_items, cpp_items);
}

static bool should_verify_rust_syntax_parser()
{
    static bool const should_verify = [] {
        auto* value = getenv("LIBWEB_VERIFY_RUST_SYNTAX_PARSER");
        return value && StringView { value, strlen(value) } == "1"sv;
    }();
    return should_verify;
}

ParsingParams::ParsingParams(ParsingMode mode)
    : mode(mode)
{
}

ParsingParams::ParsingParams(ValueParsingContext value_context)
    : value_context(Vector { move(value_context) })
{
}

ParsingParams::ParsingParams(IsUAStyleSheet is_ua_style_sheet)
    : is_ua_style_sheet(is_ua_style_sheet)
{
}

ParsingParams::ParsingParams(DOM::Document const& document, ParsingMode mode)
    : document(&document)
    , mode(mode)
{
}

Parser Parser::create(ParsingParams const& context, StringView input, StringView encoding)
{
    auto source = RustTokenizer::normalize_input(input, encoding);
    return Parser { context, move(source) };
}

Parser Parser::create(ParsingParams const& context, Utf16View input)
{
    auto source = RustTokenizer::normalize_input(input);
    return Parser { context, move(source) };
}

Parser::Parser(ParsingParams const& context, Utf16String source)
    : m_document(context.document)
    , m_parsing_mode(context.mode)
    , m_is_ua_style_sheet(context.is_ua_style_sheet)
    , m_source(move(source))
    , m_value_context(move(context.value_context))
    , m_rule_context(move(context.rule_context))
    , m_declared_namespaces(move(context.declared_namespaces))
{
}

TokenStream<Token>& Parser::token_stream()
{
    if (!m_token_stream) {
        m_tokens = RustTokenizer::tokenize(m_source);
        m_token_stream = make<TokenStream<Token>>(m_tokens);
    }
    return *m_token_stream;
}

// https://drafts.csswg.org/css-syntax/#parse-stylesheet
template<typename T>
Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<T>& input, Optional<::URL::URL> location)
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

GC::RootVector<GC::Ref<CSSRule>> Parser::convert_rules(Vector<Rule> const& raw_rules)
{
    bool import_rules_valid = true;
    bool namespace_rules_valid = true;

    // Interpret all of the resulting top-level qualified rules as style rules, defined below.
    GC::RootVector<GC::Ref<CSSRule>> rules;
    for (auto const& raw_rule : raw_rules) {
        auto rule = convert_to_rule<CSSNestedDeclarations>(raw_rule, Nested::No);
        // If any style rule is invalid, or any at-rule is not recognized or is invalid according to its grammar or context, it’s a parse error.
        // Discard that rule.
        if (!rule) {
            log_parse_error();
            continue;
        }

        // "Any @import rules must precede all other valid at-rules and style rules in a style sheet
        // (ignoring @charset and @layer statement rules) and must not have any other valid at-rules
        // or style rules between it and previous @import rules, or else the @import rule is invalid."
        // https://drafts.csswg.org/css-cascade-5/#at-import
        //
        // "Any @namespace rules must follow all @charset and @import rules and precede all other
        // non-ignored at-rules and style rules in a style sheet.
        // ...
        // A syntactically invalid @namespace rule (whether malformed or misplaced) must be ignored."
        // https://drafts.csswg.org/css-namespaces/#syntax
        switch (rule->type()) {
        case CSSRule::Type::LayerStatement:
            break;
        case CSSRule::Type::Import:
            if (!import_rules_valid)
                continue;
            break;
        case CSSRule::Type::Namespace:
            import_rules_valid = false;

            if (!namespace_rules_valid)
                continue;

            m_declared_namespaces.set(as<CSSNamespaceRule>(*rule).prefix());
            break;
        default:
            import_rules_valid = false;
            namespace_rules_valid = false;
            break;
        }

        rules.append(*rule);
    }

    return rules;
}

GC::RootVector<GC::Ref<CSSRule>> Parser::parse_as_stylesheet_contents()
{
    auto rules = RustSyntaxParser::parse_stylesheet(*this);
    if (should_verify_rust_syntax_parser())
        verify_rust_syntax_declarations(rules, parse_a_stylesheet(token_stream(), {}).rules);
    return convert_rules(rules);
}

// https://drafts.csswg.org/css-syntax/#parse-a-css-stylesheet
GC::Ref<CSS::CSSStyleSheet> Parser::parse_as_css_stylesheet(Optional<::URL::URL> location, GC::Ptr<MediaList> media_list)
{
    // To parse a CSS stylesheet, first parse a stylesheet.
    ParsedStyleSheet style_sheet {
        .location = location,
        .rules = RustSyntaxParser::parse_stylesheet(*this),
    };
    if (should_verify_rust_syntax_parser())
        verify_rust_syntax_declarations(style_sheet.rules, parse_a_stylesheet(token_stream(), location).rules);

    auto rule_list = CSSRuleList::create(convert_rules(style_sheet.rules));
    if (!media_list)
        media_list = MediaList::create({});
    return CSSStyleSheet::create(rule_list, *media_list, move(location));
}

RefPtr<Supports> Parser::parse_as_supports()
{
    return RustQueryParser::parse_supports(*this, m_source);
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
        .prelude_text = {},
        .parsed_prelude = {},
        .child_rules_and_lists_of_declarations = {},
        .is_block_rule = false,
    };
    auto preserve_prelude_text = [&] {
        rule.prelude_text = serialize_a_series_of_component_values_preserving_original_source_text(rule.prelude);
    };

    // Process input:
    for (;;) {
        auto& token = input.next_token();

        // <semicolon-token>
        // <EOF-token>
        if (token.is(Token::Type::Semicolon) || token.is(Token::Type::EndOfFile)) {
            // Discard a token from input. If rule is valid in the current context, return it; otherwise return nothing.
            input.discard_a_token();
            preserve_prelude_text();
            if (is_valid_in_the_current_context(rule))
                return rule;
            return {};
        }

        // <}-token>
        if (token.is(Token::Type::CloseCurly)) {
            // If nested is true:
            if (nested == Nested::Yes) {
                // If rule is valid in the current context, return it.
                preserve_prelude_text();
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
            m_rule_context.append(rule_context_type_for_at_rule(rule.name));
            rule.child_rules_and_lists_of_declarations = consume_a_block(input);
            rule.is_block_rule = true;
            m_rule_context.take_last();

            // If rule is valid in the current context, return it. Otherwise, return nothing.
            preserve_prelude_text();
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
        .prelude_text = {},
        .parsed_prelude = {},
        .declarations = {},
        .child_rules = {},
        .source_position = {},
    };

    // NOTE: Qualified rules inside @keyframes are a keyframe rule.
    //       We'll assume all others are style rules.
    auto type_of_qualified_rule = (!m_rule_context.is_empty() && m_rule_context.last() == RuleContext::AtKeyframes)
        ? RuleContext::Keyframe
        : RuleContext::Style;

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
            rule.prelude_text = serialize_a_series_of_component_values_for_retokenization(rule.prelude);
            // If the first two non-<whitespace-token> values of rule’s prelude are an <ident-token> whose value starts with "--"
            // followed by a <colon-token>, then:
            TokenStream prelude_tokens { rule.prelude };
            prelude_tokens.discard_whitespace();
            auto& first_non_whitespace = prelude_tokens.consume_a_token();
            prelude_tokens.discard_whitespace();
            auto& second_non_whitespace = prelude_tokens.consume_a_token();
            if (first_non_whitespace.is(Token::Type::Ident) && first_non_whitespace.token().ident().starts_with("--"sv)
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
            auto component_value = consume_a_component_value(input);
            if (!rule.source_position.has_value() && !component_value.is(Token::Type::Whitespace))
                rule.source_position = component_value.start_position();
            rule.prelude.append(move(component_value));
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
            // OPTIMIZATION: Look ahead to determine if this can be a declaration (ident whitespace* ':').
            //               If not, skip straight to qualified rule parsing, avoiding the expensive
            //               mark/restore cycle and consume_the_remnants_of_a_bad_declaration.
            bool could_be_declaration = false;
            if (token.is(Token::Type::Ident)) {
                size_t lookahead = 1;
                while (input.peek_token(lookahead).is(Token::Type::Whitespace))
                    ++lookahead;
                could_be_declaration = input.peek_token(lookahead).is(Token::Type::Colon);
            }

            auto flush_declarations = [&] {
                if (!declarations.is_empty()) {
                    rules.append(move(declarations));
                    declarations = {};
                }
            };

            auto consume_qualified_rule = [&] {
                consume_a_qualified_rule(input, Token::Type::Semicolon, Nested::Yes).visit([](Empty&) {}, [&](InvalidRuleError&) { flush_declarations(); }, [&](QualifiedRule rule) {
                        flush_declarations();
                        rules.append({ move(rule) }); });
            };

            if (could_be_declaration) {
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
                    consume_qualified_rule();
                }
            } else {
                // Not a declaration, go straight to qualified rule parsing.
                consume_qualified_rule();
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
Optional<Declaration> Parser::consume_a_declaration(TokenStream<T>& input, Nested nested, SaveOriginalText save_full_text)
{
    // To consume a declaration from a token stream input, given an optional bool nested (default false):

    // TODO: As noted in the "Implementation note" below https://drafts.csswg.org/css-syntax/#consume-block-contents
    //       there are ways we can optimise this by early-exiting.

    // Let decl be a new declaration, with an initially empty name and a value set to an empty list.
    Declaration declaration {
        .name {},
        .value {},
        .important = Important::No,
        .original_value_text = {},
        .original_full_text = {},
        .source_position = {},
        .value_text = {},
        .parsed_property_id = {},
        .parsed_descriptor_id = {},
        .parsed_value = {},
    };
    auto start_token_index = input.current_index();

    // 1. If the next token is an <ident-token>, consume a token from input and set decl’s name to the token’s value.
    if (input.next_token().is(Token::Type::Ident)) {
        auto token = (Token)input.consume_a_token();
        declaration.source_position = token.start_position();
        declaration.name = token.ident();
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
            if (value.is_ident("important"_utf16)) {
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
    if (is_invalid_custom_property_name_string(declaration.name))
        return {};
    if (is_a_custom_property_name_string(declaration.name)) {
        // TODO: If we could reach inside the source string that the TokenStream uses, we could grab this as
        //       a single substring instead of having to reconstruct it.
        Utf16StringBuilder original_text;
        for (auto const& value : declaration.value) {
            original_text.append(value.original_source_text());
        }
        declaration.original_value_text = original_text.to_string();
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
    if (is_valid_in_the_current_context(declaration)) {
        // AD-HOC: Assemble source tokens.
        if (save_full_text == SaveOriginalText::Yes) {
            Utf16StringBuilder original_full_text;
            for (auto& token : input.tokens_since(start_token_index))
                token.serialize_to(original_full_text);

            declaration.original_full_text = original_full_text.to_string();
        }
        return declaration;
    }
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

CSSRule* Parser::parse_as_css_rule(bool nested)
{
    auto nested_mode = nested ? Nested::Yes : Nested::No;
    auto rule = RustSyntaxParser::parse_rule(*this, m_rule_context, nested ? RuleNesting::Yes : RuleNesting::No);
    if (!rule.has_value())
        return {};
    return convert_to_rule<CSSNestedDeclarations>(*rule, nested_mode).ptr();
}

GC::Ptr<CSSKeyframeRule> Parser::parse_as_keyframe_rule()
{
    m_rule_context.append(RuleContext::AtKeyframes);
    ScopeGuard guard = [&] {
        [[maybe_unused]] auto last = m_rule_context.take_last();
        VERIFY(last == RuleContext::AtKeyframes);
    };

    auto items = RustSyntaxParser::parse_block_contents(*this, m_rule_context);
    if (items.size() != 1 || !items.first().has<Rule>())
        return {};
    auto const& rule = items.first().get<Rule>();
    if (!rule.has<QualifiedRule>())
        return {};
    return convert_to_keyframe_rule(rule.get<QualifiedRule>());
}

Vector<Percentage> Parser::parse_as_keyframe_selectors()
{
    auto component_values = parse_a_list_of_component_values(token_stream());
    auto tokens = TokenStream { component_values };
    return parse_keyframe_selectors(tokens);
}

// https://drafts.csswg.org/css-syntax/#parse-rule
template<typename T>
Optional<Rule> Parser::parse_a_rule(TokenStream<T>& input, Nested nested)
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
        rule = consume_an_at_rule(input, nested).map([](auto&& it) { return Rule { it }; });
    }
    //    Otherwise, consume a qualified rule from input and let rule be the return value.
    //    If nothing or an invalid rule error was returned, return a syntax error.
    else {
        consume_a_qualified_rule(input, {}, nested).visit([&](QualifiedRule qualified_rule) { rule = move(qualified_rule); }, [](auto&) {});

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
    return parse_a_component_value(token_stream());
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
    bool just_consumed_comma = false;
    while (!input.is_empty()) {

        // 1. Consume a list of component values from input, with <comma-token> as the stop token, and append the result to groups.
        groups.append(consume_a_list_of_component_values(input, Token::Type::Comma));

        // 2. Discard a token from input.
        just_consumed_comma = input.consume_a_token().is(Token::Type::Comma);
    }

    // AD-HOC: Also append an empty group if there was a trailing comma.
    // Some related spec discussion: https://github.com/w3c/csswg-drafts/issues/11254
    if (just_consumed_comma)
        groups.append({});

    // 4. Return groups.
    return groups;
}

// https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
Parser::PropertiesAndCustomProperties Parser::parse_as_property_declaration_block()
{
    auto expand_shorthands = [&](Vector<StyleProperty>& properties) -> Vector<StyleProperty> {
        Vector<StyleProperty> expanded_properties;
        for (auto& property : properties) {
            if (property_is_shorthand(property.property_id)) {
                StyleComputer::for_each_property_expanding_shorthands(property.property_id, *property.value, [&](PropertyID longhand_property_id, StyleValue const& longhand_value) {
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

    // 1. Let declarations be the returned declarations from invoking parse a block’s contents with string.
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context, PreservePropertySourceText::Yes);
    if (should_verify_rust_syntax_parser())
        verify_rust_syntax_declarations(declarations_and_at_rules, parse_a_blocks_contents(token_stream()));

    // 2. Let parsed declarations be a new empty list.
    PropertiesAndCustomProperties parsed_declarations;

    // 3. For each item declaration in declarations, follow these substeps:
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (rule_or_list.has<Rule>())
            continue;

        auto& rule_declarations = rule_or_list.get<Vector<Declaration>>();
        for (auto const& declaration : rule_declarations) {
            // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
            //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
            //    parsed declaration be null.
            // 2. If parsed declaration is not null, append it to parsed declarations.
            extract_property(declaration, parsed_declarations);
        }
    }
    parsed_declarations.properties = expand_shorthands(parsed_declarations.properties);

    // 4. Return parsed declarations.
    return parsed_declarations;
}

Vector<DevToolsStyleDeclaration> Parser::parse_as_devtools_property_declaration_block()
{
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context);

    Vector<DevToolsStyleDeclaration> parsed_declarations;
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (auto* rule_declarations = rule_or_list.get_pointer<Vector<Declaration>>()) {
            for (auto const& declaration : *rule_declarations) {
                auto property = PropertyNameAndID::from_name(declaration.name);

                parsed_declarations.append(DevToolsStyleDeclaration {
                    .name = declaration.name,
                    .value = declaration.value_text,
                    .important = declaration.important,
                    .is_custom_property = property.has_value() && property->is_custom_property(),
                    .is_name_valid = property.has_value(),
                    .is_valid = property.has_value() && convert_to_style_property(declaration).has_value(),
                });
            }
        }
    }

    return parsed_declarations;
}

Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const& parsing_params, StringView declaration_block)
{
    auto devtools_parsing_params = parsing_params;
    if (devtools_parsing_params.rule_context.is_empty())
        devtools_parsing_params.rule_context.append(RuleContext::Style);

    auto parser = Parser::create(devtools_parsing_params, declaration_block);
    return parser.parse_as_devtools_property_declaration_block();
}

Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const& parsing_params, Utf16View declaration_block)
{
    auto devtools_parsing_params = parsing_params;
    if (devtools_parsing_params.rule_context.is_empty())
        devtools_parsing_params.rule_context.append(RuleContext::Style);

    auto parser = Parser::create(devtools_parsing_params, declaration_block);
    return parser.parse_as_devtools_property_declaration_block();
}

// https://drafts.csswg.org/cssom/#parse-a-css-declaration-block
Vector<Descriptor> Parser::parse_as_descriptor_declaration_block(AtRuleID at_rule_id)
{
    auto context_type = [at_rule_id] {
        switch (at_rule_id) {
        case AtRuleID::FontFace:
            return RuleContext::AtFontFace;
        case AtRuleID::Function:
            return RuleContext::AtFunction;
        case AtRuleID::Page:
            return RuleContext::AtPage;
        case AtRuleID::Property:
            return RuleContext::AtProperty;
        case AtRuleID::CounterStyle:
            // NB: We don't actually have a `CSSDescriptors` for `@counter-style` so this function shouldn't ever be
            //     called with `AtRuleID::CounterStyle`.
            VERIFY_NOT_REACHED();
        }
        VERIFY_NOT_REACHED();
    }();

    // 1. Let declarations be the returned declarations from invoking parse a block’s contents with string.
    m_rule_context.append(context_type);
    auto declarations_and_at_rules = RustSyntaxParser::parse_block_contents(*this, m_rule_context);
    m_rule_context.take_last();

    // 2. Let parsed declarations be a new empty list.
    Vector<Descriptor> parsed_declarations;

    // 3. For each item declaration in declarations, follow these substeps:
    for (auto const& rule_or_list : declarations_and_at_rules) {
        if (rule_or_list.has<Rule>())
            continue;

        auto& rule_declarations = rule_or_list.get<Vector<Declaration>>();
        for (auto const& declaration : rule_declarations) {
            // 1. Let parsed declaration be the result of parsing declaration according to the appropriate CSS
            //    specifications, dropping parts that are said to be ignored. If the whole declaration is dropped, let
            //    parsed declaration be null.
            // 2. If parsed declaration is not null, append it to parsed declarations.
            if (auto parsed_declaration = convert_to_descriptor(at_rule_id, declaration); parsed_declaration.has_value())
                parsed_declarations.append(parsed_declaration.release_value());
        }
    }

    // 4. Return parsed declarations.
    return parsed_declarations;
}

bool Parser::is_valid_in_the_current_context(Declaration const& declaration) const
{
    // TODO: Determine if this *particular* declaration is valid here, not just declarations in general.

    // Declarations can't appear at the top level
    if (m_rule_context.is_empty())
        return false;

    switch (m_rule_context.last()) {
    case RuleContext::Unknown:
        // If the context is an unknown type, we don't accept anything.
        return false;

    case RuleContext::Style:
        // Style rules contain property declarations
        return true;

    case RuleContext::Keyframe: {
        // https://drafts.csswg.org/css-animations-1/#keyframes
        // The <declaration-list> inside of <keyframe-block> accepts any CSS property except those defined in this
        // specification, but does accept the animation-timing-function property and interprets it specially
        // NB: animation-composition is defined in CSS Animations Level 2, so it is not excluded by this rule.
        auto property = PropertyNameAndID::from_name(declaration.name);
        if (!property.has_value())
            return true;
        switch (property->id()) {
        case PropertyID::Animation:
        case PropertyID::AnimationDelay:
        case PropertyID::AnimationDirection:
        case PropertyID::AnimationDuration:
        case PropertyID::AnimationFillMode:
        case PropertyID::AnimationIterationCount:
        case PropertyID::AnimationName:
        case PropertyID::AnimationPlayState:
        case PropertyID::AnimationTimeline:
            return false;
        default:
            return true;
        }
    }

    case RuleContext::AtContainer:
    case RuleContext::AtLayer:
    case RuleContext::AtMedia:
    case RuleContext::AtSupports:
        // Grouping rules can contain declarations if they are themselves inside a style or function rule
        return m_rule_context.contains([](auto const& context) { return context == RuleContext::Style || context == RuleContext::AtFunction; });

    case RuleContext::AtScope:
        // @scope can contain declarations directly, matching the scoping root with zero specificity.
        return true;

    case RuleContext::FontFeatureValue:
        // Each feature value block accepts a list of declarations
        return true;

    case RuleContext::AtFunction:
        // @function rules contain descriptor declarations
        return true;

    case RuleContext::AtCounterStyle:
    case RuleContext::AtFontFace:
    case RuleContext::AtFontFeatureValues:
    case RuleContext::AtPage:
    case RuleContext::AtProperty:
    case RuleContext::Margin:
        // These have descriptor declarations
        return true;

    case RuleContext::AtKeyframes:
        // @keyframes can only contain keyframe rules
        return false;

    case RuleContext::SupportsCondition:
        // @supports conditions accept all declarations
        return true;
    }

    VERIFY_NOT_REACHED();
}

bool Parser::is_valid_in_the_current_context(AtRule const& at_rule) const
{
    // All at-rules can appear at the top level, except margin rules
    if (m_rule_context.is_empty())
        return !is_margin_rule_name(at_rule.name);

    // Only grouping rules can be nested within style rules
    if (m_rule_context.contains_slow(RuleContext::Style))
        return at_rule.name.is_one_of("container"sv, "layer"sv, "media"sv, "scope"sv, "supports"sv);

    if (m_rule_context.contains_slow(RuleContext::AtFunction)) {
        // https://drafts.csswg.org/css-mixins-1/#function-body
        // The body of a @function rule accepts conditional group rules
        return at_rule.name.is_one_of("container"sv, "media"sv, "supports"sv);
    }

    switch (m_rule_context.last()) {
    case RuleContext::Unknown:
        // If the context is an unknown type, we don't accept anything.
        return false;

    case RuleContext::Style:
        // Already handled above
        VERIFY_NOT_REACHED();

    case RuleContext::AtContainer:
    case RuleContext::AtLayer:
    case RuleContext::AtMedia:
    case RuleContext::AtScope:
    case RuleContext::AtSupports:
        // Grouping rules can contain anything except @import or @namespace
        return !at_rule.name.is_one_of("import"sv, "namespace"sv);

    case RuleContext::SupportsCondition:
        // @supports cannot check for at-rules
        return false;

    case RuleContext::AtPage:
        // @page rules can contain margin rules
        return is_margin_rule_name(at_rule.name);

    case RuleContext::AtCounterStyle:
    case RuleContext::AtFontFace:
    case RuleContext::FontFeatureValue:
    case RuleContext::AtKeyframes:
    case RuleContext::Keyframe:
    case RuleContext::AtProperty:
    case RuleContext::Margin:
        // These can't contain any at-rules
        return false;
    case RuleContext::AtFontFeatureValues:
        return CSSFontFeatureValuesRule::is_font_feature_value_type_at_keyword(at_rule.name);
    case RuleContext::AtFunction:
        // Already handled above
        VERIFY_NOT_REACHED();
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
    case RuleContext::Unknown:
        // If the context is an unknown type, we don't accept anything.
        return false;

    case RuleContext::Style:
        // Style rules can contain style rules
        return true;

    case RuleContext::AtContainer:
    case RuleContext::AtLayer:
    case RuleContext::AtMedia:
    case RuleContext::AtScope:
    case RuleContext::AtSupports:
        // Grouping rules can contain style rules
        return true;

    case RuleContext::AtKeyframes:
        // @keyframes can contain keyframe rules
        return true;

    case RuleContext::SupportsCondition:
        // @supports cannot check qualified rules
        return false;

    case RuleContext::AtCounterStyle:
    case RuleContext::AtFontFace:
    case RuleContext::AtFontFeatureValues:
    case RuleContext::FontFeatureValue:
    case RuleContext::AtFunction:
    case RuleContext::AtPage:
    case RuleContext::AtProperty:
    case RuleContext::Keyframe:
    case RuleContext::Margin:
        // These can't contain qualified rules
        return false;
    }

    VERIFY_NOT_REACHED();
}

void Parser::extract_property(Declaration const& declaration, PropertiesAndCustomProperties& dest)
{
    if (auto maybe_property_and_name = convert_to_style_property(declaration); maybe_property_and_name.has_value()) {
        auto property = maybe_property_and_name->property;
        if (property.property_id == PropertyID::Custom) {
            dest.custom_properties.set(maybe_property_and_name->name, property);
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
    return CSSStyleProperties::create(move(properties.properties), move(properties.custom_properties));
}

Optional<StylePropertyAndName> Parser::convert_to_style_property(Declaration const& declaration)
{
    auto property = PropertyNameAndID::from_name(declaration.name);

    if (!property.has_value()) {
        if (has_ignored_vendor_prefix(declaration.name)) {
            return {};
        }
        ErrorReporter::the().report(UnknownPropertyError { .property_name = declaration.name });
        return {};
    }

    RefPtr<StyleValue const> legacy_value;
    if (declaration.name.equals_ignoring_ascii_case("-webkit-box-orient"sv)) {
        // INTEROP: -webkit-box-orient predates flex-direction and uses horizontal and vertical
        //          for the values now represented by row and column, respectively.
        auto value = declaration.value_text.trim_ascii_whitespace();
        if (value.equals_ignoring_ascii_case("horizontal"sv))
            legacy_value = KeywordStyleValue::create(Keyword::Row);
        else if (value.equals_ignoring_ascii_case("vertical"sv))
            legacy_value = KeywordStyleValue::create(Keyword::Column);
    }

    if (declaration.parsed_property_id.has_value()) {
        auto value = legacy_value ? legacy_value : declaration.parsed_value;
        if (!value) {
            ErrorReporter::the().report(InvalidPropertyError {
                .property_name = property->name(),
                .value_string = declaration.value_text.to_utf8(),
                .description = "Failed to parse."_string,
            });
            return {};
        }
        return property->is_custom_property()
            ? StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_nonnull() }, property->name() }
            : StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_nonnull() } };
    }

    auto value = legacy_value
        ? ParseErrorOr<NonnullRefPtr<StyleValue const>> { legacy_value.release_nonnull() }
        : parse_css_value_from_source(property->id(), declaration.original_value_text.value_or(declaration.value_text));
    if (value.is_error())
        return {};
    return property->is_custom_property()
        ? StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_value() }, property->name() }
        : StylePropertyAndName { StyleProperty { declaration.important, property->id(), value.release_value() } };
}

Vector<ComponentValue> Parser::parse_as_list_of_component_values()
{
    return parse_a_list_of_component_values(token_stream());
}

RefPtr<StyleValue const> Parser::parse_as_css_value(PropertyID property_id)
{
    auto parsed_value = parse_css_value_from_source(property_id, m_source);
    if (parsed_value.is_error())
        return nullptr;
    return parsed_value.release_value();
}

RefPtr<StyleValue const> Parser::parse_css_value_from_filtered_source(ParsingParams const& context, Utf16View source, PropertyID property_id)
{
    Parser parser { context, {} };
    auto parsed_value = parser.parse_css_value_from_source(property_id, source);
    if (parsed_value.is_error())
        return nullptr;
    return parsed_value.release_value();
}

RefPtr<StyleValue const> Parser::parse_as_descriptor_value(AtRuleID at_rule_id, DescriptorNameAndID const& descriptor_name_and_id)
{
    return RustSyntaxParser::parse_descriptor(*this, at_rule_id, descriptor_name_and_id);
}

RefPtr<StyleValue const> Parser::parse_as_type(ValueType value_type)
{
    return parse_primitive_value_from_source(value_type, m_source);
}

// https://html.spec.whatwg.org/multipage/images.html#parsing-a-sizes-attribute
NonnullRefPtr<StyleValue const> Parser::parse_as_sizes_attribute(DOM::Element const& element, HTML::HTMLImageElement const* img)
{
    // When asked to parse a sizes attribute from an element element, with an img element or null img:

    // AD-HOC: If element has no sizes attribute, this algorithm always logs a parse error and then returns 100vw.
    //         The attribute is optional, so avoid spamming the debug log with false positives by just returning early.
    if (!element.has_attribute(HTML::AttributeNames::sizes))
        return LengthStyleValue::create(Length(100, LengthUnit::Vw));

    // 1. Let unparsed sizes list be the result of parsing a comma-separated list of component values
    //    from the value of element's sizes attribute (or the empty string, if the attribute is absent).
    auto unparsed_sizes_list = parse_a_comma_separated_list_of_component_values(token_stream());

    // 2. Let size be null.
    RefPtr<StyleValue const> size;

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
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "sizes attribute"_utf16_fly_string,
                .value_string = token_stream().dump_string(),
                .description = "Failed in step 3.1; all whitespace"_string,
            });
            continue;
        }

        // 2. If the last component value in unparsed size is a valid non-negative <source-size-value>,
        //    then set size to its value and remove the component value from unparsed size.
        //    Any CSS function other than the math functions is invalid.
        //    Otherwise, there is a parse error; continue.
        auto last_value_source = serialize_a_series_of_component_values_preserving_original_source_text({ &unparsed_size.last(), 1 });
        if (auto source_size_value = RustQueryParser::parse_source_size_value(*this, last_value_source)) {
            size = source_size_value.release_nonnull();
            unparsed_size.take_last();
        } else {
            log_parse_error();
            ErrorReporter::the().report(InvalidValueError {
                .value_type = "sizes attribute"_utf16_fly_string,
                .value_string = token_stream().dump_string(),
                .description = "Failed in step 3.2; couldn't parse {} as a <source-size-value>"_string,
            });
            continue;
        }

        // 3. If size is auto, and img is not null, and img is being rendered, and img allows auto-sizes,
        //    then set size to the concrete object size width of img, in CSS pixels.
        // FIXME: "img is being rendered" - we just see if it has image data for now
        if (size->has_auto() && img && img->is_image_available() && img->allows_auto_sizes()) {
            // FIXME: The spec doesn't seem to tell us how to determine the concrete size of an <img>, so use the default sizing algorithm.
            //        Should this use some of the methods from FormattingContext?
            auto concrete_size = run_default_sizing_algorithm(
                img->width(), img->height(),
                { img->natural_width(), img->natural_height(), img->intrinsic_aspect_ratio() },
                // NOTE: https://html.spec.whatwg.org/multipage/rendering.html#img-contain-size
                CSSPixelSize { 300, 150 });
            size = LengthStyleValue::create(Length::make_px(concrete_size.width()));
        }

        // 4. Remove all consecutive <whitespace-token>s from the end of unparsed size.
        //    If unparsed size is now empty:
        remove_all_consecutive_whitespace_tokens_from_the_end_of(unparsed_size);
        if (unparsed_size.is_empty()) {
            // 1. If this was not the last item in unparsed sizes list, that is a parse error.
            if (i != unparsed_sizes_list.size() - 1) {
                log_parse_error();
                ErrorReporter::the().report(InvalidValueError {
                    .value_type = "sizes attribute"_utf16_fly_string,
                    .value_string = token_stream().dump_string(),
                    .description = MUST(String::formatted("Failed in step 3.4.1; is unparsed size #{}, count {}", i, unparsed_sizes_list.size())),
                });
            }

            // 2. If size is not auto, then return size. Otherwise, continue.
            if (!size->has_auto())
                return size.release_nonnull();
            continue;
        }

        // 5. Parse the remaining component values in unparsed size as a <media-condition>.
        //    If it does not parse correctly, or it does parse correctly but the <media-condition> evaluates to false, continue.
        auto media_condition_source = serialize_a_series_of_component_values_preserving_original_source_text(unparsed_size);
        auto media_condition = RustQueryParser::parse_media_condition(*this, media_condition_source);
        if (!media_condition)
            continue;

        // https://drafts.csswg.org/mediaqueries-5/#evaluating
        // "If the result of any of the above productions is used in any
        // context that expects a two-valued boolean, 'unknown' must be
        // converted to 'false'."
        if (m_document && !media_condition->evaluate_to_boolean({ .document = m_document }))
            continue;

        // 5. If size is not auto, then return size. Otherwise, continue.
        if (!size->has_auto())
            return size.release_nonnull();
    }

    // 4. Return 100vw.
    return LengthStyleValue::create(Length(100, LengthUnit::Vw));
}

bool Parser::has_ignored_vendor_prefix(Utf16View string)
{
    if (!string.starts_with('-'))
        return false;
    if (string.starts_with("--"sv))
        return false;
    if (string.starts_with("-libweb-"sv))
        return false;
    if (string.count("-"sv) == 1)
        return false;
    return true;
}

template Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<Token>&, Optional<::URL::URL>);
template Parser::ParsedStyleSheet Parser::parse_a_stylesheet(TokenStream<ComponentValue>&, Optional<::URL::URL>);

template Vector<Rule> Parser::parse_a_stylesheets_contents(TokenStream<Token>& input);
template Vector<Rule> Parser::parse_a_stylesheets_contents(TokenStream<ComponentValue>& input);

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

template Optional<Declaration> Parser::consume_a_declaration(TokenStream<Token>&, Nested, SaveOriginalText);
template Optional<Declaration> Parser::consume_a_declaration(TokenStream<ComponentValue>&, Nested, SaveOriginalText);

template void Parser::consume_the_remnants_of_a_bad_declaration(TokenStream<Token>&, Nested);
template void Parser::consume_the_remnants_of_a_bad_declaration(TokenStream<ComponentValue>&, Nested);

template Optional<Rule> Parser::parse_a_rule(TokenStream<Token>&, Nested);
template Optional<Rule> Parser::parse_a_rule(TokenStream<ComponentValue>&, Nested);

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
    return m_document.ptr();
}

HTML::Window const* Parser::window() const
{
    if (!m_document)
        return nullptr;
    return m_document->window().ptr();
}

bool Parser::in_quirks_mode() const
{
    return m_document ? m_document->in_quirks_mode() : false;
}

bool Parser::is_parsing_svg_presentation_attribute() const
{
    return m_parsing_mode == ParsingMode::SVGPresentationAttribute;
}

}
