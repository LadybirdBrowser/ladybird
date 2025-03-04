/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/SyntaxHighlighter.h>
#include <LibJS/Token.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWebView/SourceHighlighter.h>

namespace WebView {

SourceDocument::SourceDocument(String const& source)
{
    // HTML, CSS and JS differ slightly on what they consider a newline to be.
    // In order to make them get along in documents that include a mix of the three, process the source to make the
    // newlines consistent before doing any highlighting.

    // Optimization: If all the newlines are \n, just use the input string.
    if (!source.code_points().contains_any_of(Array<u32, 3> { '\r', 0x2028, 0x2029 })) {
        m_source = source;
    } else {
        StringBuilder builder { source.byte_count() };
        // Convert any '\r\n', \r, <LS> or <PS> to \n
        bool previous_was_cr = false;
        for (u32 code_point : source.code_points()) {
            if (previous_was_cr && code_point != '\n')
                builder.append('\n');
            previous_was_cr = false;

            switch (code_point) {
            case '\r':
                previous_was_cr = true;
                break;
            case JS::LINE_SEPARATOR:
            case JS::PARAGRAPH_SEPARATOR:
                builder.append('\n');
                break;
            default:
                builder.append_code_point(code_point);
            }
        }
        m_source = builder.to_string_without_validation();
    }

    m_source.code_points().for_each_split_view(
        [](u32 it) { return it == '\n'; },
        SplitBehavior::KeepEmpty,
        [&](auto line) {
            m_lines.append(Syntax::TextDocumentLine { *this, line.as_string() });
        });
}

Syntax::TextDocumentLine& SourceDocument::line(size_t line_index)
{
    return m_lines[line_index];
}

Syntax::TextDocumentLine const& SourceDocument::line(size_t line_index) const
{
    return m_lines[line_index];
}

SourceHighlighterClient::SourceHighlighterClient(String const& source, Syntax::Language language)
    : m_document(SourceDocument::create(source))
{
    // HACK: Syntax highlighters require a palette, but we don't actually care about the output styling, only the type of token for each span.
    //       Also, getting a palette from the chrome is nontrivial. So, create a dummy blank one and use that.
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
    auto palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
    Gfx::Palette dummy_palette { palette_impl };

    switch (language) {
    case Syntax::Language::CSS:
        m_highlighter = make<Web::CSS::SyntaxHighlighter>();
        break;
    case Syntax::Language::HTML:
        m_highlighter = make<Web::HTML::SyntaxHighlighter>();
        break;
    case Syntax::Language::JavaScript:
        m_highlighter = make<JS::SyntaxHighlighter>();
        break;
    default:
        break;
    }

    if (m_highlighter) {
        m_highlighter->attach(*this);
        m_highlighter->rehighlight(dummy_palette);
    }
}

Vector<Syntax::TextDocumentSpan> const& SourceHighlighterClient::spans() const
{
    return document().spans();
}

void SourceHighlighterClient::set_span_at_index(size_t index, Syntax::TextDocumentSpan span)
{
    document().set_span_at_index(index, span);
}

Vector<Syntax::TextDocumentFoldingRegion>& SourceHighlighterClient::folding_regions()
{
    return document().folding_regions();
}

Vector<Syntax::TextDocumentFoldingRegion> const& SourceHighlighterClient::folding_regions() const
{
    return document().folding_regions();
}

ByteString SourceHighlighterClient::highlighter_did_request_text() const
{
    return document().text();
}

void SourceHighlighterClient::highlighter_did_request_update()
{
    // No-op
}

Syntax::Document& SourceHighlighterClient::highlighter_did_request_document()
{
    return document();
}

Syntax::TextPosition SourceHighlighterClient::highlighter_did_request_cursor() const
{
    return {};
}

void SourceHighlighterClient::highlighter_did_set_spans(Vector<Syntax::TextDocumentSpan> spans)
{
    document().set_spans(span_collection_index, move(spans));
}

void SourceHighlighterClient::highlighter_did_set_folding_regions(Vector<Syntax::TextDocumentFoldingRegion> folding_regions)
{
    document().set_folding_regions(move(folding_regions));
}

String highlight_source(URL::URL const& url, URL::URL const& base_url, String const& source, Syntax::Language language, HighlightOutputMode mode)
{
    SourceHighlighterClient highlighter_client { source, language };
    return highlighter_client.to_html_string(url, base_url, mode);
}

StringView SourceHighlighterClient::class_for_token(u64 token_type) const
{
    auto class_for_css_token = [](u64 token_type) {
        switch (static_cast<Web::CSS::Parser::Token::Type>(token_type)) {
        case Web::CSS::Parser::Token::Type::Invalid:
        case Web::CSS::Parser::Token::Type::BadString:
        case Web::CSS::Parser::Token::Type::BadUrl:
            return "invalid"sv;
        case Web::CSS::Parser::Token::Type::Ident:
            return "identifier"sv;
        case Web::CSS::Parser::Token::Type::Function:
            return "function"sv;
        case Web::CSS::Parser::Token::Type::AtKeyword:
            return "at-keyword"sv;
        case Web::CSS::Parser::Token::Type::Hash:
            return "hash"sv;
        case Web::CSS::Parser::Token::Type::String:
            return "string"sv;
        case Web::CSS::Parser::Token::Type::Url:
            return "url"sv;
        case Web::CSS::Parser::Token::Type::Number:
        case Web::CSS::Parser::Token::Type::Dimension:
        case Web::CSS::Parser::Token::Type::Percentage:
            return "number"sv;
        case Web::CSS::Parser::Token::Type::Whitespace:
            return "whitespace"sv;
        case Web::CSS::Parser::Token::Type::Delim:
        case Web::CSS::Parser::Token::Type::Colon:
        case Web::CSS::Parser::Token::Type::Semicolon:
        case Web::CSS::Parser::Token::Type::Comma:
        case Web::CSS::Parser::Token::Type::OpenSquare:
        case Web::CSS::Parser::Token::Type::CloseSquare:
        case Web::CSS::Parser::Token::Type::OpenParen:
        case Web::CSS::Parser::Token::Type::CloseParen:
        case Web::CSS::Parser::Token::Type::OpenCurly:
        case Web::CSS::Parser::Token::Type::CloseCurly:
            return "delimiter"sv;
        case Web::CSS::Parser::Token::Type::CDO:
        case Web::CSS::Parser::Token::Type::CDC:
            return "comment"sv;
        case Web::CSS::Parser::Token::Type::EndOfFile:
        default:
            break;
        }
        return ""sv;
    };

    auto class_for_js_token = [](u64 token_type) {
        auto category = JS::Token::category(static_cast<JS::TokenType>(token_type));
        switch (category) {
        case JS::TokenCategory::Invalid:
            return "invalid"sv;
        case JS::TokenCategory::Trivia:
            return "comment"sv;
        case JS::TokenCategory::Number:
            return "number"sv;
        case JS::TokenCategory::String:
            return "string"sv;
        case JS::TokenCategory::Punctuation:
            return "punctuation"sv;
        case JS::TokenCategory::Operator:
            return "operator"sv;
        case JS::TokenCategory::Keyword:
            return "keyword"sv;
        case JS::TokenCategory::ControlKeyword:
            return "control-keyword"sv;
        case JS::TokenCategory::Identifier:
            return "identifier"sv;
        default:
            break;
        }
        return ""sv;
    };

    switch (m_highlighter->language()) {
    case Syntax::Language::CSS:
        return class_for_css_token(token_type);
    case Syntax::Language::JavaScript:
        return class_for_js_token(token_type);
    case Syntax::Language::HTML: {
        // HTML has nested CSS and JS highlighters, so we have to decode their token types.

        // HTML
        if (token_type < Web::HTML::SyntaxHighlighter::JS_TOKEN_START_VALUE) {
            switch (static_cast<Web::HTML::AugmentedTokenKind>(token_type)) {
            case Web::HTML::AugmentedTokenKind::AttributeName:
                return "attribute-name"sv;
            case Web::HTML::AugmentedTokenKind::AttributeValue:
                return "attribute-value"sv;
            case Web::HTML::AugmentedTokenKind::OpenTag:
            case Web::HTML::AugmentedTokenKind::CloseTag:
                return "tag"sv;
            case Web::HTML::AugmentedTokenKind::Comment:
                return "comment"sv;
            case Web::HTML::AugmentedTokenKind::Doctype:
                return "doctype"sv;
            case Web::HTML::AugmentedTokenKind::__Count:
            default:
                return ""sv;
            }
        }

        // JS
        if (token_type < Web::HTML::SyntaxHighlighter::CSS_TOKEN_START_VALUE) {
            return class_for_js_token(token_type - Web::HTML::SyntaxHighlighter::JS_TOKEN_START_VALUE);
        }

        // CSS
        return class_for_css_token(token_type - Web::HTML::SyntaxHighlighter::CSS_TOKEN_START_VALUE);
    }
    default:
        return "unknown"sv;
    }
}

String SourceHighlighterClient::to_html_string(URL::URL const& url, URL::URL const& base_url, HighlightOutputMode mode) const
{
    StringBuilder builder;

    auto append_escaped = [&](Utf32View text) {
        for (auto code_point : text) {
            if (code_point == '&') {
                builder.append("&amp;"sv);
            } else if (code_point == 0xA0) {
                builder.append("&nbsp;"sv);
            } else if (code_point == '<') {
                builder.append("&lt;"sv);
            } else if (code_point == '>') {
                builder.append("&gt;"sv);
            } else {
                builder.append_code_point(code_point);
            }
        }
    };

    auto start_token = [&](u64 type) {
        builder.appendff("<span class=\"{}\">", class_for_token(type));
    };
    auto end_token = [&]() {
        builder.append("</span>"sv);
    };

    if (mode == HighlightOutputMode::FullDocument) {
        builder.append(R"~~~(
<!DOCTYPE html>
<html>
<head>
    <meta name="color-scheme" content="dark light">)~~~"sv);

        builder.appendff("<title>View Source - {}</title>", escape_html_entities(url.serialize_for_display()));
        builder.appendff("<style type=\"text/css\">{}</style>", HTML_HIGHLIGHTER_STYLE);
        builder.append(R"~~~(
</head>
<body>)~~~"sv);
    }
    builder.append("<pre class=\"html\">"sv);

    static constexpr auto href = to_array<u32>({ 'h', 'r', 'e', 'f' });
    static constexpr auto src = to_array<u32>({ 's', 'r', 'c' });
    bool linkify_attribute = false;

    auto resolve_url_for_attribute = [&](Utf32View const& attribute_value) -> Optional<URL::URL> {
        if (!linkify_attribute)
            return {};

        auto attribute_url = MUST(String::formatted("{}", attribute_value));
        auto attribute_url_without_quotes = attribute_url.bytes_as_string_view().trim("\""sv);

        return Web::DOMURL::parse(attribute_url_without_quotes, base_url);
    };

    size_t span_index = 0;
    for (size_t line_index = 0; line_index < document().line_count(); ++line_index) {
        auto& line = document().line(line_index);
        auto line_view = line.view();
        builder.append("<div class=\"line\">"sv);

        size_t next_column = 0;

        auto draw_text_helper = [&](size_t start, size_t end, Optional<Syntax::TextDocumentSpan const&> span) {
            size_t length = end - start;
            if (length == 0)
                return;

            auto text = line_view.substring_view(start, length);

            if (span.has_value()) {
                bool append_anchor_close = false;

                if (span->data == to_underlying(Web::HTML::AugmentedTokenKind::AttributeName)) {
                    linkify_attribute = text == Utf32View { href } || text == Utf32View { src };
                } else if (span->data == to_underlying(Web::HTML::AugmentedTokenKind::AttributeValue)) {
                    if (auto href = resolve_url_for_attribute(text); href.has_value()) {
                        builder.appendff("<a href=\"{}\">", *href);
                        append_anchor_close = true;
                    }
                }

                start_token(span->data);
                append_escaped(text);
                end_token();

                if (append_anchor_close)
                    builder.append("</a>"sv);
            } else {
                append_escaped(text);
            }
        };

        while (span_index < document().spans().size()) {
            auto& span = document().spans()[span_index];
            if (span.range.start().line() > line_index) {
                // No more spans in this line, moving on
                break;
            }
            size_t span_start;
            if (span.range.start().line() < line_index) {
                span_start = 0;
            } else {
                span_start = span.range.start().column();
            }
            size_t span_end;
            bool span_consumed;
            if (span.range.end().line() > line_index) {
                span_end = line.length();
                span_consumed = false;
            } else {
                span_end = span.range.end().column();
                span_consumed = true;
            }

            if (span_start != next_column) {
                // Draw unspanned text between spans
                draw_text_helper(next_column, span_start, {});
            }
            draw_text_helper(span_start, span_end, span);
            next_column = span_end;
            if (!span_consumed) {
                // Continue with same span on next line
                break;
            } else {
                ++span_index;
            }
        }
        // Draw unspanned text after last span
        if (next_column < line.length()) {
            draw_text_helper(next_column, line.length(), {});
        }

        builder.append("</div>"sv);
    }

    builder.append("</pre>"sv);
    if (mode == HighlightOutputMode::FullDocument) {
        builder.append(R"~~~(
</body>
</html>
)~~~"sv);
    }

    return builder.to_string_without_validation();
}

}
