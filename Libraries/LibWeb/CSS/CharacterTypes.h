/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CharacterTypes.h>
#include <AK/Types.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-syntax-3/#digit
constexpr bool is_digit(u32 code_point)
{
    // A code point between U+0030 DIGIT ZERO (0) and U+0039 DIGIT NINE (9) inclusive.
    return code_point >= 0x30 && code_point <= 0x39;
}

// https://www.w3.org/TR/css-syntax-3/#hex-digit
constexpr bool is_hex_digit(u32 code_point)
{
    // A digit,
    // or a code point between U+0041 LATIN CAPITAL LETTER A (A) and U+0046 LATIN CAPITAL LETTER F (F) inclusive,
    // or a code point between U+0061 LATIN SMALL LETTER A (a) and U+0066 LATIN SMALL LETTER F (f) inclusive.
    return is_digit(code_point) || (code_point >= 0x41 && code_point <= 0x46) || (code_point >= 0x61 && code_point <= 0x66);
}

// https://www.w3.org/TR/css-syntax-3/#ident-start-code-point
constexpr bool is_ident_start_code_point(u32 code_point)
{
    // A letter, a non-ASCII code point, or U+005F LOW LINE (_).
    // Note: the is_unicode condition is used to reject the Tokenizer's EOF codepoint.
    return is_ascii_alpha(code_point) || (!is_ascii(code_point) && is_unicode(code_point)) || code_point == '_';
}

// https://www.w3.org/TR/css-syntax-3/#ident-code-point
constexpr bool is_ident_code_point(u32 code_point)
{
    // An ident-start code point, a digit, or U+002D HYPHEN-MINUS (-).
    return is_ident_start_code_point(code_point) || is_ascii_digit(code_point) || code_point == '-';
}

// https://www.w3.org/TR/css-syntax-3/#non-printable-code-point
constexpr bool is_non_printable_code_point(u32 code_point)
{
    return code_point <= 0x8 || code_point == 0xB || (code_point >= 0xE && code_point <= 0x1F) || code_point == 0x7F;
}

// https://www.w3.org/TR/css-syntax-3/#newline
constexpr inline bool is_newline(u32 code_point)
{
    // U+000A LINE FEED.
    // Note that U+000D CARRIAGE RETURN and U+000C FORM FEED are not included in this definition,
    // as they are converted to U+000A LINE FEED during preprocessing.
    return code_point == 0xA;
}

// https://www.w3.org/TR/css-syntax-3/#whitespace
constexpr bool is_whitespace(u32 code_point)
{
    // A newline, U+0009 CHARACTER TABULATION, or U+0020 SPACE.
    return is_newline(code_point) || code_point == '\t' || code_point == ' ';
}

// https://www.w3.org/TR/css-syntax-3/#maximum-allowed-code-point
constexpr bool is_greater_than_maximum_allowed_code_point(u32 code_point)
{
    // The greatest code point defined by Unicode: U+10FFFF.
    return code_point > 0x10FFFF;
}
}
