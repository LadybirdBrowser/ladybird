/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>

namespace Web::CSS::Parser {

static ReadonlyBytes bytes(Vector<u8> const& data)
{
    return data.span();
}

static void expect_normalized_input_starts_with(StringView input, StringView encoding, StringView expected, TokenizerInput tokenizer_input = TokenizerInput::EncodedBytes)
{
    auto normalized = RustTokenizer::normalize_input(input, encoding, tokenizer_input);
    EXPECT(normalized.starts_with(Utf16String::from_utf8_without_validation(expected)));
}

TEST_CASE(tokenizer_decodes_valid_utf8_bytes_with_requested_single_byte_encoding)
{
    auto input = Vector<u8> { 0xc3, 0xa9, ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "windows-1252"sv, "Ã©"sv);
}

TEST_CASE(tokenizer_decodes_valid_utf8_bytes_with_requested_utf16_encoding)
{
    auto input = Vector<u8> { 'a', 0x00, ' ', 0x00, '{', 0x00, '}', 0x00 };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf-16le"sv, "a"sv);
}

TEST_CASE(tokenizer_keeps_utf8_fast_path_for_utf8_encoding)
{
    expect_normalized_input_starts_with("é {}"sv, "utf-8"sv, "é"sv);
}

TEST_CASE(tokenizer_strips_bom_in_utf8_fast_path)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf-8"sv, "body"sv);
}

TEST_CASE(tokenizer_strips_bom_for_utf8_encoding_alias)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf8"sv, "body"sv);
}

TEST_CASE(tokenizer_does_not_strip_bom_bytes_for_non_utf8_encoding)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "windows-1252"sv, "ï»¿body"sv);
}

TEST_CASE(tokenizer_decodes_utf8_surrogate_bytes_as_three_replacements)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf-8"sv, "���body"sv);
}

TEST_CASE(tokenizer_decodes_utf8_surrogate_bytes_as_three_replacements_for_utf8_alias)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf8"sv, "���body"sv);
}

TEST_CASE(tokenizer_strips_utf8_bom_before_decoding_surrogate_bytes)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf-8"sv, "���body"sv);
}

TEST_CASE(tokenizer_decodes_invalid_utf8_second_byte_tail_as_replacements)
{
    auto input = Vector<u8> { 0xe0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf-8"sv, "��body"sv);
}

TEST_CASE(tokenizer_decodes_truncated_utf8_tail_as_single_replacement)
{
    auto input = Vector<u8> { 0xf0, 0x9f, 0x98 };
    expect_normalized_input_starts_with(StringView(bytes(input)), "utf-8"sv, "�"sv);
}

TEST_CASE(tokenizer_decodes_surrogate_shaped_bytes_with_requested_single_byte_encoding)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_normalized_input_starts_with(StringView(bytes(input)), "windows-1252"sv, "í €body"sv);
}

TEST_CASE(tokenizer_filters_decoded_surrogate_code_points_as_single_replacements)
{
    expect_normalized_input_starts_with("foo\xed\xa0\x80"sv, "utf-8"sv, "foo�"sv, TokenizerInput::DecodedText);
}

}
