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
    builder.appendff("<style type=\"text/css\">{}</style>", HTML_HIGHLIGHTER_STYLE);
    builder.append(R"~~~(
</head>
<body>
<pre class="html">
)~~~"sv);

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
</pre>
</body>
</html>
)~~~"sv);

    return MUST(builder.to_string());
}

}
