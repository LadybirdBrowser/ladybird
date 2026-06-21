/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Vector.h>
#include <LibJS/ParserError.h>
#include <LibJS/Token.h>

namespace JS {

Utf16String ParserError::to_utf16_string() const
{
    if (!position.has_value())
        return message;
    return Utf16String::formatted("{} (line: {}, column: {})", message, position.value().line, position.value().column);
}

Utf16String ParserError::source_location_hint(Utf16View const& source, char spacer, char indicator) const
{
    if (!position.has_value())
        return {};

    // We need to modify the source to match what the lexer considers one line - normalizing
    // line terminators to \n is easier than splitting using all different LT characters.
    auto source_string = source.replace("\r\n"sv, "\n"sv, ReplaceMode::All).replace("\r"sv, "\n"sv, ReplaceMode::All).replace(LINE_SEPARATOR, "\n"sv, ReplaceMode::All).replace(PARAGRAPH_SEPARATOR, "\n"sv, ReplaceMode::All);

    Utf16StringBuilder builder;
    builder.append(source_string.split_view('\n', SplitBehavior::KeepEmpty)[position.value().line - 1]);
    builder.append_ascii('\n');
    for (size_t i = 0; i < position.value().column - 1; ++i)
        builder.append_ascii(spacer);
    builder.append_ascii(indicator);
    return builder.to_string();
}

}
