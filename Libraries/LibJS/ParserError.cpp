/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibJS/ParserError.h>
#include <LibJS/Token.h>

namespace JS {

String ParserError::to_string() const
{
    if (!position.has_value())
        return message;
    return MUST(String::formatted("{} (line: {}, column: {})", message, position.value().line, position.value().column));
}

ByteString ParserError::to_byte_string() const
{
    if (!position.has_value())
        return message.to_byte_string();
    return ByteString::formatted("{} (line: {}, column: {})", message, position.value().line, position.value().column);
}

ByteString ParserError::source_location_hint(StringView source, char const spacer, char const indicator) const
{
    if (!position.has_value())
        return {};
    // We need to modify the source to match what the lexer considers one line - normalizing
    // line terminators to \n is easier than splitting using all different LT characters.
    ByteString source_string = source.replace("\r\n"sv, "\n"sv, ReplaceMode::All).replace("\r"sv, "\n"sv, ReplaceMode::All).replace(LINE_SEPARATOR_STRING, "\n"sv, ReplaceMode::All).replace(PARAGRAPH_SEPARATOR_STRING, "\n"sv, ReplaceMode::All);
    StringBuilder builder;
    builder.append(source_string.split_view('\n', SplitBehavior::KeepEmpty)[position.value().line - 1]);
    builder.append('\n');
    for (size_t i = 0; i < position.value().column - 1; ++i)
        builder.append(spacer);
    builder.append(indicator);
    return builder.to_byte_string();
}

}
