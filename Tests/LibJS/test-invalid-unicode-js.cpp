/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Lexer.h>
#include <LibTest/TestCase.h>

static bool produces_eof_tokens(JS::Lexer& lexer)
{
    for (auto i = 0; i < 10; i++) {
        auto eof_token = lexer.next();
        if (eof_token.type() != JS::TokenType::Eof)
            return false;
    }
    return true;
}

static bool triggers_immediate_unicode_fault(StringView code)
{
    auto lexer = JS::Lexer(code);
    auto first_token = lexer.next();

    if (first_token.type() != JS::TokenType::Invalid)
        return false;

    return produces_eof_tokens(lexer);
}
// In the not leading character it must start with 0b10xxxxxx
// Thus all these options are invalid:
// \x0y = 0000 y (or \x1y, \x2y and \x3y)
// \x4y = 0100 y (or \x5y, \x6y and \x7y)
// \xCy = 1100 y (or \xDy, \xEy and \xFy)
// And the only valid option is:
// \x8y = 1000 y (or \x9y, \xAy

TEST_CASE(no_input_only_gives_eof)
{
    auto code = ""sv;
    auto lexer = JS::Lexer(code);
    EXPECT(produces_eof_tokens(lexer));
}

TEST_CASE(invalid_start_code_point)
{
    EXPECT(triggers_immediate_unicode_fault("\x80"sv));
    EXPECT(triggers_immediate_unicode_fault("\x90"sv));
    EXPECT(triggers_immediate_unicode_fault("\xA0"sv));
    EXPECT(triggers_immediate_unicode_fault("\xB0"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF8"sv));
    EXPECT(triggers_immediate_unicode_fault("\xFF"sv));
}

TEST_CASE(code_points_of_length_2)
{
    // Initial 110xxxxx -> \xCy or \xDy
    EXPECT(triggers_immediate_unicode_fault("\xC5"sv));
    EXPECT(triggers_immediate_unicode_fault("\xC5\x02"sv));
    EXPECT(triggers_immediate_unicode_fault("\xC5\x52"sv));
    EXPECT(triggers_immediate_unicode_fault("\xC5\xD2"sv));

    EXPECT(triggers_immediate_unicode_fault("\xD5"sv));
    EXPECT(triggers_immediate_unicode_fault("\xD5\x23"sv));
    EXPECT(triggers_immediate_unicode_fault("\xD5\x74"sv));
    EXPECT(triggers_immediate_unicode_fault("\xD5\xF5"sv));
}

TEST_CASE(code_points_of_length_3)
{
    // Initial 1110xxxx -> \xEy
    EXPECT(triggers_immediate_unicode_fault("\xE5"sv));
    EXPECT(triggers_immediate_unicode_fault("\xE5\x02"sv));
    EXPECT(triggers_immediate_unicode_fault("\xE5\x52"sv));
    EXPECT(triggers_immediate_unicode_fault("\xE5\xD2"sv));

    EXPECT(triggers_immediate_unicode_fault("\xEA\x80"sv));
    EXPECT(triggers_immediate_unicode_fault("\xEA\x81\x07"sv));
    EXPECT(triggers_immediate_unicode_fault("\xEA\x82\x57"sv));
    EXPECT(triggers_immediate_unicode_fault("\xEA\x83\xD7"sv));
}

TEST_CASE(code_points_of_length_4)
{
    // Initial 11110xxx -> \xF{0..7}
    EXPECT(triggers_immediate_unicode_fault("\xF0"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF1\x02"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF2\x52"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF3\xD2"sv));

    EXPECT(triggers_immediate_unicode_fault("\xF4\x80"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF5\x81\x07"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF6\x82\x57"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF7\x83\xD7"sv));

    EXPECT(triggers_immediate_unicode_fault("\xF4\x80\x80"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF5\x91\x80\x07"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF6\xA2\x80\x57"sv));
    EXPECT(triggers_immediate_unicode_fault("\xF7\xB3\x80\xD7"sv));
}

TEST_CASE(gives_valid_part_until_fault)
{
    auto code = "abc\xF5\x81\x80\x07; abc\xF5\x81\x80\x07 += 4"sv;
    JS::Lexer lexer(code);
    auto first_token = lexer.next();
    EXPECT_EQ(first_token.type(), JS::TokenType::Identifier);
    EXPECT_EQ(first_token.value(), "abc"sv);
    auto second_token = lexer.next();
    EXPECT_EQ(second_token.type(), JS::TokenType::Invalid);
    EXPECT(produces_eof_tokens(lexer));
}

TEST_CASE(gives_fully_parsed_tokens_even_if_invalid_unicode_follows)
{
    auto code = "let \xE5\xD2"sv;
    JS::Lexer lexer(code);
    auto first_token = lexer.next();
    EXPECT_EQ(first_token.type(), JS::TokenType::Let);
    auto second_token = lexer.next();
    EXPECT_EQ(second_token.type(), JS::TokenType::Invalid);
    EXPECT(produces_eof_tokens(lexer));
}

TEST_CASE(invalid_unicode_and_valid_code)
{
    EXPECT(triggers_immediate_unicode_fault("\xEA\xFDthrow 1;"sv));
}

TEST_CASE(long_invalid_unicode_and_valid_code)
{
    EXPECT(triggers_immediate_unicode_fault("\xF7throw 1;"sv));
}

TEST_CASE(invalid_unicode_after_valid_code_and_before_eof)
{
    auto code = "let \xEA\xFD;"sv;
    auto lexer = JS::Lexer(code);
    auto let_token = lexer.next();
    EXPECT_EQ(let_token.type(), JS::TokenType::Let);
    auto invalid_token = lexer.next();
    EXPECT_EQ(invalid_token.type(), JS::TokenType::Invalid);
    EXPECT(produces_eof_tokens(lexer));
}
