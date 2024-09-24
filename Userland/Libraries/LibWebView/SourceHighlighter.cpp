/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SyntaxHighlighter/SyntaxHighlighter.h>
#include <LibWebView/SourceHighlighter.h>

namespace WebView {

SourceDocument::SourceDocument(StringView source)
    : m_source(source)
{
    m_source.for_each_split_view('\n', AK::SplitBehavior::KeepEmpty, [&](auto line) {
        m_lines.append(Syntax::TextDocumentLine { *this, line });
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

SourceHighlighterClient::SourceHighlighterClient(StringView source, Syntax::Language language)
    : m_document(SourceDocument::create(source))
{
    // HACK: Syntax highlighters require a palette, but we don't actually care about the output styling, only the type of token for each span.
    //       Also, getting a palette from the chrome is nontrivial. So, create a dummy blank one and use that.
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
    auto palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
    Gfx::Palette dummy_palette { palette_impl };

    switch (language) {
    case Syntax::Language::HTML:
        m_highlighter = make<Web::HTML::SyntaxHighlighter>();
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

String highlight_source(URL::URL const& url, StringView source)
{
    SourceHighlighterClient highlighter_client { source, Syntax::Language::HTML };
    return highlighter_client.to_html_string(url);
}

StringView SourceHighlighterClient::class_for_token(u64 token_type) const
{
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
        break;
    }

    return "unknown"sv;
}

static String generate_style()
{
    StringBuilder builder;

    builder.append(HTML_HIGHLIGHTER_STYLE);
    builder.append(R"~~~(
    .html {
        counter-reset: line;
    }

    .line {
        counter-increment: line;
        white-space: nowrap;
    }

    .line::before {
        content: counter(line) " ";

        display: inline-block;
        width: 2.5em;

        padding-right: 0.5em;
        text-align: right;
    }

    @media (prefers-color-scheme: dark) {
        .line::before {
            color: darkgrey;
        }
    }

    @media (prefers-color-scheme: light) {
        .line::before {
            color: dimgray;
        }
    }
)~~~"sv);

    return MUST(builder.to_string());
}

String SourceHighlighterClient::to_html_string(URL::URL const& url) const
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

    builder.append(R"~~~(
<!DOCTYPE html>
<html>
<head>
    <meta name="color-scheme" content="dark light">)~~~"sv);

    builder.appendff("<title>View Source - {}</title>", escape_html_entities(MUST(url.to_string())));
    builder.appendff("<style type=\"text/css\">{}</style>", generate_style());
    builder.append(R"~~~(
</head>
<body>
<pre class="html">)~~~"sv);

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
                start_token(span->data);
                append_escaped(text);
                end_token();
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
