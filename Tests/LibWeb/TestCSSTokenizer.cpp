/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>

namespace Web::CSS::Parser {

static ReadonlyBytes bytes(Vector<u8> const& data)
{
    return data.span();
}

static void expect_first_token_is_ident(Vector<Token> const& tokens, StringView expected_ident, StringView expected_source)
{
    EXPECT(!tokens.is_empty());
    auto const& token = tokens.first();
    EXPECT(token.is(Token::Type::Ident));
    EXPECT_EQ(token.ident(), FlyString::from_utf8_without_validation(expected_ident.bytes()));
    EXPECT_EQ(token.original_source_text(), expected_source);
}

static void expect_first_token_is_ident_for_both_tokenizers(StringView input, StringView encoding, StringView expected_ident, StringView expected_source, TokenizerInput tokenizer_input = TokenizerInput::EncodedBytes)
{
    expect_first_token_is_ident(Tokenizer::tokenize(input, encoding, tokenizer_input), expected_ident, expected_source);
    expect_first_token_is_ident(RustTokenizer::tokenize(input, encoding, tokenizer_input), expected_ident, expected_source);
}

TEST_CASE(tokenizer_decodes_valid_utf8_bytes_with_requested_single_byte_encoding)
{
    auto input = Vector<u8> { 0xc3, 0xa9, ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "windows-1252"sv, "Ã©"sv, "Ã©"sv);
}

TEST_CASE(tokenizer_decodes_valid_utf8_bytes_with_requested_utf16_encoding)
{
    auto input = Vector<u8> { 'a', 0x00, ' ', 0x00, '{', 0x00, '}', 0x00 };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf-16le"sv, "a"sv, "a"sv);
}

TEST_CASE(tokenizer_keeps_utf8_fast_path_for_utf8_encoding)
{
    expect_first_token_is_ident_for_both_tokenizers("é {}"sv, "utf-8"sv, "é"sv, "é"sv);
}

TEST_CASE(tokenizer_strips_bom_in_utf8_fast_path)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf-8"sv, "body"sv, "body"sv);
}

TEST_CASE(tokenizer_strips_bom_for_utf8_encoding_alias)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf8"sv, "body"sv, "body"sv);
}

TEST_CASE(tokenizer_does_not_strip_bom_bytes_for_non_utf8_encoding)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "windows-1252"sv, "ï»¿body"sv, "ï»¿body"sv);
}

TEST_CASE(tokenizer_decodes_utf8_surrogate_bytes_as_three_replacements)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf-8"sv, "���body"sv, "���body"sv);
}

TEST_CASE(tokenizer_decodes_utf8_surrogate_bytes_as_three_replacements_for_utf8_alias)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf8"sv, "���body"sv, "���body"sv);
}

TEST_CASE(tokenizer_strips_utf8_bom_before_decoding_surrogate_bytes)
{
    auto input = Vector<u8> { 0xef, 0xbb, 0xbf, 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf-8"sv, "���body"sv, "���body"sv);
}

TEST_CASE(tokenizer_decodes_invalid_utf8_second_byte_tail_as_replacements)
{
    auto input = Vector<u8> { 0xe0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf-8"sv, "��body"sv, "��body"sv);
}

TEST_CASE(tokenizer_decodes_truncated_utf8_tail_as_single_replacement)
{
    auto input = Vector<u8> { 0xf0, 0x9f, 0x98 };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "utf-8"sv, "�"sv, "�"sv);
}

TEST_CASE(tokenizer_decodes_surrogate_shaped_bytes_with_requested_single_byte_encoding)
{
    auto input = Vector<u8> { 0xed, 0xa0, 0x80, 'b', 'o', 'd', 'y', ' ', '{', '}' };
    expect_first_token_is_ident_for_both_tokenizers(StringView(bytes(input)), "windows-1252"sv, "í €body"sv, "í €body"sv);
}

TEST_CASE(tokenizer_filters_decoded_surrogate_code_points_as_single_replacements)
{
    expect_first_token_is_ident_for_both_tokenizers("foo\xed\xa0\x80"sv, "utf-8"sv, "foo�"sv, "foo�"sv, TokenizerInput::DecodedText);
}

}
