/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/Enumerate.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <AK/Utf32View.h>

static Utf16String make_copy(Utf16String const& string)
{
    return string.has_ascii_storage()
        ? Utf16String::from_utf8(string.ascii_view())
        : Utf16String::from_utf16(string.utf16_view());
}

TEST_CASE(empty_string)
{
    Utf16String string {};
    EXPECT(string.is_empty());
    EXPECT(string.is_ascii());
    EXPECT(!string.has_long_ascii_storage());
    EXPECT(string.has_short_ascii_storage());
    EXPECT_EQ(string.length_in_code_units(), 0uz);
    EXPECT_EQ(string.length_in_code_points(), 0uz);
    EXPECT_EQ(string.ascii_view(), StringView {});
}

TEST_CASE(from_utf8)
{
    {
        auto string = Utf16String::from_utf8("hello!"sv);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 6uz);
        EXPECT_EQ(string.length_in_code_points(), 6uz);
        EXPECT_EQ(string.ascii_view(), "hello!"sv);
    }
    {
        auto string = Utf16String::from_utf8("hello there!"sv);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 12uz);
        EXPECT_EQ(string.length_in_code_points(), 12uz);
        EXPECT_EQ(string.ascii_view(), "hello there!"sv);
    }
    {
        auto string = Utf16String::from_utf8("😀"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 2uz);
        EXPECT_EQ(string.length_in_code_points(), 1uz);
        EXPECT_EQ(string.utf16_view(), u"😀"sv);
    }
    {
        auto string = Utf16String::from_utf8("hello 😀 there!"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 15uz);
        EXPECT_EQ(string.length_in_code_points(), 14uz);
        EXPECT_EQ(string.utf16_view(), u"hello 😀 there!"sv);
    }
    {
        auto string = Utf16String::from_utf8("hello \xed\xa0\x80!"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 8uz);
        EXPECT_EQ(string.length_in_code_points(), 8uz);
        EXPECT_EQ(string.utf16_view(), u"hello \xd800!"sv);
    }
    {
        auto string = Utf16String::from_utf8("hello \xed\xb0\x80!"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 8uz);
        EXPECT_EQ(string.length_in_code_points(), 8uz);
        EXPECT_EQ(string.utf16_view(), u"hello \xdc00!"sv);
    }
}

TEST_CASE(from_utf8_with_replacement_character)
{
    auto string1 = Utf16String::from_utf8_with_replacement_character("long string \xf4\x8f\xbf\xc0"sv, Utf16String::WithBOMHandling::No); // U+110000
    EXPECT_EQ(string1, u"long string \ufffd\ufffd\ufffd\ufffd"sv);

    auto string3 = Utf16String::from_utf8_with_replacement_character("A valid string!"sv, Utf16String::WithBOMHandling::No);
    EXPECT_EQ(string3, "A valid string!"sv);

    auto string4 = Utf16String::from_utf8_with_replacement_character(""sv, Utf16String::WithBOMHandling::No);
    EXPECT_EQ(string4, ""sv);

    auto string5 = Utf16String::from_utf8_with_replacement_character("\xEF\xBB\xBFWHF!"sv, Utf16String::WithBOMHandling::Yes);
    EXPECT_EQ(string5, "WHF!"sv);

    auto string6 = Utf16String::from_utf8_with_replacement_character("\xEF\xBB\xBFWHF!"sv, Utf16String::WithBOMHandling::No);
    EXPECT_EQ(string6, u"\ufeffWHF!"sv);

    auto string7 = Utf16String::from_utf8_with_replacement_character("\xED\xA0\x80WHF!"sv); // U+D800
    EXPECT_EQ(string7, u"\ufffdWHF!"sv);
}

TEST_CASE(from_utf16)
{
    {
        auto string = Utf16String::from_utf16(u"hello!"sv);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 6uz);
        EXPECT_EQ(string.length_in_code_points(), 6uz);
        EXPECT_EQ(string.ascii_view(), "hello!"sv);
    }
    {
        auto string = Utf16String::from_utf16(u"hello there!"sv);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 12uz);
        EXPECT_EQ(string.length_in_code_points(), 12uz);
        EXPECT_EQ(string.ascii_view(), "hello there!"sv);
    }
    {
        auto string = Utf16String::from_utf16(u"😀"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 2uz);
        EXPECT_EQ(string.length_in_code_points(), 1uz);
        EXPECT_EQ(string.utf16_view(), u"😀"sv);
    }
    {
        auto string = Utf16String::from_utf16(u"hello 😀 there!"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 15uz);
        EXPECT_EQ(string.length_in_code_points(), 14uz);
        EXPECT_EQ(string.utf16_view(), u"hello 😀 there!"sv);
    }
    {
        auto string = Utf16String::from_utf16(u"hello \xd800!"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 8uz);
        EXPECT_EQ(string.length_in_code_points(), 8uz);
        EXPECT_EQ(string.utf16_view(), u"hello \xd800!"sv);
    }
    {
        auto string = Utf16String::from_utf16(u"hello \xdc00!"sv);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 8uz);
        EXPECT_EQ(string.length_in_code_points(), 8uz);
        EXPECT_EQ(string.utf16_view(), u"hello \xdc00!"sv);
    }
}

TEST_CASE(from_utf32)
{
    auto strlen32 = [](char32_t const* string) {
        auto const* start = string;
        while (*start)
            ++start;
        return static_cast<size_t>(start - string);
    };

    auto to_utf32_view = [&](char32_t const* string) {
        return Utf32View { reinterpret_cast<u32 const*>(string), strlen32(string) };
    };

    {
        auto string = Utf16String::from_utf32(to_utf32_view(U"hello!"));
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 6uz);
        EXPECT_EQ(string.length_in_code_points(), 6uz);
        EXPECT_EQ(string.ascii_view(), "hello!"sv);
    }
    {
        auto string = Utf16String::from_utf32(to_utf32_view(U"hello there!"));
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 12uz);
        EXPECT_EQ(string.length_in_code_points(), 12uz);
        EXPECT_EQ(string.ascii_view(), "hello there!"sv);
    }
    {
        auto string = Utf16String::from_utf32(to_utf32_view(U"😀"));
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 2uz);
        EXPECT_EQ(string.length_in_code_points(), 1uz);
        EXPECT_EQ(string.utf16_view(), u"😀"sv);
    }
    {
        auto string = Utf16String::from_utf32(to_utf32_view(U"hello 😀 there!"));
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 15uz);
        EXPECT_EQ(string.length_in_code_points(), 14uz);
        EXPECT_EQ(string.utf16_view(), u"hello 😀 there!"sv);
    }
    {
        auto string = Utf16String::from_utf32(to_utf32_view(U"hello \xd800!"));
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 8uz);
        EXPECT_EQ(string.length_in_code_points(), 8uz);
        EXPECT_EQ(string.utf16_view(), u"hello \xd800!"sv);
    }
    {
        auto string = Utf16String::from_utf32(to_utf32_view(U"hello \xdc00!"));
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 8uz);
        EXPECT_EQ(string.length_in_code_points(), 8uz);
        EXPECT_EQ(string.utf16_view(), u"hello \xdc00!"sv);
    }
}

TEST_CASE(from_code_point)
{
    u32 code_point = 0;

    for (; code_point < AK::UnicodeUtils::FIRST_SUPPLEMENTARY_PLANE_CODE_POINT; ++code_point) {
        auto string = Utf16String::from_code_point(code_point);
        EXPECT_EQ(string.length_in_code_units(), 1uz);
        EXPECT_EQ(string.length_in_code_points(), 1uz);
        EXPECT_EQ(string.code_point_at(0), code_point);
        EXPECT_EQ(string.code_unit_at(0), code_point);
    }

    for (; code_point < AK::UnicodeUtils::FIRST_SUPPLEMENTARY_PLANE_CODE_POINT + 10'000; ++code_point) {
        auto string = Utf16String::from_code_point(code_point);
        EXPECT_EQ(string.length_in_code_units(), 2uz);
        EXPECT_EQ(string.length_in_code_points(), 1uz);
        EXPECT_EQ(string.code_point_at(0), code_point);

        size_t i = 0;
        (void)AK::UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
            EXPECT_EQ(string.code_unit_at(i++), code_unit);
        });
        EXPECT_EQ(i, 2uz);
    }
}

TEST_CASE(formatted)
{
    {
        auto string = Utf16String::formatted("{}", 42);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 2uz);
        EXPECT_EQ(string.length_in_code_points(), 2uz);
        EXPECT_EQ(string, u"42"sv);
    }
    {
        auto string = Utf16String::number(42);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 2uz);
        EXPECT_EQ(string.length_in_code_points(), 2uz);
        EXPECT_EQ(string, u"42"sv);
    }
    {
        auto string = Utf16String::formatted("whf {} {} {}!", "😀"sv, Utf16View { u"🍕"sv }, 3.14);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 15uz);
        EXPECT_EQ(string.length_in_code_points(), 13uz);
        EXPECT_EQ(string, u"whf 😀 🍕 3.14!"sv);
    }
    {
        Array segments {
            u"abcdefghijklmnopqrstuvwxyz"sv,
            u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv,
            u"abcdefghijklmnopqrstuvwxyz"sv,
            u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv,
            u"abcdefghijklmnopqrstuvwxyz"sv,
            u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv,
        };

        auto string = Utf16String::join(u"--"sv, segments);
        EXPECT(!string.is_empty());
        EXPECT(string.is_ascii());
        EXPECT(string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 166uz);
        EXPECT_EQ(string.length_in_code_points(), 166uz);
        EXPECT_EQ(string, u"abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ--abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ--abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv);
    }
    {
        Array segments {
            u"abcdefghijklmnopqrstuvwxyz"sv,
            u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv,
            u"\xd83d\xde00"sv,
            u"abcdefghijklmnopqrstuvwxyz"sv,
            u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv,
            u"🍕"sv,
            u"abcdefghijklmnopqrstuvwxyz"sv,
            u"ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv,
        };

        auto string = Utf16String::join(u"--"sv, segments);
        EXPECT(!string.is_empty());
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 174uz);
        EXPECT_EQ(string.length_in_code_points(), 172uz);
        EXPECT_EQ(string, u"abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ--😀--abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ--🍕--abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv);
    }
}

TEST_CASE(copy_operations)
{
    auto test = [](Utf16String const& string1) {
        auto original = make_copy(string1);

        // Copy constructor.
        Utf16String string2(string1);

        EXPECT_EQ(string1, original);
        EXPECT_EQ(string1, string2);

        // Copy assignment.
        Utf16String string3;
        string3 = string1;

        EXPECT_EQ(string1, original);
        EXPECT_EQ(string1, string3);
    };

    test({});
    test("hello"_utf16);
    test("hello there general!"_utf16);
    test("hello 😀 there!"_utf16);
}

TEST_CASE(move_operations)
{
    auto test = [](Utf16String string1) {
        auto original = make_copy(string1);

        // Move constructor.
        Utf16String string2(move(string1));

        EXPECT(string1.is_empty());
        EXPECT_EQ(string1, Utf16String {});
        EXPECT_EQ(string2, original);

        // Move assignment.
        Utf16String string3;
        string3 = move(string2);

        EXPECT(string2.is_empty());
        EXPECT_EQ(string2, Utf16String {});
        EXPECT_EQ(string3, original);
    };

    test({});
    test("hello"_utf16);
    test("hello there general!"_utf16);
    test("hello 😀 there!"_utf16);
}

TEST_CASE(equals)
{
    auto test = [](Utf16String const& string1, Utf16String const& inequal_string) {
        auto string2 = make_copy(string1);

        EXPECT_EQ(string1, string1);
        EXPECT_EQ(string1, string2);
        EXPECT_EQ(string2, string1);
        EXPECT_EQ(string2, string2);

        if (string1.has_long_utf16_storage()) {
            EXPECT_EQ(string1, string1.utf16_view());
            EXPECT_EQ(string1, string2.utf16_view());
            EXPECT_EQ(string2, string1.utf16_view());
            EXPECT_EQ(string2, string2.utf16_view());

            EXPECT_EQ(string1.utf16_view(), string1);
            EXPECT_EQ(string1.utf16_view(), string2);
            EXPECT_EQ(string2.utf16_view(), string1);
            EXPECT_EQ(string2.utf16_view(), string2);
        }

        EXPECT_NE(string1, inequal_string);
        EXPECT_NE(string2, inequal_string);
        EXPECT_NE(inequal_string, string1);
        EXPECT_NE(inequal_string, string2);

        if (string1.has_long_utf16_storage()) {
            EXPECT_NE(string1, inequal_string.utf16_view());
            EXPECT_NE(string2, inequal_string.utf16_view());
            EXPECT_NE(inequal_string, string1.utf16_view());
            EXPECT_NE(inequal_string, string2.utf16_view());

            EXPECT_NE(string1.utf16_view(), inequal_string);
            EXPECT_NE(string2.utf16_view(), inequal_string);
            EXPECT_NE(inequal_string.utf16_view(), string1);
            EXPECT_NE(inequal_string.utf16_view(), string2);
        }
    };

    // Short (empty) ASCII string comparison.
    test(Utf16String {}, "hello"_utf16);

    // Short ASCII string comparison.
    test("hello"_utf16, "there"_utf16);

    // Short and long ASCII string comparison.
    test("hello"_utf16, "hello there general!"_utf16);

    // Long ASCII string comparison.
    test("hello there!"_utf16, "hello there general!"_utf16);

    // UTF-16 string comparison.
    test("😀"_utf16, "hello 😀"_utf16);

    // Short ASCII and UTF-16 string comparison.
    test("hello"_utf16, "😀"_utf16);

    // Short ASCII and UTF-16 string of same code unit length comparison.
    test("ab"_utf16, "😀"_utf16);

    // Long ASCII and UTF-16 string comparison.
    test("hello there general!"_utf16, "😀"_utf16);

    // Long ASCII and UTF-16 string of same code unit length comparison.
    test("ababababab"_utf16, "😀😀😀😀😀"_utf16);
}

TEST_CASE(equals_ascii)
{
    auto test = [](StringView ascii, Utf16String const& inequal_string) {
        auto string = Utf16String::from_utf8(ascii);

        EXPECT_EQ(ascii, string);
        EXPECT_EQ(string, ascii);

        EXPECT_NE(ascii, inequal_string);
        EXPECT_NE(inequal_string, ascii);
    };

    // Short (empty) ASCII string comparison.
    test({}, "hello"_utf16);

    // Short ASCII string comparison.
    test("hello"sv, "there"_utf16);

    // Short and long ASCII string comparison.
    test("hello"sv, "hello there general!"_utf16);

    // Long ASCII string comparison.
    test("hello there!"sv, "hello there general!"_utf16);

    // Short ASCII and UTF-16 string comparison.
    test("hello"sv, "😀"_utf16);

    // Short ASCII and UTF-16 string of same code unit length comparison.
    test("ab"sv, "😀"_utf16);

    // Long ASCII and UTF-16 string comparison.
    test("hello there general!"sv, "😀"_utf16);

    // Long ASCII and UTF-16 string of same code unit length comparison.
    test("ababababab"sv, "😀😀😀😀😀"_utf16);

    // Non-ASCII string comparison.
    EXPECT_NE("😀"sv, "😀"_utf16);
}

TEST_CASE(equals_ignoring_ascii_case)
{
    auto test = [](Utf16String const& string1, Utf16String const& inequal_string) {
        StringBuilder builder;
        for (auto [i, code_point] : enumerate(string1))
            builder.append_code_point(i % 2 == 0 ? to_ascii_uppercase(code_point) : code_point);

        auto string2 = Utf16String::from_utf8(builder.string_view());

        EXPECT(string1.equals_ignoring_ascii_case(string1));
        EXPECT(string1.equals_ignoring_ascii_case(string2));
        EXPECT(string2.equals_ignoring_ascii_case(string1));
        EXPECT(string2.equals_ignoring_ascii_case(string2));

        if (string1.has_long_utf16_storage()) {
            EXPECT(string1.equals_ignoring_ascii_case(string1.utf16_view()));
            EXPECT(string1.equals_ignoring_ascii_case(string2.utf16_view()));
            EXPECT(string2.equals_ignoring_ascii_case(string1.utf16_view()));
            EXPECT(string2.equals_ignoring_ascii_case(string2.utf16_view()));
        }

        EXPECT(!string1.equals_ignoring_ascii_case(inequal_string));
        EXPECT(!string2.equals_ignoring_ascii_case(inequal_string));
        EXPECT(!inequal_string.equals_ignoring_ascii_case(string1));
        EXPECT(!inequal_string.equals_ignoring_ascii_case(string2));

        if (string1.has_long_utf16_storage()) {
            EXPECT(!string1.equals_ignoring_ascii_case(inequal_string.utf16_view()));
            EXPECT(!string2.equals_ignoring_ascii_case(inequal_string.utf16_view()));
            EXPECT(!inequal_string.equals_ignoring_ascii_case(string1.utf16_view()));
            EXPECT(!inequal_string.equals_ignoring_ascii_case(string2.utf16_view()));
        }
    };

    // Short (empty) ASCII string comparison.
    test(Utf16String {}, "hello"_utf16);

    // Short ASCII string comparison.
    test("hello"_utf16, "there"_utf16);

    // Short and long ASCII string comparison.
    test("hello"_utf16, "hello there general!"_utf16);

    // Long ASCII string comparison.
    test("hello there!"_utf16, "hello there general!"_utf16);

    // UTF-16 string comparison.
    test("😀"_utf16, "hello 😀"_utf16);

    // Short ASCII and UTF-16 string comparison.
    test("hello"_utf16, "😀"_utf16);

    // Short ASCII and UTF-16 string of same code unit length comparison.
    test("ab"_utf16, "😀"_utf16);

    // Long ASCII and UTF-16 string comparison.
    test("hello there general!"_utf16, "😀"_utf16);

    // Long ASCII and UTF-16 string of same code unit length comparison.
    test("ababababab"_utf16, "😀😀😀😀😀"_utf16);
}

TEST_CASE(iteration)
{
    auto test = [](Utf16String const& string, ReadonlySpan<u32> code_points) {
        EXPECT_EQ(string.length_in_code_points(), code_points.size());

        for (auto [i, code_point] : enumerate(string)) {
            if (code_points.size() == 0)
                FAIL("Iterating an empty UTF-16 string should not produce any values");
            else
                EXPECT_EQ(code_point, code_points[i]);
        }

        auto iterator = string.end();
        EXPECT_DEATH("Dereferencing a UTF-16 iterator which is at its end", *iterator);
        EXPECT_DEATH("Incrementing a UTF-16 iterator which is at its end", ++iterator);
    };

    test({}, {});
    test("hello"_utf16, { { 'h', 'e', 'l', 'l', 'o' } });
    test("hello there general!"_utf16, { { 'h', 'e', 'l', 'l', 'o', ' ', 't', 'h', 'e', 'r', 'e', ' ', 'g', 'e', 'n', 'e', 'r', 'a', 'l', '!' } });
    test("😀"_utf16, { { 0x1f600 } });
    test("hello 😀 there!"_utf16, { { 'h', 'e', 'l', 'l', 'o', ' ', 0x1f600, ' ', 't', 'h', 'e', 'r', 'e', '!' } });
}

TEST_CASE(code_unit_at)
{
    auto test = [](Utf16View const& view, size_t length_in_code_units) {
        auto string = Utf16String::from_utf16(view);
        EXPECT_EQ(string.length_in_code_units(), length_in_code_units);

        for (size_t i = 0; i < length_in_code_units; ++i)
            EXPECT_EQ(string.code_unit_at(i), view.code_unit_at(i));
    };

    test({}, 0);
    test(u"hello"sv, 5);
    test(u"hello there general!"sv, 20);
    test(u"😀"sv, 2);
    test(u"hello 😀 there!"sv, 15);
}

TEST_CASE(code_point_at)
{
    auto test = [](Utf16View const& view, size_t length_in_code_points) {
        auto string = Utf16String::from_utf16(view);
        EXPECT_EQ(string.length_in_code_points(), length_in_code_points);

        for (size_t i = 0; i < string.length_in_code_units(); ++i)
            EXPECT_EQ(string.code_point_at(i), view.code_point_at(i));
    };

    test({}, 0);
    test(u"hello"sv, 5);
    test(u"hello there general!"sv, 20);
    test(u"😀"sv, 1);
    test(u"hello 😀 there!"sv, 14);
}
