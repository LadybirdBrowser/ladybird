/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/GenericLexer.h>
#include <AK/StringView.h>

TEST_CASE(should_constexpr_construct_from_empty_string_view)
{
    constexpr GenericLexer sut(StringView {});
    static_assert(sut.is_eof());
}

TEST_CASE(should_construct_from_string_view)
{
    constexpr GenericLexer sut("abcdef"sv);
    static_assert(!sut.is_eof());
}

TEST_CASE(should_constexpr_tell)
{
    constexpr GenericLexer sut("abcdef"sv);
    static_assert(sut.tell() == 0);
}

TEST_CASE(should_constexpr_tell_remaining)
{
    constexpr GenericLexer sut("abcdef"sv);
    static_assert(sut.tell_remaining() == 6);
}

TEST_CASE(should_constexpr_peek)
{
    constexpr GenericLexer sut("abcdef"sv);
    static_assert(sut.peek() == 'a');
    static_assert(sut.peek(2) == 'c');
    static_assert(sut.peek(100) == '\0');
}

TEST_CASE(should_constexpr_next_is)
{
    constexpr GenericLexer sut("abcdef"sv);
    static_assert(sut.next_is('a'));
    static_assert(sut.next_is("abc"));
    static_assert(sut.next_is("abc"sv));
}

TEST_CASE(should_constexpr_retreat)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.consume();
        sut.retreat();
        return sut;
    }();
    static_assert(sut.peek() == 'a');
}

TEST_CASE(should_constexpr_consume_1)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.consume();
        return sut;
    }();
    static_assert(sut.peek() == 'b');
}

TEST_CASE(should_constexpr_consume_specific_char)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.consume_specific('a');
        return sut;
    }();
    static_assert(sut.peek() == 'b');
}

TEST_CASE(should_constexpr_consume_specific_string_view)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.consume_specific("ab"sv);
        return sut;
    }();
    static_assert(sut.peek() == 'c');
}

TEST_CASE(should_constexpr_consume_specific_cstring)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.consume_specific("abcd"sv);
        return sut;
    }();
    static_assert(sut.peek() == 'e');
}

TEST_CASE(should_constexpr_ignore_until)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.ignore_until('d');
        return sut;
    }();
    static_assert(sut.peek() == 'd');
}

TEST_CASE(should_constexpr_ignore_until_cstring)
{
    constexpr auto sut = [] {
        GenericLexer sut("abcdef"sv);
        sut.ignore_until("cde");
        return sut;
    }();
    static_assert(sut.peek() == 'c');
}

TEST_CASE(should_constexpr_next_is_pred)
{
    constexpr auto pred = [](auto c) {
        return c == 'a';
    };
    constexpr GenericLexer sut("abcdef"sv);
    static_assert(sut.next_is(pred));
}

TEST_CASE(should_constexpr_ignore_while_pred)
{
    constexpr auto sut = [] {
        constexpr auto pred = [](auto c) {
            return c == 'a';
        };

        GenericLexer sut("abcdef"sv);
        sut.ignore_while(pred);
        return sut;
    }();
    static_assert(sut.peek() == 'b');
}

TEST_CASE(should_constexpr_ignore_until_pred)
{
    constexpr auto sut = [] {
        constexpr auto pred = [](auto c) {
            return c == 'c';
        };

        GenericLexer sut("abcdef"sv);
        sut.ignore_until(pred);
        return sut;
    }();
    static_assert(sut.peek() == 'c');
}

TEST_CASE(consume_escaped_code_point)
{
    auto test = [](StringView test, Result<u32, GenericLexer::UnicodeEscapeError> expected, bool combine_surrogate_pairs = true) {
        GenericLexer lexer(test);

        auto actual = lexer.consume_escaped_code_point(combine_surrogate_pairs);
        EXPECT_EQ(actual.is_error(), expected.is_error());

        if (actual.is_error() && expected.is_error())
            EXPECT_EQ(actual.error(), expected.error());
        else
            EXPECT_EQ(actual.value(), expected.value());
    };

    test("\\u"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u{"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u{1"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u{}"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u{x}"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);

    test("\\u{110000}"sv, GenericLexer::UnicodeEscapeError::UnicodeEscapeOverflow);
    test("\\u{f00000000}"sv, GenericLexer::UnicodeEscapeError::UnicodeEscapeOverflow);

    test("\\u{0}"sv, 0);
    test("\\u{41}"sv, 0x41);
    test("\\u{ffff}"sv, 0xffff);
    test("\\u{10ffff}"sv, 0x10ffff);

    test("\\u1"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u11"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u111"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\u111x"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\ud800\\u"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\ud800\\u1"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\ud800\\u11"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\ud800\\u111"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);
    test("\\ud800\\u111x"sv, GenericLexer::UnicodeEscapeError::MalformedUnicodeEscape);

    test("\\u0000"sv, 0x0);
    test("\\u0041"sv, 0x41);
    test("\\uffff"sv, 0xffff);

    test("\\ud83d"sv, 0xd83d);
    test("\\ud83d\\u1111"sv, 0xd83d);
    test("\\ud83d\\ude00"sv, 0x1f600);
    test("\\ud83d\\ude00"sv, 0xd83d, false);
}

TEST_CASE(consume_decimal_integer_correctly_parses)
{
#define CHECK_PARSES_INTEGER(test, expected, type)              \
    do {                                                        \
        GenericLexer lexer(test##sv);                           \
        auto actual = lexer.consume_decimal_integer<type>();    \
        VERIFY(!actual.is_error());                             \
        EXPECT_EQ(actual.value(), static_cast<type>(expected)); \
        EXPECT_EQ(lexer.tell(), test##sv.length());             \
    } while (false)
    CHECK_PARSES_INTEGER("0", 0, u8);
    CHECK_PARSES_INTEGER("-0", -0, u8);
    CHECK_PARSES_INTEGER("10", 10, u8);
    CHECK_PARSES_INTEGER("255", 255, u8);
    CHECK_PARSES_INTEGER("0", 0, u16);
    CHECK_PARSES_INTEGER("-0", -0, u16);
    CHECK_PARSES_INTEGER("1234", 1234, u16);
    CHECK_PARSES_INTEGER("65535", 65535, u16);
    CHECK_PARSES_INTEGER("0", 0, u32);
    CHECK_PARSES_INTEGER("-0", -0, u32);
    CHECK_PARSES_INTEGER("1234", 1234, u32);
    CHECK_PARSES_INTEGER("4294967295", 4294967295, u32);
    CHECK_PARSES_INTEGER("0", 0, u64);
    CHECK_PARSES_INTEGER("-0", -0, u64);
    CHECK_PARSES_INTEGER("1234", 1234, u64);
    CHECK_PARSES_INTEGER("18446744073709551615", 18446744073709551615ULL, u64);
    CHECK_PARSES_INTEGER("0", 0, i8);
    CHECK_PARSES_INTEGER("-0", -0, i8);
    CHECK_PARSES_INTEGER("10", 10, i8);
    CHECK_PARSES_INTEGER("-10", -10, i8);
    CHECK_PARSES_INTEGER("127", 127, i8);
    CHECK_PARSES_INTEGER("-128", -128, i8);
    CHECK_PARSES_INTEGER("0", 0, i16);
    CHECK_PARSES_INTEGER("-0", -0, i16);
    CHECK_PARSES_INTEGER("1234", 1234, i16);
    CHECK_PARSES_INTEGER("-1234", -1234, i16);
    CHECK_PARSES_INTEGER("32767", 32767, i16);
    CHECK_PARSES_INTEGER("-32768", -32768, i16);
    CHECK_PARSES_INTEGER("0", 0, i32);
    CHECK_PARSES_INTEGER("-0", -0, i32);
    CHECK_PARSES_INTEGER("1234", 1234, i32);
    CHECK_PARSES_INTEGER("-1234", -1234, i32);
    CHECK_PARSES_INTEGER("2147483647", 2147483647, i32);
    CHECK_PARSES_INTEGER("-2147483648", -2147483648, i32);
    CHECK_PARSES_INTEGER("0", 0, i64);
    CHECK_PARSES_INTEGER("-0", -0, i64);
    CHECK_PARSES_INTEGER("1234", 1234, i64);
    CHECK_PARSES_INTEGER("-1234", -1234, i64);
    CHECK_PARSES_INTEGER("9223372036854775807", 9223372036854775807, i64);
    CHECK_PARSES_INTEGER("-9223372036854775808", -9223372036854775808ULL, i64);
#undef CHECK_PARSES_INTEGER
}

TEST_CASE(consume_decimal_integer_fails_with_correct_error)
{
#define CHECK_FAILS_WITH_ERROR(test, type, err)                 \
    do {                                                        \
        GenericLexer lexer(test##sv);                           \
        auto actual = lexer.consume_decimal_integer<type>();    \
        VERIFY(actual.is_error() && actual.error().is_errno()); \
        EXPECT_EQ(actual.error().code(), err);                  \
        EXPECT_EQ(lexer.tell(), static_cast<size_t>(0));        \
    } while (false)
    CHECK_FAILS_WITH_ERROR("Well hello GenericLexer!", u64, EINVAL);
    CHECK_FAILS_WITH_ERROR("+", u64, EINVAL);
    CHECK_FAILS_WITH_ERROR("+WHF", u64, EINVAL);
    CHECK_FAILS_WITH_ERROR("-WHF", u64, EINVAL);
    CHECK_FAILS_WITH_ERROR("-1", u8, ERANGE);
    CHECK_FAILS_WITH_ERROR("-100", u8, ERANGE);
    CHECK_FAILS_WITH_ERROR("-1", u16, ERANGE);
    CHECK_FAILS_WITH_ERROR("-100", u16, ERANGE);
    CHECK_FAILS_WITH_ERROR("-1", u32, ERANGE);
    CHECK_FAILS_WITH_ERROR("-100", u32, ERANGE);
    CHECK_FAILS_WITH_ERROR("-1", u64, ERANGE);
    CHECK_FAILS_WITH_ERROR("-100", u64, ERANGE);

    CHECK_FAILS_WITH_ERROR("-129", i8, ERANGE);
    CHECK_FAILS_WITH_ERROR("128", i8, ERANGE);
    CHECK_FAILS_WITH_ERROR("-32769", i16, ERANGE);
    CHECK_FAILS_WITH_ERROR("32768", i16, ERANGE);
    CHECK_FAILS_WITH_ERROR("-2147483649", i32, ERANGE);
    CHECK_FAILS_WITH_ERROR("2147483648", i32, ERANGE);
    CHECK_FAILS_WITH_ERROR("-9223372036854775809", i64, ERANGE);
    CHECK_FAILS_WITH_ERROR("9223372036854775808", i64, ERANGE);
#undef CHECK_FAILS_WITH_ERROR
}
