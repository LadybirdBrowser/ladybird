/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWebView/SourceHighlighter.h>

namespace WebView {

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

String highlight_source(URL::URL const& url, StringView source)
{
    Web::HTML::HTMLTokenizer tokenizer { source, "utf-8"sv };
    StringBuilder builder;

    builder.append(R"~~~(
<!DOCTYPE html>
<html>
<head>
    <meta name="color-scheme" content="dark light">)~~~"sv);

    builder.appendff("<title>View Source - {}</title>", url);
    builder.appendff("<style type=\"text/css\">{}</style>", generate_style());
    builder.append(R"~~~(
</head>
<body>
<pre class="html">
<span class="line">)~~~"sv);

    size_t previous_position = 0;

    auto append_source = [&](auto end_position, Optional<StringView> const& class_name = {}) {
        if (end_position <= previous_position)
            return;

        auto segment = source.substring_view(previous_position, end_position - previous_position);

        if (class_name.has_value())
            builder.appendff("<span class=\"{}\">"sv, *class_name);

        for (auto code_point : Utf8View { segment }) {
            if (code_point == '&')
                builder.append("&amp;"sv);
            else if (code_point == 0xA0)
                builder.append("&nbsp;"sv);
            else if (code_point == '<')
                builder.append("&lt;"sv);
            else if (code_point == '>')
                builder.append("&gt;"sv);
            else if (code_point == '\n')
                builder.append("</span>\n<span class=\"line\">"sv);
            else
                builder.append_code_point(code_point);
        }

        if (class_name.has_value())
            builder.append("</span>"sv);

        previous_position = end_position;
    };

    for (auto token = tokenizer.next_token(); token.has_value(); token = tokenizer.next_token()) {
        if (token->is_comment()) {
            append_source(token->start_position().byte_offset);
            append_source(token->end_position().byte_offset, "comment"sv);
        } else if (token->is_start_tag() || token->is_end_tag()) {
            auto tag_name_start = token->start_position().byte_offset;

            append_source(tag_name_start);
            append_source(tag_name_start + token->tag_name().bytes().size(), "tag"sv);

            token->for_each_attribute([&](auto const& attribute) {
                append_source(attribute.name_start_position.byte_offset);
                append_source(attribute.name_end_position.byte_offset, "attribute-name"sv);

                append_source(attribute.value_start_position.byte_offset);
                append_source(attribute.value_end_position.byte_offset, "attribute-value"sv);

                return IterationDecision::Continue;
            });

            append_source(token->end_position().byte_offset);
        } else {
            append_source(token->end_position().byte_offset);

            if (token->is_end_of_file())
                break;
        }
    }

    builder.append(R"~~~(
</span>
</pre>
</body>
</html>
)~~~"sv);

    return MUST(builder.to_string());
}

}
