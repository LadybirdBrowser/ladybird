/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/Parser.h>

namespace Web::CSS::Parser {

static void expect_decoded_bytes_start_with(Vector<u8> const& input, Optional<StringView> charset, StringView expected)
{
    auto decoded = MUST(css_decode_bytes({}, charset, input.span()));
    EXPECT(decoded.starts_with(Utf16String::from_utf8_without_validation(expected)));
}

TEST_CASE(decodes_valid_utf8_bytes_with_requested_single_byte_encoding)
{
    auto input = Vector<u8> { 0xc3, 0xa9, ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "windows-1252"sv, "Ã©"sv);
}

TEST_CASE(decodes_valid_utf8_bytes_with_requested_utf16_encoding)
{
    auto input = Vector<u8> { 'a', 0x00, ' ', 0x00, '{', 0x00, '}', 0x00 };
    expect_decoded_bytes_start_with(input, "utf-16le"sv, "a"sv);
}

TEST_CASE(defaults_to_utf8_without_charset)
{
    auto input = Vector<u8> { 0xc3, 0xa9, ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, {}, "é"sv);
}

TEST_CASE(strips_bom_for_utf8_encoding)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "utf-8"sv, "body"sv);
}

TEST_CASE(strips_bom_for_utf8_encoding_alias)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "utf8"sv, "body"sv);
}

TEST_CASE(bom_takes_precedence_over_non_utf8_charset)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "windows-1252"sv, "body"sv);
}

TEST_CASE(charset_rule_determines_fallback_encoding)
{
    Vector<u8> input;
    for (char c : "@charset \"windows-1252\";\xc3\xa9 {}"sv)
        input.append(static_cast<u8>(c));
    expect_decoded_bytes_start_with(input, {}, "@charset \"windows-1252\";Ã©"sv);
}

TEST_CASE(decodes_utf8_surrogate_bytes_as_three_replacements)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "utf-8"sv, "���body"sv);
}

TEST_CASE(decodes_invalid_utf8_second_byte_tail_as_replacements)
{
    auto input = Vector<u8> { 0xe0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "utf-8"sv, "��body"sv);
}

TEST_CASE(decodes_truncated_utf8_tail_as_single_replacement)
{
    auto input = Vector<u8> { 0xf0, 0x9f, 0x98 };
    expect_decoded_bytes_start_with(input, "utf-8"sv, "�"sv);
}

TEST_CASE(decodes_surrogate_shaped_bytes_with_requested_single_byte_encoding)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_decoded_bytes_start_with(input, "windows-1252"sv, "í €body"sv);
}

}
