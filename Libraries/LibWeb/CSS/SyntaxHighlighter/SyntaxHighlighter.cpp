/*
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>
#include <LibWeb/CSS/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWeb/RustFFI.h>

namespace Web::CSS {

void SyntaxHighlighter::rehighlight(Palette const& palette)
{
    dbgln_if(SYNTAX_HIGHLIGHTING_DEBUG, "(CSS::SyntaxHighlighter) starting rehighlight");
    auto text = m_client->get_text();

    Vector<Syntax::TextDocumentSpan> spans;

    auto highlight = [&](auto start_line, auto start_column, auto end_line, auto end_column, Gfx::TextAttributes attributes, CSS::Parser::FFI::CssTokenType type) {
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

    auto filtered_input = CSS::Parser::RustTokenizer::normalize_input(text, "utf-8"sv);
    auto filtered_input_view = filtered_input.utf16_view();
    Vector<CSS::Parser::FFI::CssSyntaxToken> tokens;
    tokens.ensure_capacity((filtered_input_view.length_in_code_units() / 2) + 1);
    CSS::Parser::FFI::rust_css_tokenize_for_syntax_highlighting(
        filtered_input_view.has_ascii_storage() ? reinterpret_cast<u8 const*>(filtered_input_view.ascii_span().data()) : nullptr,
        filtered_input_view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(filtered_input_view.utf16_span().data()),
        filtered_input_view.length_in_code_units(),
        &tokens,
        [](void* raw_tokens, CSS::Parser::FFI::CssSyntaxToken const* token) {
            static_cast<Vector<CSS::Parser::FFI::CssSyntaxToken>*>(raw_tokens)->append(*token);
        });

    for (auto const& token : tokens) {
        if (token.token_type == Parser::FFI::CssTokenType::EndOfFile)
            break;

        switch (token.token_type) {
        case Parser::FFI::CssTokenType::Ident:
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_identifier(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::String:
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_string(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::Whitespace:
            // CSS doesn't produce comment tokens, they're just included as part of whitespace.
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_comment(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::AtKeyword:
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_keyword(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::Function:
            // Function tokens include the opening '(', so we split that into two tokens for highlighting purposes.
            highlight(token.start_line, token.start_column, token.end_line, token.end_column - 1, { palette.syntax_keyword(), {} }, token.token_type);
            highlight(token.end_line, token.end_column - 1, token.end_line, token.end_column, { palette.syntax_punctuation(), {} }, Parser::FFI::CssTokenType::OpenParen);
            break;

        case Parser::FFI::CssTokenType::Url:
            // An Url token is a `url()` function with its parameter string unquoted.
            // url
            highlight(token.start_line, token.start_column, token.start_line, token.start_column + 3, { palette.syntax_keyword(), {} }, token.token_type);
            // (
            highlight(token.start_line, token.start_column + 3, token.start_line, token.start_column + 4, { palette.syntax_punctuation(), {} }, Parser::FFI::CssTokenType::OpenParen);
            // <string>
            highlight(token.start_line, token.start_column + 4, token.end_line, token.end_column - 1, { palette.syntax_string(), {} }, Parser::FFI::CssTokenType::String);
            // )
            highlight(token.end_line, token.end_column - 1, token.end_line, token.end_column, { palette.syntax_punctuation(), {} }, Parser::FFI::CssTokenType::CloseParen);
            break;

        case Parser::FFI::CssTokenType::Number:
        case Parser::FFI::CssTokenType::Dimension:
        case Parser::FFI::CssTokenType::Percentage:
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_number(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::Delim:
        case Parser::FFI::CssTokenType::Colon:
        case Parser::FFI::CssTokenType::Comma:
        case Parser::FFI::CssTokenType::Semicolon:
        case Parser::FFI::CssTokenType::OpenCurly:
        case Parser::FFI::CssTokenType::OpenParen:
        case Parser::FFI::CssTokenType::OpenSquare:
        case Parser::FFI::CssTokenType::CloseCurly:
        case Parser::FFI::CssTokenType::CloseParen:
        case Parser::FFI::CssTokenType::CloseSquare:
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_punctuation(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::CDO:
        case Parser::FFI::CssTokenType::CDC:
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_comment(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::Hash:
            // FIXME: Hash tokens can be ID selectors or colors, we don't know which without parsing properly.
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { palette.syntax_number(), {} }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::Invalid:
        case Parser::FFI::CssTokenType::BadUrl:
        case Parser::FFI::CssTokenType::BadString:
            // FIXME: Error highlighting color in palette?
            highlight(token.start_line, token.start_column, token.end_line, token.end_column, { Color(Color::NamedColor::Red), {}, true }, token.token_type);
            break;

        case Parser::FFI::CssTokenType::EndOfFile:
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
