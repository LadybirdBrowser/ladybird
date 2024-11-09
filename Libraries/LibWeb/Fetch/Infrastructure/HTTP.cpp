/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Fetch/Infrastructure/HTTP.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#collect-an-http-quoted-string
String collect_an_http_quoted_string(GenericLexer& lexer, HttpQuotedStringExtractValue extract_value)
{
    // To collect an HTTP quoted string from a string input, given a position variable position and optionally an extract-value flag, run these steps:
    // 1. Let positionStart be position.
    auto position_start = lexer.tell();

    // 2. Let value be the empty string.
    StringBuilder value;

    // 3. Assert: the code point at position within input is U+0022 (").
    VERIFY(lexer.peek() == '"');

    // 4. Advance position by 1.
    lexer.ignore(1);

    // 5. While true:
    while (true) {
        // 1. Append the result of collecting a sequence of code points that are not U+0022 (") or U+005C (\) from input, given position, to value.
        auto value_part = lexer.consume_until([](char ch) {
            return ch == '"' || ch == '\\';
        });

        value.append(value_part);

        // 2. If position is past the end of input, then break.
        if (lexer.is_eof())
            break;

        // 3. Let quoteOrBackslash be the code point at position within input.
        // 4. Advance position by 1.
        char quote_or_backslash = lexer.consume();

        // 5. If quoteOrBackslash is U+005C (\), then:
        if (quote_or_backslash == '\\') {
            // 1. If position is past the end of input, then append U+005C (\) to value and break.
            if (lexer.is_eof()) {
                value.append('\\');
                break;
            }

            // 2. Append the code point at position within input to value.
            // 3. Advance position by 1.
            value.append(lexer.consume());
        }

        // 6. Otherwise:
        else {
            // 1. Assert: quoteOrBackslash is U+0022 (").
            VERIFY(quote_or_backslash == '"');

            // 2. Break.
            break;
        }
    }

    // 6. If the extract-value flag is set, then return value.
    if (extract_value == HttpQuotedStringExtractValue::Yes)
        return MUST(value.to_string());

    // 7. Return the code points from positionStart to position, inclusive, within input.
    return MUST(String::from_utf8(lexer.input().substring_view(position_start, lexer.tell() - position_start)));
}

}
