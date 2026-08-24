/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS {

// https://www.w3.org/TR/cssom-1/#escape-a-character
void escape_a_character(StringBuilder& builder, u32 character)
{
    builder.append('\\');
    builder.append_code_point(character);
}

static void escape_a_character(Utf16StringBuilder& builder, u32 character)
{
    builder.append_ascii('\\');
    builder.append_code_point(character);
}

// https://www.w3.org/TR/cssom-1/#escape-a-character-as-code-point
void escape_a_character_as_code_point(StringBuilder& builder, u32 character)
{
    builder.appendff("\\{:x} ", character);
}

static void escape_a_character_as_code_point(Utf16StringBuilder& builder, u32 character)
{
    builder.appendff("\\{:x} ", character);
}

// https://www.w3.org/TR/cssom-1/#serialize-an-identifier
template<typename Builder>
static void serialize_an_identifier_to_builder(Builder& builder, Utf16View ident)
{
    auto first_character = ident.is_empty() ? 0 : *ident.begin();
    size_t character_index = 0;

    // To serialize an identifier means to create a string represented by the concatenation of,
    // for each character of the identifier:
    for (auto character : ident) {
        // If the character is NULL (U+0000), then the REPLACEMENT CHARACTER (U+FFFD).
        if (character == 0) {
            builder.append_code_point(0xFFFD);
            ++character_index;
            continue;
        }
        // If the character is in the range [\1-\1f] (U+0001 to U+001F) or is U+007F,
        // then the character escaped as code point.
        if ((character >= 0x0001 && character <= 0x001F) || (character == 0x007F)) {
            escape_a_character_as_code_point(builder, character);
            ++character_index;
            continue;
        }
        // If the character is the first character and is in the range [0-9] (U+0030 to U+0039),
        // then the character escaped as code point.
        if (character_index == 0 && character >= '0' && character <= '9') {
            escape_a_character_as_code_point(builder, character);
            ++character_index;
            continue;
        }
        // If the character is the second character and is in the range [0-9] (U+0030 to U+0039)
        // and the first character is a "-" (U+002D), then the character escaped as code point.
        if (character_index == 1 && first_character == '-' && character >= '0' && character <= '9') {
            escape_a_character_as_code_point(builder, character);
            ++character_index;
            continue;
        }
        // If the character is the first character and is a "-" (U+002D), and there is no second
        // character, then the escaped character.
        if (character_index == 0 && character == '-' && ident.length_in_code_points() == 1) {
            escape_a_character(builder, character);
            ++character_index;
            continue;
        }
        // If the character is not handled by one of the above rules and is greater than or equal to U+0080, is "-" (U+002D) or "_" (U+005F), or is in one of the ranges [0-9] (U+0030 to U+0039), [A-Z] (U+0041 to U+005A), or \[a-z] (U+0061 to U+007A), then the character itself.
        if ((character >= 0x0080)
            || (character == '-') || (character == '_')
            || (character >= '0' && character <= '9')
            || (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')) {
            builder.append_code_point(character);
            ++character_index;
            continue;
        }
        // Otherwise, the escaped character.
        escape_a_character(builder, character);
        ++character_index;
    }
}

void serialize_an_identifier(StringBuilder& builder, Utf16View ident)
{
    serialize_an_identifier_to_builder(builder, ident);
}

void serialize_an_identifier(Utf16StringBuilder& builder, Utf16View ident)
{
    serialize_an_identifier_to_builder(builder, ident);
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

void serialize_a_string(StringBuilder& builder, Utf16View string)
{
    // To serialize a string means to create a string represented by '"' (U+0022), followed by the result
    // of applying the rules below to each character of the given string, followed by '"' (U+0022):
    builder.append('"');

    for (auto character : string) {
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

void serialize_a_string(Utf16StringBuilder& builder, Utf16View string)
{
    // To serialize a string means to create a string represented by '"' (U+0022), followed by the result
    // of applying the rules below to each character of the given string, followed by '"' (U+0022):
    builder.append_ascii('"');

    for (auto character : string) {
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

    builder.append_ascii('"');
}

// https://www.w3.org/TR/cssom-1/#serialize-a-url
void serialize_a_url(StringBuilder& builder, Utf16View url)
{
    // To serialize a URL means to create a string represented by "url(",
    // followed by the serialization of the URL as a string, followed by ")".
    builder.append("url("sv);
    serialize_a_string(builder, url);
    builder.append(')');
}

void serialize_a_url(Utf16StringBuilder& builder, Utf16View url)
{
    // To serialize a URL means to create a string represented by "url(",
    // followed by the serialization of the URL as a string, followed by ")".
    builder.append_ascii("url("sv);
    serialize_a_string(builder, url);
    builder.append_ascii(')');
}

// https://drafts.csswg.org/cssom/#serialize-a-css-value
void serialize_a_number(StringBuilder& builder, double value)
{
    // -> <number>
    //    A base-ten number using digits 0-9 (U+0030 to U+0039) in the shortest form possible, using "." to separate
    //    decimals (if any), rounding the value if necessary to not produce more than 6 decimals, preceded by "-"
    //    (U+002D) if it is negative.
    // NOTE: scientific notation is not used.

    // AD-HOC: If the number is small enough that it would not print any digits when rounded, serialize it as 0.
    if (AK::abs(value) < 0.0000005) {
        builder.append("0"sv);
        return;
    }

    // FIXME: Prevent scientific notation for large values.
    builder.appendff("{:.6}", value);
}

void serialize_a_number(Utf16StringBuilder& builder, double value)
{
    // -> <number>
    //    A base-ten number using digits 0-9 (U+0030 to U+0039) in the shortest form possible, using "." to separate
    //    decimals (if any), rounding the value if necessary to not produce more than 6 decimals, preceded by "-"
    //    (U+002D) if it is negative.
    // NOTE: scientific notation is not used.

    // AD-HOC: If the number is small enough that it would not print any digits when rounded, serialize it as 0.
    if (AK::abs(value) < 0.0000005) {
        builder.append_ascii('0');
        return;
    }

    // FIXME: Prevent scientific notation for large values.
    builder.appendff("{:.6}", value);
}

String serialize_an_identifier(Utf16View ident)
{
    StringBuilder builder;
    serialize_an_identifier(builder, ident);
    return builder.to_string_without_validation();
}

Utf16String serialize_an_identifier_to_utf16(Utf16View ident)
{
    Utf16StringBuilder builder;
    serialize_an_identifier(builder, ident);
    return builder.to_string();
}

String serialize_a_string(Utf16View string)
{
    StringBuilder builder;
    serialize_a_string(builder, string);
    return builder.to_string_without_validation();
}

String serialize_a_url(Utf16View url)
{
    StringBuilder builder;
    serialize_a_url(builder, url);
    return builder.to_string_without_validation();
}

String serialize_a_number(double value)
{
    StringBuilder builder;
    serialize_a_number(builder, value);
    return builder.to_string_without_validation();
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration
Utf16String serialize_a_css_declaration_to_utf16(Utf16View property, Utf16View value, Important important)
{
    // 1. Let s be the empty string.
    Utf16StringBuilder builder;

    // 2. Append property to s.
    // AD-HOC: There's no place currently on the spec where the property name properly escaped,
    //         and this needs to be done when custom properties have special characters.
    //         Related spec issues:
    //          - https://github.com/w3c/csswg-drafts/issues/11729
    //          - https://github.com/w3c/csswg-drafts/issues/12258
    serialize_an_identifier(builder, property);

    // 3. Append ": " (U+003A U+0020) to s.
    builder.append_ascii(": "sv);

    // 4. If value contains any non-whitespace characters, append value to s.
    if (!value.is_ascii_whitespace())
        builder.append(value);

    // 5. If the important flag is set, append " !important" (U+0020 U+0021 U+0069 U+006D U+0070 U+006F U+0072 U+0074
    //    U+0061 U+006E U+0074) to s.
    if (important == Important::Yes)
        builder.append_ascii(" !important"sv);

    // 6. Append ";" (U+003B) to s.
    builder.append_ascii(';');

    // 7. Return s.
    return builder.to_string();
}

// https://drafts.csswg.org/cssom/#serialize-a-css-declaration
Utf16String serialize_a_css_declaration_to_utf16(StringView property, Utf16View value, Important important)
{
    // 1. Let s be the empty string.
    Utf16StringBuilder builder;

    // 2. Append property to s.
    // AD-HOC: There's no place currently on the spec where the property name properly escaped,
    //         and this needs to be done when custom properties have special characters.
    //         Related spec issues:
    //          - https://github.com/w3c/csswg-drafts/issues/11729
    //          - https://github.com/w3c/csswg-drafts/issues/12258
    serialize_an_identifier(builder, property);

    // 3. Append ": " (U+003A U+0020) to s.
    builder.append_ascii(": "sv);

    // 4. If value contains any non-whitespace characters, append value to s.
    if (!value.is_ascii_whitespace())
        builder.append(value);

    // 5. If the important flag is set, append " !important" (U+0020 U+0021 U+0069 U+006D U+0070 U+006F U+0072 U+0074
    //    U+0061 U+006E U+0074) to s.
    if (important == Important::Yes)
        builder.append_ascii(" !important"sv);

    // 6. Append ";" (U+003B) to s.
    builder.append_ascii(';');

    // 7. Return s.
    return builder.to_string();
}

String serialize_a_positional_value_list(ReadonlySpan<ValueComparingNonnullRefPtr<StyleValue const>> values, SerializationMode mode)
{
    switch (values.size()) {
    case 2: {
        auto first_property_serialized = values[0]->to_string(mode);
        auto second_property_serialized = values[1]->to_string(mode);

        if (first_property_serialized == second_property_serialized)
            return first_property_serialized;

        return MUST(String::formatted("{} {}", first_property_serialized, second_property_serialized));
    }
    case 4: {
        auto first_property_serialized = values[0]->to_string(mode);
        auto second_property_serialized = values[1]->to_string(mode);
        auto third_property_serialized = values[2]->to_string(mode);
        auto fourth_property_serialized = values[3]->to_string(mode);

        if (first_is_equal_to_all_of(first_property_serialized, second_property_serialized, third_property_serialized, fourth_property_serialized))
            return first_property_serialized;

        if (first_property_serialized == third_property_serialized && second_property_serialized == fourth_property_serialized)
            return MUST(String::formatted("{} {}", first_property_serialized, second_property_serialized));

        if (second_property_serialized == fourth_property_serialized)
            return MUST(String::formatted("{} {} {}", first_property_serialized, second_property_serialized, third_property_serialized));

        return MUST(String::formatted("{} {} {} {}", first_property_serialized, second_property_serialized, third_property_serialized, fourth_property_serialized));
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

}
