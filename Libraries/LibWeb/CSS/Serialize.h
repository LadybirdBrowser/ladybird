/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibWeb/CSS/StyleProperty.h>

namespace Web::CSS {

void escape_a_character(StringBuilder&, u32 character);
void escape_a_character_as_code_point(StringBuilder&, u32 character);
void serialize_an_identifier(StringBuilder&, StringView ident);
void serialize_a_string(StringBuilder&, StringView string);
void serialize_a_url(StringBuilder&, StringView url);
void serialize_unicode_ranges(StringBuilder&, Vector<Gfx::UnicodeRange> const& unicode_ranges);
void serialize_a_srgb_value(StringBuilder&, Color color);

String serialize_an_identifier(StringView ident);
String serialize_a_string(StringView string);
String serialize_a_url(StringView url);
String serialize_a_srgb_value(Color color);

// https://www.w3.org/TR/cssom/#serialize-a-comma-separated-list
template<typename T, typename SerializeItem>
void serialize_a_comma_separated_list(StringBuilder& builder, Vector<T> const& items, SerializeItem serialize_item)
{
    // To serialize a comma-separated list concatenate all items of the list in list order
    // while separating them by ", ", i.e., COMMA (U+002C) followed by a single SPACE (U+0020).
    for (size_t i = 0; i < items.size(); i++) {
        auto& item = items.at(i);
        serialize_item(builder, item);
        if ((i + 1) < items.size()) {
            builder.append(", "sv);
        }
    }
}

String serialize_a_css_declaration(StringView property, StringView value, Important = Important::No);

enum class InsertWhitespace : u8 {
    No,
    Yes,
};
// FIXME: Remove InsertWhitespace param once style value parsing stops discarding whitespace tokens.
String serialize_a_series_of_component_values(ReadonlySpan<Parser::ComponentValue>, InsertWhitespace = InsertWhitespace::No);

}
