/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>
#include <LibWeb/CSS/SyntaxHighlighter/SyntaxHighlighter.h>

namespace Web::CSS {

void SyntaxHighlighter::rehighlight(Palette const& palette)
{
    dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(CSS::SyntaxHighlighter) starting rehighlight");
    auto text = m_client->get_text();

    Vector<Syntax::TextDocumentSpan> spans;

    auto highlight = [&](auto start_line, auto start_column, auto end_line, auto end_column, Gfx::TextAttributes attributes, CSS::Parser::Token::Type type) {
        if (start_line > end_line || (start_line == end_line && start_column >= end_column)) {
            dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(CSS::SyntaxHighlighter) discarding ({}-{}) to ({}-{}) because it has zero or negative length", start_line, start_column, end_line, end_column);
            return;
        }
        dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(CSS::SyntaxHighlighter) highlighting ({}-{}) to ({}-{}) with color {}", start_line, start_column, end_line, end_column, attributes.color);
        spans.empend(
            Syntax::TextRange {
                { start_line, start_column },
                { end_line, end_column },
            },
            move(attributes),
            static_cast<u64>(type),
            false);
    };

    auto tokens = CSS::Parser::Tokenizer::tokenize(text, "utf-8"sv);
    for (auto const& token : tokens) {
        if (token.is(Parser::Token::Type::EndOfFile))
            break;

        switch (token.type()) {
        case Parser::Token::Type::Ident:
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_identifier(), {} }, token.type());
            break;

        case Parser::Token::Type::String:
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_string(), {} }, token.type());
            break;

        case Parser::Token::Type::Whitespace:
            // CSS doesn't produce comment tokens, they're just included as part of whitespace.
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_comment(), {} }, token.type());
            break;

        case Parser::Token::Type::AtKeyword:
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_keyword(), {} }, token.type());
            break;

        case Parser::Token::Type::Function:
            // Function tokens include the opening '(', so we split that into two tokens for highlighting purposes.
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column - 1, { palette.syntax_keyword(), {} }, token.type());
            highlight(token.end_position().line, token.end_position().column - 1, token.end_position().line, token.end_position().column, { palette.syntax_punctuation(), {} }, Parser::Token::Type::OpenParen);
            break;

        case Parser::Token::Type::Url:
            // An Url token is a `url()` function with its parameter string unquoted.
            // url
            highlight(token.start_position().line, token.start_position().column, token.start_position().line, token.start_position().column + 3, { palette.syntax_keyword(), {} }, token.type());
            // (
            highlight(token.start_position().line, token.start_position().column + 3, token.start_position().line, token.start_position().column + 4, { palette.syntax_punctuation(), {} }, Parser::Token::Type::OpenParen);
            // <string>
            highlight(token.start_position().line, token.start_position().column + 4, token.end_position().line, token.end_position().column - 1, { palette.syntax_string(), {} }, Parser::Token::Type::String);
            // )
            highlight(token.end_position().line, token.end_position().column - 1, token.end_position().line, token.end_position().column, { palette.syntax_punctuation(), {} }, Parser::Token::Type::CloseParen);
            break;

        case Parser::Token::Type::Number:
        case Parser::Token::Type::Dimension:
        case Parser::Token::Type::Percentage:
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_number(), {} }, token.type());
            break;

        case Parser::Token::Type::Delim:
        case Parser::Token::Type::Colon:
        case Parser::Token::Type::Comma:
        case Parser::Token::Type::Semicolon:
        case Parser::Token::Type::OpenCurly:
        case Parser::Token::Type::OpenParen:
        case Parser::Token::Type::OpenSquare:
        case Parser::Token::Type::CloseCurly:
        case Parser::Token::Type::CloseParen:
        case Parser::Token::Type::CloseSquare:
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_punctuation(), {} }, token.type());
            break;

        case Parser::Token::Type::CDO:
        case Parser::Token::Type::CDC:
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_comment(), {} }, token.type());
            break;

        case Parser::Token::Type::Hash:
            // FIXME: Hash tokens can be ID selectors or colors, we don't know which without parsing properly.
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { palette.syntax_number(), {} }, token.type());
            break;

        case Parser::Token::Type::Invalid:
        case Parser::Token::Type::BadUrl:
        case Parser::Token::Type::BadString:
            // FIXME: Error highlighting color in palette?
            highlight(token.start_position().line, token.start_position().column, token.end_position().line, token.end_position().column, { Color(Color::NamedColor::Red), {}, true }, token.type());
            break;

        case Parser::Token::Type::EndOfFile:
        default:
            break;
        }
    }

    if constexpr (SYNTAX_HIGHLIGHTING_DEBUG) {
        dbgln("(CSS::SyntaxHighlighter) list of all spans:");
        for (auto& span : spans)
            dbgln("{}, {} - {}", span.range, span.attributes.color, span.data);
        dbgln("(CSS::SyntaxHighlighter) end of list");
    }

    m_client->do_set_spans(move(spans));
}

}
