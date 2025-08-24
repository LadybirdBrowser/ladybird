/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#escape-a-character
void escape_a_character(StringBuilder& builder, u32 character)
{
    builder.append('\\');
    builder.append_code_point(character);
}

// https://www.w3.org/TR/cssom-1/#escape-a-character-as-code-point
void escape_a_character_as_code_point(StringBuilder& builder, u32 character)
{
    builder.appendff("\\{:x} ", character);
}

// https://www.w3.org/TR/cssom-1/#serialize-an-identifier
void serialize_an_identifier(StringBuilder& builder, StringView ident)
{
    Utf8View characters { ident };
    auto first_character = characters.is_empty() ? 0 : *characters.begin();

    // To serialize an identifier means to create a string represented by the concatenation of,
    // for each character of the identifier:
    for (auto character : characters) {
        // If the character is NULL (U+0000), then the REPLACEMENT CHARACTER (U+FFFD).
        if (character == 0) {
            builder.append_code_point(0xFFFD);
            continue;
        }
        // If the character is in the range [\1-\1f] (U+0001 to U+001F) or is U+007F,
        // then the character escaped as code point.
        if ((character >= 0x0001 && character <= 0x001F) || (character == 0x007F)) {
            escape_a_character_as_code_point(builder, character);
            continue;
        }
        // If the character is the first character and is in the range [0-9] (U+0030 to U+0039),
        // then the character escaped as code point.
        if (builder.is_empty() && character >= '0' && character <= '9') {
            escape_a_character_as_code_point(builder, character);
            continue;
        }
        // If the character is the second character and is in the range [0-9] (U+0030 to U+0039)
        // and the first character is a "-" (U+002D), then the character escaped as code point.
        if (builder.length() == 1 && first_character == '-' && character >= '0' && character <= '9') {
            escape_a_character_as_code_point(builder, character);
            continue;
        }
        // If the character is the first character and is a "-" (U+002D), and there is no second
        // character, then the escaped character.
        if (builder.is_empty() && character == '-' && characters.length() == 1) {
            escape_a_character(builder, character);
            continue;
        }
        // If the character is not handled by one of the above rules and is greater than or equal to U+0080, is "-" (U+002D) or "_" (U+005F), or is in one of the ranges [0-9] (U+0030 to U+0039), [A-Z] (U+0041 to U+005A), or \[a-z] (U+0061 to U+007A), then the character itself.
        if ((character >= 0x0080)
            || (character == '-') || (character == '_')
            || (character >= '0' && character <= '9')
            || (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')) {
            builder.append_code_point(character);
            continue;
        }
        // Otherwise, the escaped character.
        escape_a_character(builder, character);
    }
}

// https://www.w3.org/TR/cssom-1/#serialize-a-string
void serialize_a_string(StringBuilder& builder, StringView string)
{
    Utf8View characters { string };

    // To serialize a string means to create a string represented by '"' (U+0022), followed by the result
    // of applying the rules below to each character of the given string, followed by '"' (U+0022):
    builder.append('"');

    for (auto character : characters) {
        // If the character is NULL (U+0000), then the REPLACEMENT CHARACTER (U+FFFD).
        if (character == 0) {
            builder.append_code_point(0xFFFD);
            continue;
        }
        // If the character is in the range [\1-\1f] (U+0001 to U+001F) or is U+007F, the character escaped as code point.
        if ((character >= 0x0001 && character <= 0x001F) || (character == 0x007F)) {
            escape_a_character_as_code_point(builder, character);
            continue;
        }
        // If the character is '"' (U+0022) or "\" (U+005C), the escaped character.
        if (character == 0x0022 || character == 0x005C) {
            escape_a_character(builder, character);
            continue;
        }
        // Otherwise, the character itself.
        builder.append_code_point(character);
    }

    builder.append('"');
}

// https://www.w3.org/TR/cssom-1/#serialize-a-url
void serialize_a_url(StringBuilder& builder, StringView url)
{
    // To serialize a URL means to create a string represented by "url(",
    // followed by the serialization of the URL as a string, followed by ")".
    builder.append("url("sv);
    serialize_a_string(builder, url);
    builder.append(')');
}

// NOTE: No spec currently exists for serializing a <'unicode-range'>.
void serialize_unicode_ranges(StringBuilder& builder, Vector<Gfx::UnicodeRange> const& unicode_ranges)
{
    serialize_a_comma_separated_list(builder, unicode_ranges, [](auto& builder, Gfx::UnicodeRange unicode_range) -> void {
        return serialize_a_string(builder, unicode_range.to_string());
    });
}

namespace {

char nth_digit(u32 value, u8 digit)
{
    // This helper is used to format integers.
    // nth_digit(745, 1) -> '5'
    // nth_digit(745, 2) -> '4'
    // nth_digit(745, 3) -> '7'

    VERIFY(value < 1000);
    VERIFY(digit <= 3);
    VERIFY(digit > 0);

    while (digit > 1) {
        value /= 10;
        digit--;
    }

    return '0' + value % 10;
}

Array<char, 4> format_to_8bit_compatible(u8 value)
{
    // This function formats to the shortest string that roundtrips at 8 bits.
    // As an example:
    //      127 / 255 = 0.498 ± 0.001
    //      128 / 255 = 0.502 ± 0.001
    // But round(.5 * 255) == 128, so this function returns (note that it's only the fractional part):
    //      127 -> "498"
    //      128 -> "5"

    u32 const three_digits = (value * 1000u + 127) / 255;
    u32 const rounded_to_two_digits = (three_digits + 5) / 10 * 10;

    if ((rounded_to_two_digits * 255 / 100 + 5) / 10 != value)
        return { nth_digit(three_digits, 3), nth_digit(three_digits, 2), nth_digit(three_digits, 1), '\0' };

    u32 const rounded_to_one_digit = (three_digits + 50) / 100 * 100;
    if ((rounded_to_one_digit * 255 / 100 + 5) / 10 != value)
        return { nth_digit(rounded_to_two_digits, 3), nth_digit(rounded_to_two_digits, 2), '\0', '\0' };

    return { nth_digit(rounded_to_one_digit, 3), '\0', '\0', '\0' };
}

}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
void serialize_a_srgb_value(StringBuilder& builder, Color color)
{
    // The serialized form is derived from the computed value and thus, uses either the rgb() or rgba() form
    // (depending on whether the alpha is exactly 1, or not), with lowercase letters for the function name.
    // NOTE: Since we use Gfx::Color, having an "alpha of 1" means its value is 255.
    if (color.alpha() == 0)
        builder.appendff("rgba({}, {}, {}, 0)", color.red(), color.green(), color.blue());
    else if (color.alpha() == 255)
        builder.appendff("rgb({}, {}, {})", color.red(), color.green(), color.blue());
    else
        builder.appendff("rgba({}, {}, {}, 0.{})", color.red(), color.green(), color.blue(), format_to_8bit_compatible(color.alpha()).data());
}

// https://drafts.csswg.org/cssom/#serialize-a-css-value
void serialize_a_number(StringBuilder& builder, double value)
{
    // -> <number>
    // A base-ten number using digits 0-9 (U+0030 to U+0039) in the shortest form possible, using "." to separate
    // decimals (if any), rounding the value if necessary to not produce more than 6 decimals, preceded by "-" (U+002D)
    // if it is negative.
    builder.appendff("{:.6}", value);
}

String serialize_an_identifier(StringView ident)
{
    StringBuilder builder;
    serialize_an_identifier(builder, ident);
    return builder.to_string_without_validation();
}

String serialize_a_string(StringView string)
{
    StringBuilder builder;
    serialize_a_string(builder, string);
    return builder.to_string_without_validation();
}

String serialize_a_url(StringView url)
{
    StringBuilder builder;
    serialize_a_url(builder, url);
    return builder.to_string_without_validation();
}

String serialize_a_srgb_value(Color color)
{
    StringBuilder builder;
    serialize_a_srgb_value(builder, color);
    return builder.to_string_without_validation();
}

String serialize_a_number(double value)
{
    StringBuilder builder;
    serialize_a_number(builder, value);
    return builder.to_string_without_validation();
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration
String serialize_a_css_declaration(StringView property, StringView value, Important important)
{
    // 1. Let s be the empty string.
    StringBuilder builder;

    // 2. Append property to s.
    // AD-HOC: There's no place currently on the spec where the property name properly escaped,
    //         and this needs to be done when custom properties have special characters.
    //         Related spec issues:
    //          - https://github.com/w3c/csswg-drafts/issues/11729
    //          - https://github.com/w3c/csswg-drafts/issues/12258
    serialize_an_identifier(builder, property);

    // 3. Append ": " (U+003A U+0020) to s.
    builder.append(": "sv);

    // 4. If value contains any non-whitespace characters, append value to s.
    if (!value.is_whitespace())
        builder.append(value);

    // 5. If the important flag is set, append " !important" (U+0020 U+0021 U+0069 U+006D U+0070 U+006F U+0072 U+0074
    //    U+0061 U+006E U+0074) to s.
    if (important == Important::Yes)
        builder.append(" !important"sv);

    // 6. Append ";" (U+003B) to s.
    builder.append(';');

    // 7. Return s.
    return builder.to_string_without_validation();
}

// https://drafts.csswg.org/css-syntax/#serialization
String serialize_a_series_of_component_values(ReadonlySpan<Parser::ComponentValue> component_values, InsertWhitespace insert_whitespace)
{
    // FIXME: There are special rules here where we should insert a comment between certain tokens. Do that!
    if (insert_whitespace == InsertWhitespace::Yes)
        return MUST(String::join(' ', component_values));
    return MUST(String::join(""sv, component_values));
}

}
