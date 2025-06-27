/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/StringView.h>
#include <AK/Utf8GenericLexer.h>
#include <AK/Utf8View.h>

using namespace AK;

TEST_CASE(should_construct_from_empty_utf8_view)
{
    Utf8GenericLexer const sut(Utf8View(""sv));
    EXPECT(sut.is_eof());
}

TEST_CASE(should_construct_from_utf8_string_literal)
{
    Utf8GenericLexer const sut(u8"Hello, 方でぱん!");
    EXPECT(!sut.is_eof());
    EXPECT_EQ(sut.peek(), U'H');
}

TEST_CASE(should_construct_from_string_view)
{
    Utf8GenericLexer const sut("Hello, 世界! 🌍"sv);
    EXPECT(!sut.is_eof());
    EXPECT_EQ(sut.peek(), U'H');
}

TEST_CASE(should_construct_from_utf8_view)
{
    Utf8View const view("Hello, 世界! 🌍"sv);
    Utf8GenericLexer const sut(view);
    EXPECT(!sut.is_eof());
    EXPECT_EQ(sut.peek(), U'H');
}

TEST_CASE(should_tell_byte_position)
{
    Utf8GenericLexer sut(u8"Hello, 世界!");
    EXPECT_EQ(sut.tell(), 0u);

    sut.consume(); // H
    EXPECT_EQ(sut.tell(), 1u);

    sut.consume(); // e
    EXPECT_EQ(sut.tell(), 2u);

    // Skip to Non-Ascii characters
    while (sut.peek() != U'世' && !sut.is_eof()) {
        sut.consume();
    }

    auto const pos_before_non_ascii = sut.tell();
    sut.consume(); // 世 (3 bytes in UTF-8)
    EXPECT_EQ(sut.tell(), pos_before_non_ascii + 3);
}

TEST_CASE(should_tell_remaining_bytes)
{
    constexpr char8_t input[] = u8"Hello, 世界!";
    Utf8GenericLexer sut(input);

    auto initial_remaining = sut.tell_remaining();
    EXPECT_EQ(initial_remaining, sut.input().byte_length());

    sut.consume(); // H
    EXPECT_EQ(sut.tell_remaining(), initial_remaining - 1);
}

TEST_CASE(should_peek_code_points)
{
    Utf8GenericLexer sut(u8"Hello, 世界! 🌍");

    EXPECT_EQ(sut.peek(), U'H');
    EXPECT_EQ(sut.peek(1), U'e');
    EXPECT_EQ(sut.peek(2), U'l');

    // Skip to Non-Ascii characters
    while (sut.peek() != U'世' && !sut.is_eof()) {
        sut.consume();
    }

    EXPECT_EQ(sut.peek(), U'世');
    EXPECT_EQ(sut.peek(1), U'界');

    // Test peeking beyond EOF
    EXPECT_EQ(sut.peek(100), 0u);
}

TEST_CASE(should_peek_string)
{
    Utf8GenericLexer sut(u8"Hello, 世界! 🌍");

    auto hello = sut.peek_string(5);
    EXPECT(hello.has_value());
    EXPECT_EQ(hello->as_string(), "Hello");

    // Skip to Non-Ascii part
    while (sut.peek() != U'世' && !sut.is_eof()) {
        sut.consume();
    }

    auto non_ascii = sut.peek_string(2);
    EXPECT(non_ascii.has_value());
    EXPECT_EQ(non_ascii->as_string(), "世界");

    // Test peeking beyond EOF
    auto const beyond = sut.peek_string(100);
    EXPECT(!beyond.has_value());
}

// TEST_CASE(should_next_is_code_point)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!");
//
//     EXPECT(sut.next_is(U'H'));
//     EXPECT(!sut.next_is(U'e'));
//
//     // Skip to Non-Ascii characters
//     while (sut.peek() != U'世' && !sut.is_eof()) {
//         sut.consume();
//     }
//
//     EXPECT(sut.next_is(U'世'));
//     EXPECT(!sut.next_is(U'界'));
// }

// TEST_CASE(should_next_is_utf8_view)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界! 🌍"_utf8);
//
//     EXPECT(sut.next_is(u8"Hello"_utf8.as_utf8_view()));
//     EXPECT(!sut.next_is(u8"hello"_utf8.as_utf8_view()));
//
//     // Skip to Non-Ascii part
//     while (sut.peek() != U'世' && !sut.is_eof()) {
//         sut.consume();
//     }
//
//     EXPECT(sut.next_is(u8"世界"_utf8.as_utf8_view()));
//     EXPECT(!sut.next_is(u8"界世"_utf8.as_utf8_view()));
// }
//
// TEST_CASE(should_next_is_string_view)
// {
//     Utf8GenericLexer sut(StringView("Hello, 世界! 🌍"));
//
//     EXPECT(sut.next_is(StringView("Hello")));
//     EXPECT(!sut.next_is(StringView("hello")));
// }
//
// TEST_CASE(should_retreat_single_code_point)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     auto first = sut.consume(); // H
//     EXPECT_EQ(first, U'H');
//     EXPECT_EQ(sut.peek(), U'e');
//
//     sut.retreat();
//     EXPECT_EQ(sut.peek(), U'H');
// }
//
// TEST_CASE(should_retreat_multiple_code_points)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     // Consume "Hello"
//     for (int i = 0; i < 5; ++i) {
//         sut.consume();
//     }
//     EXPECT_EQ(sut.peek(), U',');
//
//     sut.retreat(3); // Back to 'l'
//     EXPECT_EQ(sut.peek(), U'l');
// }
//
// TEST_CASE(should_retreat_over_multibyte_characters)
// {
//     Utf8GenericLexer sut(u8"A世界B"_utf8);
//
//     sut.consume(); // A
//     sut.consume(); // 世
//     sut.consume(); // 界
//     EXPECT_EQ(sut.peek(), U'B');
//
//     sut.retreat(); // Back to 界
//     EXPECT_EQ(sut.peek(), U'界');
//
//     sut.retreat(); // Back to 世
//     EXPECT_EQ(sut.peek(), U'世');
//
//     sut.retreat(); // Back to A
//     EXPECT_EQ(sut.peek(), U'A');
// }
//
// TEST_CASE(should_consume_single_code_point)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     auto ch = sut.consume();
//     EXPECT_EQ(ch, U'H');
//     EXPECT_EQ(sut.peek(), U'e');
// }
//
// TEST_CASE(should_consume_multibyte_characters)
// {
//     Utf8GenericLexer sut(u8"世界🌍"_utf8);
//
//     auto ch1 = sut.consume(); // 世
//     EXPECT_EQ(ch1, U'世');
//
//     auto ch2 = sut.consume(); // 界
//     EXPECT_EQ(ch2, U'界');
//
//     auto ch3 = sut.consume(); // 🌍 (emoji)
//     EXPECT_EQ(ch3, 0x1F30D); // Earth globe emoji
// }
//
// TEST_CASE(should_consume_specific_code_point)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     EXPECT(sut.consume_specific(U'H'));
//     EXPECT_EQ(sut.peek(), U'e');
//
//     EXPECT(!sut.consume_specific(U'x'));
//     EXPECT_EQ(sut.peek(), U'e');
//
//     // Skip to Non-Ascii part
//     while (sut.peek() != U'世' && !sut.is_eof()) {
//         sut.consume();
//     }
//
//     EXPECT(sut.consume_specific(U'世'));
//     EXPECT_EQ(sut.peek(), U'界');
// }
//
// TEST_CASE(should_consume_specific_utf8_string_literal)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     EXPECT(sut.consume_specific(u8"Hello"_utf8));
//     EXPECT_EQ(sut.peek(), U',');
//
//     EXPECT(!sut.consume_specific(u8"world"_utf8));
//     EXPECT_EQ(sut.peek(), U',');
//
//     sut.consume(); // comma
//     sut.consume(); // space
//
//     EXPECT(sut.consume_specific(u8"世界"_utf8));
//     EXPECT_EQ(sut.peek(), U'!');
// }
//
// TEST_CASE(should_consume_specific_utf8_view)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     EXPECT(sut.consume_specific(Utf8View("Hello")));
//     EXPECT_EQ(sut.peek(), U',');
// }
//
// TEST_CASE(should_consume_specific_string)
// {
//     Utf8GenericLexer sut(StringView("Hello, 世界!"));
//
//     auto hello_string = String::from_utf8("Hello"sv).release_value();
//     EXPECT(sut.consume_specific(hello_string));
//     EXPECT_EQ(sut.peek(), U',');
// }
//
// TEST_CASE(should_consume_escaped_character)
// {
//     Utf8GenericLexer sut(u8"a\\nb\\tc\\\\d"_utf8);
//
//     auto ch1 = sut.consume_escaped_character();
//     EXPECT_EQ(ch1, U'a');
//
//     auto ch2 = sut.consume_escaped_character();
//     EXPECT_EQ(ch2, U'\n'); // \n becomes newline
//
//     auto ch3 = sut.consume_escaped_character();
//     EXPECT_EQ(ch3, U'b');
//
//     auto ch4 = sut.consume_escaped_character();
//     EXPECT_EQ(ch4, U'\t'); // \t becomes tab
//
//     auto ch5 = sut.consume_escaped_character();
//     EXPECT_EQ(ch5, U'c');
//
//     auto ch6 = sut.consume_escaped_character();
//     EXPECT_EQ(ch6, U'\\'); // \\ becomes backslash
//
//     auto ch7 = sut.consume_escaped_character();
//     EXPECT_EQ(ch7, U'd');
// }
//
// TEST_CASE(should_consume_count)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     auto result = sut.consume(5);
//     EXPECT_EQ(result.as_string(), "Hello");
//     EXPECT_EQ(sut.peek(), U',');
// }
//
// TEST_CASE(should_consume_all)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     // Consume first few characters
//     sut.consume(7); // "Hello, "
//
//     auto rest = sut.consume_all();
//     EXPECT_EQ(rest.as_string(), "世界!");
//     EXPECT(sut.is_eof());
// }
//
// TEST_CASE(should_consume_line)
// {
//     Utf8GenericLexer sut(u8"First line 世界\nSecond line\r\nThird line"_utf8);
//
//     auto line1 = sut.consume_line();
//     EXPECT_EQ(line1.as_string(), "First line 世界");
//
//     auto line2 = sut.consume_line();
//     EXPECT_EQ(line2.as_string(), "Second line");
//
//     auto line3 = sut.consume_line();
//     EXPECT_EQ(line3.as_string(), "Third line");
//
//     EXPECT(sut.is_eof());
// }
//
// TEST_CASE(should_consume_until_code_point)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界! How are you?"_utf8);
//
//     auto until_comma = sut.consume_until(U',');
//     EXPECT_EQ(until_comma.as_string(), "Hello");
//     EXPECT_EQ(sut.peek(), U',');
//
//     sut.consume(); // skip comma
//     sut.consume(); // skip space
//
//     auto until_exclamation = sut.consume_until(U'!');
//     EXPECT_EQ(until_exclamation.as_string(), "世界");
//     EXPECT_EQ(sut.peek(), U'!');
// }
//
// TEST_CASE(should_consume_until_utf8_view)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界! How are you?"_utf8);
//
//     auto until_non_ascii = sut.consume_until(u8"世界"_utf8.as_utf8_view());
//     EXPECT_EQ(until_non_ascii.as_string(), "Hello, ");
//     EXPECT_EQ(sut.peek(), U'世');
// }
//
// TEST_CASE(should_consume_quoted_string)
// {
//     Utf8GenericLexer sut(u8R"("Hello, 世界!" 'Single quotes' "Escaped \"quote\"")"_utf8);
//
//     auto quoted1 = sut.consume_quoted_string();
//     EXPECT_EQ(quoted1.as_string(), "Hello, 世界!");
//
//     sut.ignore_while(is_utf8_whitespace);
//
//     auto quoted2 = sut.consume_quoted_string();
//     EXPECT_EQ(quoted2.as_string(), "Single quotes");
//
//     sut.ignore_while(is_utf8_whitespace);
//
//     auto quoted3 = sut.consume_quoted_string(U'\\');
//     EXPECT_EQ(quoted3.as_string(), "Escaped \\\"quote\\\"");
// }
//
// TEST_CASE(should_consume_and_unescape_string)
// {
//     Utf8GenericLexer sut(u8R"("Hello\nWorld\t世界")"_utf8);
//
//     auto unescaped = sut.consume_and_unescape_string();
//     EXPECT(unescaped.has_value());
//     EXPECT_EQ(unescaped->bytes_as_string_view(), "Hello\nWorld\t世界");
// }
//
// TEST_CASE(should_ignore_code_points)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界!"_utf8);
//
//     sut.ignore(7); // "Hello, "
//     EXPECT_EQ(sut.peek(), U'世');
//
//     sut.ignore(); // default 1
//     EXPECT_EQ(sut.peek(), U'界');
// }
//
// TEST_CASE(should_ignore_until_code_point)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界! How are you?"_utf8);
//
//     sut.ignore_until(U'世');
//     EXPECT_EQ(sut.peek(), U'世');
//
//     sut.ignore_until(U'!');
//     EXPECT_EQ(sut.peek(), U'!');
// }
//
// TEST_CASE(should_ignore_until_utf8_view)
// {
//     Utf8GenericLexer sut(u8"Hello, 世界! How are you?"_utf8);
//
//     sut.ignore_until(u8"世界"_utf8.as_utf8_view());
//     EXPECT_EQ(sut.peek(), U'世');
// }
//
// TEST_CASE(should_work_with_predicates)
// {
//     Utf8GenericLexer sut(u8"Hello123 世界456"_utf8);
//
//     // Test next_is with predicate
//     EXPECT(sut.next_is(is_ascii_alpha_utf8));
//     EXPECT(!sut.next_is(is_ascii_digit_utf8));
//
//     // Consume alphabetic characters
//     auto letters = sut.consume_while(is_ascii_alpha_utf8);
//     EXPECT_EQ(letters.as_string(), "Hello");
//
//     // Consume digits
//     auto digits = sut.consume_while(is_ascii_digit_utf8);
//     EXPECT_EQ(digits.as_string(), "123");
//
//     // Skip space
//     sut.ignore_while(is_utf8_whitespace);
//
//     // Consume Non-Ascii characters (using lambda)
//     auto non_ascii = sut.consume_while([](u32 c) {
//         return c >= 0x4E00 && c <= 0x9FFF; // CJK Unified Ideographs
//     });
//     EXPECT_EQ(non_ascii.as_string(), "世界");
// }
//
// TEST_CASE(should_consume_until_with_predicate)
// {
//     Utf8GenericLexer sut(u8"Hello123World"_utf8);
//
//     auto until_digit = sut.consume_until(is_ascii_digit_utf8);
//     EXPECT_EQ(until_digit.as_string(), "Hello");
//
//     auto until_alpha = sut.consume_until(is_ascii_alpha_utf8);
//     EXPECT_EQ(until_alpha.as_string(), "123");
// }
//
// TEST_CASE(should_ignore_with_predicates)
// {
//     Utf8GenericLexer sut(u8"   \t\n  Hello"_utf8);
//
//     sut.ignore_while(is_utf8_whitespace);
//     EXPECT_EQ(sut.peek(), U'H');
//
//     sut.ignore_until([](u32 c) { return c == U'l'; });
//     EXPECT_EQ(sut.peek(), U'l');
// }
//
// TEST_CASE(predicate_helpers)
// {
//     // Test is_any_of_utf8
//     auto vowels = is_any_of_utf8(u8"aeiou"_utf8.as_utf8_view());
//     EXPECT(vowels(U'a'));
//     EXPECT(vowels(U'e'));
//     EXPECT(!vowels(U'b'));
//
//     // Test is_not_any_of_utf8
//     auto not_vowels = is_not_any_of_utf8(u8"aeiou"_utf8.as_utf8_view());
//     EXPECT(!not_vowels(U'a'));
//     EXPECT(not_vowels(U'b'));
//
//     // Test built-in predicates
//     EXPECT(is_utf8_whitespace(U' '));
//     EXPECT(is_utf8_whitespace(U'\t'));
//     EXPECT(is_utf8_whitespace(U'\n'));
//     EXPECT(is_utf8_whitespace(0x00A0)); // Non-breaking space
//     EXPECT(!is_utf8_whitespace(U'a'));
//
//     EXPECT(is_utf8_newline(U'\n'));
//     EXPECT(is_utf8_newline(U'\r'));
//     EXPECT(is_utf8_newline(0x2028)); // Line separator
//     EXPECT(!is_utf8_newline(U' '));
//
//     EXPECT(is_ascii_digit_utf8(U'5'));
//     EXPECT(!is_ascii_digit_utf8(U'a'));
//
//     EXPECT(is_ascii_alpha_utf8(U'a'));
//     EXPECT(is_ascii_alpha_utf8(U'Z'));
//     EXPECT(!is_ascii_alpha_utf8(U'5'));
//
//     EXPECT(is_ascii_alnum_utf8(U'a'));
//     EXPECT(is_ascii_alnum_utf8(U'5'));
//     EXPECT(!is_ascii_alnum_utf8(U'!'));
// }
//
// TEST_CASE(consume_decimal_integer_correctly_parses_utf8)
// {
// #define CHECK_PARSES_INTEGER_UTF8(test, expected, type)                    \
//     do {                                                                   \
//         Utf8GenericLexer lexer(u8##test##_utf8);                          \
//         auto actual = lexer.consume_decimal_integer<type>();               \
//         VERIFY(!actual.is_error());                                        \
//         EXPECT_EQ(actual.value(), static_cast<type>(expected));            \
//         EXPECT(lexer.is_eof() || !is_ascii_digit_utf8(lexer.peek()));      \
//     } while (false)
//
//     CHECK_PARSES_INTEGER_UTF8("0", 0, u8);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, u8);
//     CHECK_PARSES_INTEGER_UTF8("10", 10, u8);
//     CHECK_PARSES_INTEGER_UTF8("255", 255, u8);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, u16);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, u16);
//     CHECK_PARSES_INTEGER_UTF8("1234", 1234, u16);
//     CHECK_PARSES_INTEGER_UTF8("65535", 65535, u16);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, u32);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, u32);
//     CHECK_PARSES_INTEGER_UTF8("1234", 1234, u32);
//     CHECK_PARSES_INTEGER_UTF8("4294967295", 4294967295, u32);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, u64);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, u64);
//     CHECK_PARSES_INTEGER_UTF8("1234", 1234, u64);
//     CHECK_PARSES_INTEGER_UTF8("18446744073709551615", 18446744073709551615ULL, u64);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, i8);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, i8);
//     CHECK_PARSES_INTEGER_UTF8("10", 10, i8);
//     CHECK_PARSES_INTEGER_UTF8("-10", -10, i8);
//     CHECK_PARSES_INTEGER_UTF8("127", 127, i8);
//     CHECK_PARSES_INTEGER_UTF8("-128", -128, i8);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, i16);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, i16);
//     CHECK_PARSES_INTEGER_UTF8("1234", 1234, i16);
//     CHECK_PARSES_INTEGER_UTF8("-1234", -1234, i16);
//     CHECK_PARSES_INTEGER_UTF8("32767", 32767, i16);
//     CHECK_PARSES_INTEGER_UTF8("-32768", -32768, i16);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, i32);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, i32);
//     CHECK_PARSES_INTEGER_UTF8("1234", 1234, i32);
//     CHECK_PARSES_INTEGER_UTF8("-1234", -1234, i32);
//     CHECK_PARSES_INTEGER_UTF8("2147483647", 2147483647, i32);
//     CHECK_PARSES_INTEGER_UTF8("-2147483648", -2147483648, i32);
//     CHECK_PARSES_INTEGER_UTF8("0", 0, i64);
//     CHECK_PARSES_INTEGER_UTF8("-0", -0, i64);
//     CHECK_PARSES_INTEGER_UTF8("1234", 1234, i64);
//     CHECK_PARSES_INTEGER_UTF8("-1234", -1234, i64);
//     CHECK_PARSES_INTEGER_UTF8("9223372036854775807", 9223372036854775807, i64);
//     CHECK_PARSES_INTEGER_UTF8("-9223372036854775808", -9223372036854775808ULL, i64);
// #undef CHECK_PARSES_INTEGER_UTF8
// }
//
// TEST_CASE(consume_decimal_integer_fails_with_correct_error_utf8)
// {
// #define CHECK_FAILS_WITH_ERROR_UTF8(test, type, err)                      \
//     do {                                                                   \
//         Utf8GenericLexer lexer(u8##test##_utf8);                          \
//         auto actual = lexer.consume_decimal_integer<type>();               \
//         VERIFY(actual.is_error() && actual.error().is_errno());           \
//         EXPECT_EQ(actual.error().code(), err);                            \
//         EXPECT_EQ(lexer.tell(), static_cast<size_t>(0));                  \
//     } while (false)
//
//     CHECK_FAILS_WITH_ERROR_UTF8("Well hello Utf8GenericLexer! 世界", u64, EINVAL);
//     CHECK_FAILS_WITH_ERROR_UTF8("+", u64, EINVAL);
//     CHECK_FAILS_WITH_ERROR_UTF8("+WHF", u64, EINVAL);
//     CHECK_FAILS_WITH_ERROR_UTF8("-WHF", u64, EINVAL);
//     CHECK_FAILS_WITH_ERROR_UTF8("-1", u8, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-100", u8, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-1", u16, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-100", u16, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-1", u32, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-100", u32, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-1", u64, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-100", u64, ERANGE);
//
//     CHECK_FAILS_WITH_ERROR_UTF8("-129", i8, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("128", i8, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-32769", i16, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("32768", i16, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-2147483649", i32, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("2147483648", i32, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("-9223372036854775809", i64, ERANGE);
//     CHECK_FAILS_WITH_ERROR_UTF8("9223372036854775808", i64, ERANGE);
// #undef CHECK_FAILS_WITH_ERROR_UTF8
// }
//
// TEST_CASE(consume_escaped_code_point_utf8)
// {
//     auto test = [](StringView test_input, Result<u32, Utf8GenericLexer::UnicodeEscapeError> expected, bool combine_surrogate_pairs = true) {
//         Utf8GenericLexer lexer(test_input);
//
//         auto actual = lexer.consume_escaped_code_point(combine_surrogate_pairs);
//         EXPECT_EQ(actual.is_error(), expected.is_error());
//
//         if (actual.is_error() && expected.is_error())
//             EXPECT_EQ(actual.error(), expected.error());
//         else
//             EXPECT_EQ(actual.value(), expected.value());
//     };
//
//     test("\\u"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u{"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u{1"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u{}"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u{x}"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//
//     test("\\u{110000}"sv, Utf8GenericLexer::UnicodeEscapeError::UnicodeEscapeOverflow);
//     test("\\u{f00000000}"sv, Utf8GenericLexer::UnicodeEscapeError::UnicodeEscapeOverflow);
//
//     test("\\u{0}"sv, 0);
//     test("\\u{41}"sv, 0x41);
//     test("\\u{ffff}"sv, 0xffff);
//     test("\\u{10ffff}"sv, 0x10ffff);
//
//     test("\\u1"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u11"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u111"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\u111x"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\ud800\\u"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\ud800\\u1"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\ud800\\u11"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\ud800\\u111"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//     test("\\ud800\\u111x"sv, Utf8GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
//
//     test("\\u0000"sv, 0x0);
//     test("\\u0041"sv, 0x41);
//     test("\\uffff"sv, 0xffff);
//
//     test("\\ud83d"sv, 0xd83d);
//     test("\\ud83d\\u1111"sv, 0xd83d);
//     test("\\ud83d\\ude00"sv, 0x1f600);
//     test("\\ud83d\\ude00"sv, 0xd83d, false);
// }
//
// TEST_CASE(utf8_string_literal_helper)
// {
//     // Test the UTF-8 string literal helper
//     auto literal = u8"Hello, 世界! 🌍"_utf8;
//
//     EXPECT_EQ(literal.size, 18u); // Byte count (Hello, = 7, 世界 = 6, ! = 1, space = 1, 🌍 = 4, null terminator excluded)
//
//     auto view = literal.as_string_view();
//     EXPECT_EQ(view.length(), 18u);
//
//     auto utf8_view = literal.as_utf8_view();
//     EXPECT_EQ(utf8_view.length(), 12u); // Code point count
//
//     Utf8GenericLexer lexer(literal);
//     EXPECT_EQ(lexer.peek(), U'H');
// }
//
// TEST_CASE(complex_unicode_parsing)
// {
//     // Test with various Unicode categories
//     auto input = u8"ASCII αβγ العربية 中文 🌍🎉 Ελληνικά"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     // ASCII part
//     auto ascii = lexer.consume_while(is_ascii_alnum_utf8);
//     EXPECT_EQ(ascii.as_string(), "ASCII");
//
//     // Skip space
//     lexer.ignore_while(is_utf8_whitespace);
//
//     // Greek letters
//     auto greek_letters = lexer.consume_while([](u32 c) {
//         return c >= 0x0370 && c <= 0x03FF; // Greek and Coptic block
//     });
//     EXPECT_EQ(greek_letters.as_string(), "αβγ");
//
//     // Skip space
//     lexer.ignore_while(is_utf8_whitespace);
//
//     // Arabic
//     auto arabic = lexer.consume_while([](u32 c) {
//         return c >= 0x0600 && c <= 0x06FF; // Arabic block
//     });
//     EXPECT_EQ(arabic.as_string(), "العربية");
//
//     // Skip space
//     lexer.ignore_while(is_utf8_whitespace);
//
//     // Non-Ascii
//     auto non_ascii = lexer.consume_while([](u32 c) {
//         return c >= 0x4E00 && c <= 0x9FFF; // CJK Unified Ideographs
//     });
//     EXPECT_EQ(non_ascii.as_string(), "中文");
//
//     // Skip space
//     lexer.ignore_while(is_utf8_whitespace);
//
//     // Emojis
//     auto emojis = lexer.consume_while([](u32 c) {
//         return c >= 0x1F300 && c <= 0x1F9FF; // Miscellaneous Symbols and Pictographs, etc.
//     });
//     EXPECT_EQ(emojis.as_string(), "🌍🎉");
//
//     // Skip space
//     lexer.ignore_while(is_utf8_whitespace);
//
//     // More Greek
//     auto more_greek = lexer.consume_while([](u32 c) {
//         return c >= 0x0370 && c <= 0x03FF;
//     });
//     EXPECT_EQ(more_greek.as_string(), "Ελληνικά");
//
//     EXPECT(lexer.is_eof());
// }
//
// TEST_CASE(mixed_quoted_strings_with_unicode)
// {
//     auto input = u8R"("English" "العربية" "中文" "Ελληνικά" "🌍🎉")"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     Vector<String> expected_strings = {
//         String::from_utf8("English"sv).release_value(),
//         String::from_utf8("العربية"sv).release_value(),
//         String::from_utf8("中文"sv).release_value(),
//         String::from_utf8("Ελληνικά"sv).release_value(),
//         String::from_utf8("🌍🎉"sv).release_value()
//     };
//
//     size_t index = 0;
//     while (!lexer.is_eof()) {
//         lexer.ignore_while(is_utf8_whitespace);
//
//         if (lexer.is_eof()) break;
//
//         auto quoted = lexer.consume_quoted_string();
//         EXPECT(!quoted.is_empty());
//
//         if (index < expected_strings.size()) {
//             EXPECT_EQ(quoted.as_string(), expected_strings[index].bytes_as_string_view());
//             ++index;
//         }
//     }
//
//     EXPECT_EQ(index, expected_strings.size());
// }
//
// TEST_CASE(escape_sequences_with_unicode)
// {
//     auto input = u8R"("Hello\nWorld\t世界\r\n")"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     auto unescaped = lexer.consume_and_unescape_string();
//     EXPECT(unescaped.has_value());
//     EXPECT_EQ(unescaped->bytes_as_string_view(), "Hello\nWorld\t世界\r\n");
// }
//
// TEST_CASE(numbers_with_unicode_context)
// {
//     auto input = u8"Price: 42€, Quantity: 世界123, ID: 456🎉"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     // Skip to first number
//     lexer.ignore_until(is_ascii_digit_utf8);
//     auto price = lexer.consume_decimal_integer<u32>();
//     EXPECT(!price.is_error());
//     EXPECT_EQ(price.value(), 42u);
//
//     // Skip to second number
//     lexer.ignore_until(is_ascii_digit_utf8);
//     auto quantity = lexer.consume_decimal_integer<u32>();
//     EXPECT(!quantity.is_error());
//     EXPECT_EQ(quantity.value(), 123u);
//
//     // Skip to third number
//     lexer.ignore_until(is_ascii_digit_utf8);
//     auto id = lexer.consume_decimal_integer<u32>();
//     EXPECT(!id.is_error());
//     EXPECT_EQ(id.value(), 456u);
// }
//
// TEST_CASE(line_handling_with_unicode_line_separators)
// {
//     // Test various Unicode line separators
//     auto input = u8"Line 1\nLine 2\rLine 3\r\nLine 4\u2028Line 5\u2029Line 6"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     auto line1 = lexer.consume_line();
//     EXPECT_EQ(line1.as_string(), "Line 1");
//
//     auto line2 = lexer.consume_line();
//     EXPECT_EQ(line2.as_string(), "Line 2");
//
//     auto line3 = lexer.consume_line();
//     EXPECT_EQ(line3.as_string(), "Line 3");
//
//     auto line4 = lexer.consume_line();
//     EXPECT_EQ(line4.as_string(), "Line 4");
//
//     auto line5 = lexer.consume_line();
//     EXPECT_EQ(line5.as_string(), "Line 5");
//
//     auto line6 = lexer.consume_line();
//     EXPECT_EQ(line6.as_string(), "Line 6");
//
//     EXPECT(lexer.is_eof());
// }
//
// TEST_CASE(remaining_view_with_unicode)
// {
//     auto input = u8"Hello, 世界! 🌍"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     // Consume "Hello, "
//     lexer.consume(7);
//
//     auto remaining = lexer.remaining();
//     EXPECT_EQ(remaining.as_string(), "世界! 🌍");
//
//     // Consume one more character
//     lexer.consume();
//
//     remaining = lexer.remaining();
//     EXPECT_EQ(remaining.as_string(), "界! 🌍");
// }
//
// TEST_CASE(peek_beyond_boundaries)
// {
//     auto input = u8"Hi"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     EXPECT_EQ(lexer.peek(0), U'H');
//     EXPECT_EQ(lexer.peek(1), U'i');
//     EXPECT_EQ(lexer.peek(2), 0u); // Beyond end
//     EXPECT_EQ(lexer.peek(100), 0u); // Way beyond end
//
//     // Consume everything
//     lexer.consume_all();
//     EXPECT_EQ(lexer.peek(), 0u);
//     EXPECT_EQ(lexer.peek(0), 0u);
// }
//
// TEST_CASE(retreat_from_beginning)
// {
//     auto input = u8"Hello"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     // At beginning - retreat should be safe (though this might VERIFY in debug)
//     // We test that it doesn't crash in release builds
//     auto initial_pos = lexer.tell();
//
//     // Consume one character then retreat
//     lexer.consume();
//     EXPECT_EQ(lexer.peek(), U'e');
//
//     lexer.retreat();
//     EXPECT_EQ(lexer.peek(), U'H');
//     EXPECT_EQ(lexer.tell(), initial_pos);
// }
//
// TEST_CASE(empty_input_handling)
// {
//     Utf8GenericLexer lexer(u8""_utf8);
//
//     EXPECT(lexer.is_eof());
//     EXPECT_EQ(lexer.tell(), 0u);
//     EXPECT_EQ(lexer.tell_remaining(), 0u);
//     EXPECT_EQ(lexer.peek(), 0u);
//     EXPECT(!lexer.peek_string(1).has_value());
//
//     auto remaining = lexer.remaining();
//     EXPECT(remaining.is_empty());
//
//     auto consumed = lexer.consume_all();
//     EXPECT(consumed.is_empty());
// }
//
// TEST_CASE(unicode_whitespace_handling)
// {
//     // Test various Unicode whitespace characters
//     auto input = u8" \t\n\r\f\v\u00A0\u2000\u2001\u2028\u2029Hello"_utf8;
//     Utf8GenericLexer lexer(input);
//
//     lexer.ignore_while(is_utf8_whitespace);
//     EXPECT_EQ(lexer.peek(), U'H');
//
//     auto remaining = lexer.consume_all();
//     EXPECT_EQ(remaining.as_string(), "Hello");
// }
