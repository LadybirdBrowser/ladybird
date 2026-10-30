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
#include <LibWeb/CSS/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/HTML/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWeb/Loader/SourceHighlighter.h>
#include <LibWeb/RustFFI.h>

namespace Web {

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
    //       Also, getting a palette from the UI is nontrivial. So, create a dummy blank one and use that.
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

StringView SourceHighlighterClient::highlighter_did_request_text() const
{
    return document().text();
}

void SourceHighlighterClient::highlighter_did_set_spans(Vector<Syntax::TextDocumentSpan> spans)
{
    document().set_spans(move(spans));
}

String highlight_source(Optional<URL::URL> const& url, URL::URL const& base_url, String const& source, Syntax::Language language)
{
    SourceHighlighterClient highlighter_client { source, language };
    return highlighter_client.to_html_string(url, base_url);
}

StringView SourceHighlighterClient::class_for_token(u64 token_type) const
{
    auto class_for_css_token = [](u64 token_type) {
        switch (static_cast<Web::CSS::Parser::FFI::CssTokenType>(token_type)) {
        case Web::CSS::Parser::FFI::CssTokenType::BadString:
        case Web::CSS::Parser::FFI::CssTokenType::BadUrl:
            return "invalid"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Ident:
            return "identifier"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Function:
            return "function"sv;
        case Web::CSS::Parser::FFI::CssTokenType::AtKeyword:
            return "at-keyword"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Hash:
            return "hash"sv;
        case Web::CSS::Parser::FFI::CssTokenType::String:
            return "string"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Url:
            return "url"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Number:
        case Web::CSS::Parser::FFI::CssTokenType::Dimension:
        case Web::CSS::Parser::FFI::CssTokenType::Percentage:
            return "number"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Whitespace:
            return "whitespace"sv;
        case Web::CSS::Parser::FFI::CssTokenType::Delim:
        case Web::CSS::Parser::FFI::CssTokenType::Colon:
        case Web::CSS::Parser::FFI::CssTokenType::Semicolon:
        case Web::CSS::Parser::FFI::CssTokenType::Comma:
        case Web::CSS::Parser::FFI::CssTokenType::OpenSquare:
        case Web::CSS::Parser::FFI::CssTokenType::CloseSquare:
        case Web::CSS::Parser::FFI::CssTokenType::OpenParen:
        case Web::CSS::Parser::FFI::CssTokenType::CloseParen:
        case Web::CSS::Parser::FFI::CssTokenType::OpenCurly:
        case Web::CSS::Parser::FFI::CssTokenType::CloseCurly:
            return "delimiter"sv;
        case Web::CSS::Parser::FFI::CssTokenType::CDO:
        case Web::CSS::Parser::FFI::CssTokenType::CDC:
            return "comment"sv;
        case Web::CSS::Parser::FFI::CssTokenType::EndOfFile:
        default:
            break;
        }
        return ""sv;
    };

    auto class_for_js_token = [](u64 token_type) {
        auto category = JS::token_category_from_packed(token_type);
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

String SourceHighlighterClient::to_html_string(Optional<URL::URL> const& url, URL::URL const& base_url) const
{
    StringBuilder builder;

    auto append_escaped = [&](Utf8View const& text) {
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

    builder.append(R"~~~(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="color-scheme" content="dark light">)~~~"sv);

    if (url.has_value())
        builder.appendff("<title>View Source - {}</title>", escape_html_entities(url->serialize_for_display()));
    else
        builder.append("<title>View Source</title>"sv);

    builder.appendff("<style type=\"text/css\">{}</style>", HTML_HIGHLIGHTER_STYLE);
    builder.append(R"~~~(
</head>
<body>
<pre class=\"html\">)~~~"sv);

    // NB: Which attribute holds a URL on the start tag whose attributes we are drawing. End tags have no attributes
    //     that matter.
    enum class URLAttribute : u8 {
        None,
        Href,
        Src,
    };
    auto url_attribute_for_tag = [](StringView tag_name) {
        if (tag_name.is_one_of_ignoring_ascii_case("a"sv, "area"sv, "base"sv, "link"sv))
            return URLAttribute::Href;
        if (tag_name.is_one_of_ignoring_ascii_case("audio"sv, "embed"sv, "frame"sv, "iframe"sv, "img"sv, "input"sv, "script"sv, "source"sv, "track"sv, "video"sv))
            return URLAttribute::Src;
        return URLAttribute::None;
    };
    URLAttribute current_tag_url_attribute = URLAttribute::None;
    bool linkify_attribute = false;

    auto is_url_attribute = [&](StringView attribute_name) {
        switch (current_tag_url_attribute) {
        case URLAttribute::None:
            return false;
        case URLAttribute::Href:
            return attribute_name.equals_ignoring_ascii_case("href"sv);
        case URLAttribute::Src:
            return attribute_name.equals_ignoring_ascii_case("src"sv);
        }
        VERIFY_NOT_REACHED();
    };

    auto resolve_url_for_attribute = [&](Utf8View const& attribute_value) -> Optional<URL::URL> {
        if (!linkify_attribute)
            return {};

        // NB: Only a leading quote starts a quoted value. Quotes elsewhere in an unquoted value are part of the value.
        auto attribute_url = attribute_value.as_string();
        if (attribute_url.starts_with('"') || attribute_url.starts_with('\'')) {
            // NB: A quoted value that spans several lines reaches us one line at a time, so do not link any part of it.
            if (attribute_url.length() < 2 || attribute_url[attribute_url.length() - 1] != attribute_url[0]) {
                linkify_attribute = false;
                return {};
            }
            attribute_url = attribute_url.substring_view(1, attribute_url.length() - 2);
        }

        auto url = Web::DOMURL::parse(attribute_url, base_url);

        // NB: Following a javascript:, data: or blob: link could run the page's script inside the view-source document,
        //     so only link to schemes that load an ordinary resource.
        if (url.has_value() && !url->scheme().is_one_of("http"sv, "https"sv, "file"sv))
            return {};
        return url;
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

            auto text = line_view.unicode_substring_view(start, length);

            if (span.has_value()) {
                bool append_anchor_close = false;

                if (span->data == to_underlying(Web::HTML::AugmentedTokenKind::OpenTag)) {
                    current_tag_url_attribute = url_attribute_for_tag(text.as_string());
                    linkify_attribute = false;
                } else if (span->data == to_underlying(Web::HTML::AugmentedTokenKind::CloseTag)) {
                    current_tag_url_attribute = URLAttribute::None;
                    linkify_attribute = false;
                } else if (span->data == to_underlying(Web::HTML::AugmentedTokenKind::AttributeName)) {
                    linkify_attribute = is_url_attribute(text.as_string());
                } else if (span->data == to_underlying(Web::HTML::AugmentedTokenKind::AttributeValue)) {
                    if (auto href = resolve_url_for_attribute(text); href.has_value()) {
                        builder.appendff("<a href=\"{}\">", escape_html_entities(href->serialize()));
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

    builder.append(R"~~~(
</pre>
</body>
</html>
)~~~"sv);

    return builder.to_string_without_validation();
}

}
