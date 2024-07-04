/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This is included first on purpose. We specifically do not want LibTest to override VERIFY here so
// that we can actually test that some String factory methods cause a crash with invalid input.
#include <AK/String.h>

#include <LibTest/TestCase.h>

#include <AK/MemoryStream.h>
#include <AK/StringBuilder.h>
#include <AK/Try.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <ctype.h>

TEST_CASE(construct_empty)
{
    String empty;
    EXPECT(empty.is_empty());
    EXPECT_EQ(empty.bytes().size(), 0u);
    EXPECT_EQ(empty, ""sv);

    auto empty2 = ""_string;
    EXPECT(empty2.is_empty());
    EXPECT_EQ(empty, empty2);

    auto empty3 = MUST(String::from_utf8(""sv));
    EXPECT(empty3.is_empty());
    EXPECT_EQ(empty, empty3);
}

TEST_CASE(move_assignment)
{
    String string1 = "hello"_string;
    string1 = "friends!"_string;
    EXPECT_EQ(string1, "friends!"sv);
}

TEST_CASE(copy_assignment)
{
    auto test = [](auto string1, auto string2) {
        string1 = string2;
        EXPECT_EQ(string1, string2);
    };

    test(String {}, String {});
    test(String {}, "abc"_string);
    test(String {}, "long string"_string);

    test("abc"_string, String {});
    test("abc"_string, "abc"_string);
    test("abc"_string, "long string"_string);

    test("long string"_string, String {});
    test("long string"_string, "abc"_string);
    test("long string"_string, "long string"_string);
}

TEST_CASE(short_strings)
{
    /** NOTE: make sure that the test strings' first character has an even ASCII code.
     *        This is important for the odd pointer address checks (this is to
     *        test if the ShortString structs are endian agnostic). */
#ifdef AK_ARCH_64_BIT
    auto string1 = MUST(String::from_utf8("foo bar"sv));
    EXPECT_EQ(string1.is_short_string(), true);
    EXPECT_EQ(string1.bytes().size(), 7u);
    EXPECT_EQ(string1.bytes_as_string_view(), "foo bar"sv);
    // check for odd "pointer" value, i.e. short string flag
    EXPECT_EQ(*((uintptr_t*)&string1) % 2UL, 1U);

    auto string2 = "foo bar"_string;
    EXPECT_EQ(string2.is_short_string(), true);
    EXPECT_EQ(string2.bytes().size(), 7u);
    EXPECT_EQ(string2, string1);
    // check for odd "pointer" value, i.e. short string flag
    EXPECT_EQ(*((uintptr_t*)&string2) % 2UL, 1U);
#else
    auto string1 = MUST(String::from_utf8("foo"sv));
    EXPECT_EQ(string1.is_short_string(), true);
    EXPECT_EQ(string1.bytes().size(), 3u);
    EXPECT_EQ(string1.bytes_as_string_view(), "foo"sv);
    // check for odd "pointer" value, i.e. short string flag
    EXPECT_EQ(*((uintptr_t*)&string1) % 2U, 1U);

    auto string2 = "foo"_string;
    EXPECT_EQ(string2.is_short_string(), true);
    EXPECT_EQ(string2.bytes().size(), 3u);
    EXPECT_EQ(string2, string1);
    // check for odd "pointer" value, i.e. short string flag
    EXPECT_EQ(*((uintptr_t*)&string2) % 2U, 1U);
#endif
}

TEST_CASE(long_strings)
{
    auto string = MUST(String::from_utf8("abcdefgh"sv));
    EXPECT_EQ(string.is_short_string(), false);
    EXPECT_EQ(string.bytes().size(), 8u);
    EXPECT_EQ(string.bytes_as_string_view(), "abcdefgh"sv);
}

TEST_CASE(long_streams)
{
    {
        u8 bytes[64] = {};
        constexpr auto test_view = "Well, hello friends"sv;
        FixedMemoryStream stream(Bytes { bytes, sizeof(bytes) });
        MUST(stream.write_until_depleted(test_view.bytes()));
        MUST(stream.seek(0));

        auto string = MUST(String::from_stream(stream, test_view.length()));

        EXPECT_EQ(string.is_short_string(), false);
        EXPECT_EQ(string.bytes().size(), 19u);
        EXPECT_EQ(string.bytes_as_string_view(), test_view);
    }

    {
        AllocatingMemoryStream stream;
        MUST(stream.write_until_depleted(("abc"sv).bytes()));

        auto string = MUST(String::from_stream(stream, 3u));

        EXPECT_EQ(string.is_short_string(), true);
        EXPECT_EQ(string.bytes().size(), 3u);
        EXPECT_EQ(string.bytes_as_string_view(), "abc"sv);
    }

    {
        AllocatingMemoryStream stream;
        MUST(stream.write_until_depleted(("0123456789"sv).bytes()));

        auto string = MUST(String::from_stream(stream, 9u));

        EXPECT_EQ(string.is_short_string(), false);
        EXPECT_EQ(string.bytes().size(), 9u);
        EXPECT_EQ(string.bytes_as_string_view(), "012345678"sv);
    }

    {
        AllocatingMemoryStream stream;
        MUST(stream.write_value(0xffffffff));
        MUST(stream.write_value(0xffffffff));
        MUST(stream.write_value(0xffffffff));
        auto error_or_string = String::from_stream(stream, stream.used_buffer_size());
        EXPECT_EQ(error_or_string.is_error(), true);
    }
}

TEST_CASE(invalid_utf8)
{
    auto string1 = String::from_utf8("long string \xf4\x8f\xbf\xc0"sv); // U+110000
    EXPECT(string1.is_error());
    EXPECT(string1.error().string_literal().contains("Input was not valid UTF-8"sv));

    auto string2 = String::from_utf8("\xf4\xa1\xb0\xbd"sv); // U+121C3D
    EXPECT(string2.is_error());
    EXPECT(string2.error().string_literal().contains("Input was not valid UTF-8"sv));

    AllocatingMemoryStream stream;
    MUST(stream.write_value<u8>(0xf4));
    MUST(stream.write_value<u8>(0xa1));
    MUST(stream.write_value<u8>(0xb0));
    MUST(stream.write_value<u8>(0xbd));
    auto string3 = String::from_stream(stream, stream.used_buffer_size());
    EXPECT_EQ(string3.is_error(), true);
    EXPECT(string3.error().string_literal().contains("Input was not valid UTF-8"sv));
}

TEST_CASE(from_code_points)
{
    for (u32 code_point = 0; code_point < 0x80; ++code_point) {
        auto string = String::from_code_point(code_point);

        auto ch = static_cast<char>(code_point);
        StringView view { &ch, 1 };

        EXPECT_EQ(string, view);
    }

    auto string = String::from_code_point(0x10ffff);
    EXPECT_EQ(string, "\xF4\x8F\xBF\xBF"sv);

    EXPECT_CRASH("Creating a string from an invalid code point", [] {
        String::from_code_point(0xffffffff);
        return Test::Crash::Failure::DidNotCrash;
    });
}

TEST_CASE(substring)
{
    auto superstring = "Hello I am a long string"_string;
    auto short_substring = MUST(superstring.substring_from_byte_offset(0, 5));
    EXPECT_EQ(short_substring, "Hello"sv);

    auto long_substring = MUST(superstring.substring_from_byte_offset(0, 10));
    EXPECT_EQ(long_substring, "Hello I am"sv);
}

TEST_CASE(substring_with_shared_superstring)
{
    auto superstring = "Hello I am a long string"_string;

    auto substring1 = MUST(superstring.substring_from_byte_offset_with_shared_superstring(0, 5));
    EXPECT_EQ(substring1, "Hello"sv);

    auto substring2 = MUST(superstring.substring_from_byte_offset_with_shared_superstring(0, 10));
    EXPECT_EQ(substring2, "Hello I am"sv);
}

TEST_CASE(code_points)
{
    auto string = "🦬🪒"_string;

    Vector<u32> code_points;
    for (auto code_point : string.code_points())
        code_points.append(code_point);

    EXPECT_EQ(code_points[0], 0x1f9acu);
    EXPECT_EQ(code_points[1], 0x1fa92u);
}

TEST_CASE(string_builder)
{
    StringBuilder builder;
    builder.append_code_point(0x1f9acu);
    builder.append_code_point(0x1fa92u);

    auto string = MUST(builder.to_string());
    EXPECT_EQ(string, "🦬🪒"sv);
    EXPECT_EQ(string.bytes().size(), 8u);
}

TEST_CASE(ak_format)
{
    auto foo = MUST(String::formatted("Hello {}", "friends"_string));
    EXPECT_EQ(foo, "Hello friends"sv);
}

TEST_CASE(replace)
{
    {
        auto haystack = "Hello enemies"_string;
        auto result = MUST(haystack.replace("enemies"sv, "friends"sv, ReplaceMode::All));
        EXPECT_EQ(result, "Hello friends"sv);
    }

    {
        auto base_title = "anon@courage:~"_string;
        auto result = MUST(base_title.replace("[*]"sv, "(*)"sv, ReplaceMode::FirstOnly));
        EXPECT_EQ(result, "anon@courage:~"sv);
    }
}

TEST_CASE(reverse)
{
    auto test_reverse = [](auto test, auto expected) {
        auto string = MUST(String::from_utf8(test));
        auto result = MUST(string.reverse());

        EXPECT_EQ(result, expected);
    };

    test_reverse(""sv, ""sv);
    test_reverse("a"sv, "a"sv);
    test_reverse("ab"sv, "ba"sv);
    test_reverse("ab cd ef"sv, "fe dc ba"sv);
    test_reverse("😀"sv, "😀"sv);
    test_reverse("ab😀cd"sv, "dc😀ba"sv);
}

TEST_CASE(to_lowercase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = MUST("\u00DF"_string.to_lowercase());
    EXPECT_EQ(result, "\u00DF");

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = MUST("\u0130"_string.to_lowercase());
    EXPECT_EQ(result, "\u0069\u0307");

    // LATIN SMALL LIGATURE FF
    result = MUST("\uFB00"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB00");

    // LATIN SMALL LIGATURE FI
    result = MUST("\uFB01"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB01");

    // LATIN SMALL LIGATURE FL
    result = MUST("\uFB02"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB02");

    // LATIN SMALL LIGATURE FFI
    result = MUST("\uFB03"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB03");

    // LATIN SMALL LIGATURE FFL
    result = MUST("\uFB04"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB04");

    // LATIN SMALL LIGATURE LONG S T
    result = MUST("\uFB05"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB05");

    // LATIN SMALL LIGATURE ST
    result = MUST("\uFB06"_string.to_lowercase());
    EXPECT_EQ(result, "\uFB06");

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FB7"_string.to_lowercase());
    EXPECT_EQ(result, "\u1FB7");

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FC7"_string.to_lowercase());
    EXPECT_EQ(result, "\u1FC7");

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FF7"_string.to_lowercase());
    EXPECT_EQ(result, "\u1FF7");
}

TEST_CASE(to_lowercase_special_casing_sigma)
{
    auto result = MUST("ABCI"_string.to_lowercase());
    EXPECT_EQ(result, "abci");

    // Sigma preceded by A
    result = MUST("A\u03A3"_string.to_lowercase());
    EXPECT_EQ(result, "a\u03C2");

    // Sigma preceded by FEMININE ORDINAL INDICATOR
    result = MUST("\u00AA\u03A3"_string.to_lowercase());
    EXPECT_EQ(result, "\u00AA\u03C2");

    // Sigma preceded by ROMAN NUMERAL ONE
    result = MUST("\u2160\u03A3"_string.to_lowercase());
    EXPECT_EQ(result, "\u2170\u03C2");

    // Sigma preceded by COMBINING GREEK YPOGEGRAMMENI
    result = MUST("\u0345\u03A3"_string.to_lowercase());
    EXPECT_EQ(result, "\u0345\u03C3");

    // Sigma preceded by A and FULL STOP
    result = MUST("A.\u03A3"_string.to_lowercase());
    EXPECT_EQ(result, "a.\u03C2");

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR
    result = MUST("A\u180E\u03A3"_string.to_lowercase());
    EXPECT_EQ(result, "a\u180E\u03C2");

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR, followed by B
    result = MUST("A\u180E\u03A3B"_string.to_lowercase());
    EXPECT_EQ(result, "a\u180E\u03C3b");

    // Sigma followed by A
    result = MUST("\u03A3A"_string.to_lowercase());
    EXPECT_EQ(result, "\u03C3a");

    // Sigma preceded by A, followed by MONGOLIAN VOWEL SEPARATOR
    result = MUST("A\u03A3\u180E"_string.to_lowercase());
    EXPECT_EQ(result, "a\u03C2\u180E");

    // Sigma preceded by A, followed by MONGOLIAN VOWEL SEPARATOR and B
    result = MUST("A\u03A3\u180EB"_string.to_lowercase());
    EXPECT_EQ(result, "a\u03C3\u180Eb");

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR, followed by MONGOLIAN VOWEL SEPARATOR
    result = MUST("A\u180E\u03A3\u180E"_string.to_lowercase());
    EXPECT_EQ(result, "a\u180E\u03C2\u180E");

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR, followed by MONGOLIAN VOWEL SEPARATOR and B
    result = MUST("A\u180E\u03A3\u180EB"_string.to_lowercase());
    EXPECT_EQ(result, "a\u180E\u03C3\u180Eb");
}

TEST_CASE(to_lowercase_special_casing_i)
{
    // LATIN CAPITAL LETTER I
    auto result = MUST("I"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "i"sv);

    result = MUST("I"_string.to_lowercase("az"sv));
    EXPECT_EQ(result, "\u0131"sv);

    result = MUST("I"_string.to_lowercase("tr"sv));
    EXPECT_EQ(result, "\u0131"sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = MUST("\u0130"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "\u0069\u0307"sv);

    result = MUST("\u0130"_string.to_lowercase("az"sv));
    EXPECT_EQ(result, "i"sv);

    result = MUST("\u0130"_string.to_lowercase("tr"sv));
    EXPECT_EQ(result, "i"sv);

    // LATIN CAPITAL LETTER I followed by COMBINING DOT ABOVE
    result = MUST("I\u0307"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "i\u0307"sv);

    result = MUST("I\u0307"_string.to_lowercase("az"sv));
    EXPECT_EQ(result, "i"sv);

    result = MUST("I\u0307"_string.to_lowercase("tr"sv));
    EXPECT_EQ(result, "i"sv);

    // LATIN CAPITAL LETTER I followed by combining class 0 and COMBINING DOT ABOVE
    result = MUST("IA\u0307"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "ia\u0307"sv);

    result = MUST("IA\u0307"_string.to_lowercase("az"sv));
    EXPECT_EQ(result, "\u0131a\u0307"sv);

    result = MUST("IA\u0307"_string.to_lowercase("tr"sv));
    EXPECT_EQ(result, "\u0131a\u0307"sv);
}

TEST_CASE(to_lowercase_special_casing_more_above)
{
    // LATIN CAPITAL LETTER I
    auto result = MUST("I"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "i"sv);

    result = MUST("I"_string.to_lowercase("lt"sv));
    EXPECT_EQ(result, "i"sv);

    // LATIN CAPITAL LETTER J
    result = MUST("J"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "j"sv);

    result = MUST("J"_string.to_lowercase("lt"sv));
    EXPECT_EQ(result, "j"sv);

    // LATIN CAPITAL LETTER I WITH OGONEK
    result = MUST("\u012e"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "\u012f"sv);

    result = MUST("\u012e"_string.to_lowercase("lt"sv));
    EXPECT_EQ(result, "\u012f"sv);

    // LATIN CAPITAL LETTER I followed by COMBINING GRAVE ACCENT
    result = MUST("I\u0300"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "i\u0300"sv);

    result = MUST("I\u0300"_string.to_lowercase("lt"sv));
    EXPECT_EQ(result, "i\u0307\u0300"sv);

    // LATIN CAPITAL LETTER J followed by COMBINING GRAVE ACCENT
    result = MUST("J\u0300"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "j\u0300"sv);

    result = MUST("J\u0300"_string.to_lowercase("lt"sv));
    EXPECT_EQ(result, "j\u0307\u0300"sv);

    // LATIN CAPITAL LETTER I WITH OGONEK followed by COMBINING GRAVE ACCENT
    result = MUST("\u012e\u0300"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "\u012f\u0300"sv);

    result = MUST("\u012e\u0300"_string.to_lowercase("lt"sv));
    EXPECT_EQ(result, "\u012f\u0307\u0300"sv);
}

TEST_CASE(to_lowercase_special_casing_not_before_dot)
{
    // LATIN CAPITAL LETTER I
    auto result = MUST("I"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "i"sv);

    result = MUST("I"_string.to_lowercase("az"sv));
    EXPECT_EQ(result, "\u0131"sv);

    result = MUST("I"_string.to_lowercase("tr"sv));
    EXPECT_EQ(result, "\u0131"sv);

    // LATIN CAPITAL LETTER I followed by COMBINING DOT ABOVE
    result = MUST("I\u0307"_string.to_lowercase("en"sv));
    EXPECT_EQ(result, "i\u0307"sv);

    result = MUST("I\u0307"_string.to_lowercase("az"sv));
    EXPECT_EQ(result, "i"sv);

    result = MUST("I\u0307"_string.to_lowercase("tr"sv));
    EXPECT_EQ(result, "i"sv);
}

TEST_CASE(to_uppercase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = MUST("\u00DF"_string.to_uppercase());
    EXPECT_EQ(result, "\u0053\u0053");

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = MUST("\u0130"_string.to_uppercase());
    EXPECT_EQ(result, "\u0130");

    // LATIN SMALL LIGATURE FF
    result = MUST("\uFB00"_string.to_uppercase());
    EXPECT_EQ(result, "\u0046\u0046");

    // LATIN SMALL LIGATURE FI
    result = MUST("\uFB01"_string.to_uppercase());
    EXPECT_EQ(result, "\u0046\u0049");

    // LATIN SMALL LIGATURE FL
    result = MUST("\uFB02"_string.to_uppercase());
    EXPECT_EQ(result, "\u0046\u004C");

    // LATIN SMALL LIGATURE FFI
    result = MUST("\uFB03"_string.to_uppercase());
    EXPECT_EQ(result, "\u0046\u0046\u0049");

    // LATIN SMALL LIGATURE FFL
    result = MUST("\uFB04"_string.to_uppercase());
    EXPECT_EQ(result, "\u0046\u0046\u004C");

    // LATIN SMALL LIGATURE LONG S T
    result = MUST("\uFB05"_string.to_uppercase());
    EXPECT_EQ(result, "\u0053\u0054");

    // LATIN SMALL LIGATURE ST
    result = MUST("\uFB06"_string.to_uppercase());
    EXPECT_EQ(result, "\u0053\u0054");

    // GREEK SMALL LETTER IOTA WITH DIALYTIKA AND TONOS
    result = MUST("\u0390"_string.to_uppercase());
    EXPECT_EQ(result, "\u0399\u0308\u0301");

    // GREEK SMALL LETTER UPSILON WITH DIALYTIKA AND TONOS
    result = MUST("\u03B0"_string.to_uppercase());
    EXPECT_EQ(result, "\u03A5\u0308\u0301");

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FB7"_string.to_uppercase());
    EXPECT_EQ(result, "\u0391\u0342\u0399");

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FC7"_string.to_uppercase());
    EXPECT_EQ(result, "\u0397\u0342\u0399");

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FF7"_string.to_uppercase());
    EXPECT_EQ(result, "\u03A9\u0342\u0399");
}

TEST_CASE(to_uppercase_special_casing_soft_dotted)
{
    // LATIN SMALL LETTER I
    auto result = MUST("i"_string.to_uppercase("en"sv));
    EXPECT_EQ(result, "I"sv);

    result = MUST("i"_string.to_uppercase("lt"sv));
    EXPECT_EQ(result, "I"sv);

    // LATIN SMALL LETTER J
    result = MUST("j"_string.to_uppercase("en"sv));
    EXPECT_EQ(result, "J"sv);

    result = MUST("j"_string.to_uppercase("lt"sv));
    EXPECT_EQ(result, "J"sv);

    // LATIN SMALL LETTER I followed by COMBINING DOT ABOVE
    result = MUST("i\u0307"_string.to_uppercase("en"sv));
    EXPECT_EQ(result, "I\u0307"sv);

    result = MUST("i\u0307"_string.to_uppercase("lt"sv));
    EXPECT_EQ(result, "I"sv);

    // LATIN SMALL LETTER J followed by COMBINING DOT ABOVE
    result = MUST("j\u0307"_string.to_uppercase("en"sv));
    EXPECT_EQ(result, "J\u0307"sv);

    result = MUST("j\u0307"_string.to_uppercase("lt"sv));
    EXPECT_EQ(result, "J"sv);
}

TEST_CASE(to_titlecase)
{
    EXPECT_EQ(MUST(""_string.to_titlecase()), ""sv);
    EXPECT_EQ(MUST(" "_string.to_titlecase()), " "sv);
    EXPECT_EQ(MUST(" - "_string.to_titlecase()), " - "sv);

    EXPECT_EQ(MUST("a"_string.to_titlecase()), "A"sv);
    EXPECT_EQ(MUST("A"_string.to_titlecase()), "A"sv);
    EXPECT_EQ(MUST(" a"_string.to_titlecase()), " A"sv);
    EXPECT_EQ(MUST("a "_string.to_titlecase()), "A "sv);

    EXPECT_EQ(MUST("ab"_string.to_titlecase()), "Ab"sv);
    EXPECT_EQ(MUST("Ab"_string.to_titlecase()), "Ab"sv);
    EXPECT_EQ(MUST("aB"_string.to_titlecase()), "Ab"sv);
    EXPECT_EQ(MUST("AB"_string.to_titlecase()), "Ab"sv);
    EXPECT_EQ(MUST(" ab"_string.to_titlecase()), " Ab"sv);
    EXPECT_EQ(MUST("ab "_string.to_titlecase()), "Ab "sv);

    EXPECT_EQ(MUST("foo bar baz"_string.to_titlecase()), "Foo Bar Baz"sv);
    EXPECT_EQ(MUST("foo \n \r bar \t baz"_string.to_titlecase()), "Foo \n \r Bar \t Baz"sv);
    EXPECT_EQ(MUST("f\"oo\" b'ar'"_string.to_titlecase()), "F\"Oo\" B'ar'"sv);
}

TEST_CASE(to_casefold)
{
    for (u8 code_point = 0; code_point < 0x80; ++code_point) {
        auto ascii = tolower(code_point);
        auto unicode = MUST(MUST(String::from_utf8({ reinterpret_cast<char const*>(&code_point), 1 })).to_casefold());

        EXPECT_EQ(unicode.bytes_as_string_view().length(), 1u);
        EXPECT_EQ(unicode.bytes_as_string_view()[0], ascii);
    }

    // LATIN SMALL LETTER SHARP S
    auto result = MUST("\u00DF"_string.to_casefold());
    EXPECT_EQ(result, "\u0073\u0073"sv);

    // GREEK SMALL LETTER ALPHA WITH YPOGEGRAMMENI
    result = MUST("\u1FB3"_string.to_casefold());
    EXPECT_EQ(result, "\u03B1\u03B9"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI
    result = MUST("\u1FB6"_string.to_casefold());
    EXPECT_EQ(result, "\u03B1\u0342"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FB7"_string.to_casefold());
    EXPECT_EQ(result, "\u03B1\u0342\u03B9"sv);
}

TEST_CASE(to_titlecase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = MUST("\u00DF"_string.to_titlecase());
    EXPECT_EQ(result, "\u0053\u0073"sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = MUST("\u0130"_string.to_titlecase());
    EXPECT_EQ(result, "\u0130"sv);

    // LATIN SMALL LIGATURE FF
    result = MUST("\uFB00"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0066"sv);

    // LATIN SMALL LIGATURE FI
    result = MUST("\uFB01"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0069"sv);

    // LATIN SMALL LIGATURE FL
    result = MUST("\uFB02"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u006C"sv);

    // LATIN SMALL LIGATURE FFI
    result = MUST("\uFB03"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0066\u0069"sv);

    // LATIN SMALL LIGATURE FFL
    result = MUST("\uFB04"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0066\u006C"sv);

    // LATIN SMALL LIGATURE LONG S T
    result = MUST("\uFB05"_string.to_titlecase());
    EXPECT_EQ(result, "\u0053\u0074"sv);

    // LATIN SMALL LIGATURE ST
    result = MUST("\uFB06"_string.to_titlecase());
    EXPECT_EQ(result, "\u0053\u0074"sv);

    // GREEK SMALL LETTER IOTA WITH DIALYTIKA AND TONOS
    result = MUST("\u0390"_string.to_titlecase());
    EXPECT_EQ(result, "\u0399\u0308\u0301"sv);

    // GREEK SMALL LETTER UPSILON WITH DIALYTIKA AND TONOS
    result = MUST("\u03B0"_string.to_titlecase());
    EXPECT_EQ(result, "\u03A5\u0308\u0301"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FB7"_string.to_titlecase());
    EXPECT_EQ(result, "\u0391\u0342\u0345"sv);

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FC7"_string.to_titlecase());
    EXPECT_EQ(result, "\u0397\u0342\u0345"sv);

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FF7"_string.to_titlecase());
    EXPECT_EQ(result, "\u03A9\u0342\u0345"sv);
}

TEST_CASE(to_titlecase_special_casing_i)
{
    // LATIN SMALL LETTER I
    auto result = MUST("i"_string.to_titlecase("en"sv));
    EXPECT_EQ(result, "I"sv);

    result = MUST("i"_string.to_titlecase("az"sv));
    EXPECT_EQ(result, "\u0130"sv);

    result = MUST("i"_string.to_titlecase("tr"sv));
    EXPECT_EQ(result, "\u0130"sv);
}

BENCHMARK_CASE(casefold)
{
    for (size_t i = 0; i < 50'000; ++i) {
        __test_to_casefold();
    }
}

TEST_CASE(equals_ignoring_case)
{
    {
        String string1 {};
        String string2 {};

        EXPECT(string1.equals_ignoring_case(string2));
    }
    {
        auto string1 = "abcd"_string;
        auto string2 = "ABCD"_string;
        auto string3 = "AbCd"_string;
        auto string4 = "dcba"_string;
        auto string5 = "abce"_string;
        auto string6 = "abc"_string;

        EXPECT(string1.equals_ignoring_case(string2));
        EXPECT(string1.equals_ignoring_case(string3));
        EXPECT(!string1.equals_ignoring_case(string4));
        EXPECT(!string1.equals_ignoring_case(string5));
        EXPECT(!string1.equals_ignoring_case(string6));

        EXPECT(string2.equals_ignoring_case(string1));
        EXPECT(string2.equals_ignoring_case(string3));
        EXPECT(!string2.equals_ignoring_case(string4));
        EXPECT(!string2.equals_ignoring_case(string5));
        EXPECT(!string2.equals_ignoring_case(string6));

        EXPECT(string3.equals_ignoring_case(string1));
        EXPECT(string3.equals_ignoring_case(string2));
        EXPECT(!string3.equals_ignoring_case(string4));
        EXPECT(!string3.equals_ignoring_case(string5));
        EXPECT(!string3.equals_ignoring_case(string6));
    }
    {
        auto string1 = "\u00DF"_string; // LATIN SMALL LETTER SHARP S
        auto string2 = "SS"_string;
        auto string3 = "Ss"_string;
        auto string4 = "ss"_string;
        auto string5 = "S"_string;
        auto string6 = "s"_string;

        EXPECT(string1.equals_ignoring_case(string2));
        EXPECT(string1.equals_ignoring_case(string3));
        EXPECT(string1.equals_ignoring_case(string4));
        EXPECT(!string1.equals_ignoring_case(string5));
        EXPECT(!string1.equals_ignoring_case(string6));

        EXPECT(string2.equals_ignoring_case(string1));
        EXPECT(string2.equals_ignoring_case(string3));
        EXPECT(string2.equals_ignoring_case(string4));
        EXPECT(!string2.equals_ignoring_case(string5));
        EXPECT(!string2.equals_ignoring_case(string6));

        EXPECT(string3.equals_ignoring_case(string1));
        EXPECT(string3.equals_ignoring_case(string2));
        EXPECT(string3.equals_ignoring_case(string4));
        EXPECT(!string3.equals_ignoring_case(string5));
        EXPECT(!string3.equals_ignoring_case(string6));

        EXPECT(string4.equals_ignoring_case(string1));
        EXPECT(string4.equals_ignoring_case(string2));
        EXPECT(string4.equals_ignoring_case(string3));
        EXPECT(!string4.equals_ignoring_case(string5));
        EXPECT(!string4.equals_ignoring_case(string6));
    }
    {

        auto string1 = "Ab\u00DFCd\u00DFeF"_string;
        auto string2 = "ABSSCDSSEF"_string;
        auto string3 = "absscdssef"_string;
        auto string4 = "aBSscDsSEf"_string;
        auto string5 = "Ab\u00DFCd\u00DFeg"_string;
        auto string6 = "Ab\u00DFCd\u00DFe"_string;

        EXPECT(string1.equals_ignoring_case(string1));
        EXPECT(string1.equals_ignoring_case(string2));
        EXPECT(string1.equals_ignoring_case(string3));
        EXPECT(string1.equals_ignoring_case(string4));
        EXPECT(!string1.equals_ignoring_case(string5));
        EXPECT(!string1.equals_ignoring_case(string6));

        EXPECT(string2.equals_ignoring_case(string1));
        EXPECT(string2.equals_ignoring_case(string2));
        EXPECT(string2.equals_ignoring_case(string3));
        EXPECT(string2.equals_ignoring_case(string4));
        EXPECT(!string2.equals_ignoring_case(string5));
        EXPECT(!string2.equals_ignoring_case(string6));

        EXPECT(string3.equals_ignoring_case(string1));
        EXPECT(string3.equals_ignoring_case(string2));
        EXPECT(string3.equals_ignoring_case(string3));
        EXPECT(string3.equals_ignoring_case(string4));
        EXPECT(!string3.equals_ignoring_case(string5));
        EXPECT(!string3.equals_ignoring_case(string6));

        EXPECT(string4.equals_ignoring_case(string1));
        EXPECT(string4.equals_ignoring_case(string2));
        EXPECT(string4.equals_ignoring_case(string3));
        EXPECT(string4.equals_ignoring_case(string4));
        EXPECT(!string4.equals_ignoring_case(string5));
        EXPECT(!string4.equals_ignoring_case(string6));
    }
}

TEST_CASE(is_one_of)
{
    auto foo = "foo"_string;
    auto bar = "bar"_string;

    EXPECT(foo.is_one_of(foo));
    EXPECT(foo.is_one_of(foo, bar));
    EXPECT(foo.is_one_of(bar, foo));
    EXPECT(!foo.is_one_of(bar));

    EXPECT(!bar.is_one_of("foo"sv));
    EXPECT(bar.is_one_of("foo"sv, "bar"sv));
    EXPECT(bar.is_one_of("bar"sv, "foo"sv));
    EXPECT(bar.is_one_of("bar"sv));
}

TEST_CASE(split)
{
    {
        auto test = "foo bar baz"_string;
        auto parts = MUST(test.split(' '));
        EXPECT_EQ(parts.size(), 3u);
        EXPECT_EQ(parts[0], "foo");
        EXPECT_EQ(parts[1], "bar");
        EXPECT_EQ(parts[2], "baz");
    }
    {
        auto test = "ωΣ2ωΣω"_string;
        auto parts = MUST(test.split(0x03A3u));
        EXPECT_EQ(parts.size(), 3u);
        EXPECT_EQ(parts[0], "ω"sv);
        EXPECT_EQ(parts[1], "2ω"sv);
        EXPECT_EQ(parts[2], "ω"sv);
    }
}

TEST_CASE(find_byte_offset)
{
    {
        String string {};
        auto index1 = string.find_byte_offset(0);
        EXPECT(!index1.has_value());

        auto index2 = string.find_byte_offset(""sv);
        EXPECT(!index2.has_value());
    }
    {
        auto string = "foo"_string;

        auto index1 = string.find_byte_offset('f');
        EXPECT_EQ(index1, 0u);

        auto index2 = string.find_byte_offset('o');
        EXPECT_EQ(index2, 1u);

        auto index3 = string.find_byte_offset('o', *index2 + 1);
        EXPECT_EQ(index3, 2u);

        auto index4 = string.find_byte_offset('b');
        EXPECT(!index4.has_value());
    }
    {
        auto string = "foo"_string;

        auto index1 = string.find_byte_offset("fo"sv);
        EXPECT_EQ(index1, 0u);

        auto index2 = string.find_byte_offset("oo"sv);
        EXPECT_EQ(index2, 1u);

        auto index3 = string.find_byte_offset("o"sv, *index2 + 1);
        EXPECT_EQ(index3, 2u);

        auto index4 = string.find_byte_offset("fooo"sv);
        EXPECT(!index4.has_value());
    }
    {
        auto string = "ωΣωΣω"_string;

        auto index1 = string.find_byte_offset(0x03C9U);
        EXPECT_EQ(index1, 0u);

        auto index2 = string.find_byte_offset(0x03A3u);
        EXPECT_EQ(index2, 2u);

        auto index3 = string.find_byte_offset(0x03C9U, 2);
        EXPECT_EQ(index3, 4u);

        auto index4 = string.find_byte_offset(0x03A3u, 4);
        EXPECT_EQ(index4, 6u);

        auto index5 = string.find_byte_offset(0x03C9U, 6);
        EXPECT_EQ(index5, 8u);
    }
    {
        auto string = "ωΣωΣω"_string;

        auto index1 = string.find_byte_offset("ω"sv);
        EXPECT_EQ(index1, 0u);

        auto index2 = string.find_byte_offset("Σ"sv);
        EXPECT_EQ(index2, 2u);

        auto index3 = string.find_byte_offset("ω"sv, 2);
        EXPECT_EQ(index3, 4u);

        auto index4 = string.find_byte_offset("Σ"sv, 4);
        EXPECT_EQ(index4, 6u);

        auto index5 = string.find_byte_offset("ω"sv, 6);
        EXPECT_EQ(index5, 8u);
    }
}

TEST_CASE(find_byte_offset_ignoring_case)
{
    {
        auto string = ""_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case(""sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("1"sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("2"sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("23"sv).has_value(), false);
    }
    {
        auto string = "1234567"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case(""sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("1"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("2"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("3"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("4"sv), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("5"sv), 4u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("6"sv), 5u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("7"sv), 6u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("34"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("45"sv), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("56"sv), 4u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("67"sv), 5u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("a"sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("8"sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("78"sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("46"sv).has_value(), false);
    }
    {
        auto string = "abCDef"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("A"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("B"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("c"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("d"sv), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("e"sv), 4u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("f"sv), 5u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("AbC"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("BcdE"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("cd"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("cD"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("E"sv), 4u);
    }
    {
        auto string = "abßcd"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"sv), 2u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSS"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSs"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bss"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bS"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bs"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bß"sv), 1u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSSc"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSsc"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bssc"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bßc"sv), 1u);
        EXPECT(!string.find_byte_offset_ignoring_case("bSc"sv).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("bsc"sv).has_value());
    }
    {
        auto string = "abSScd"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"sv), 2u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSS"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSs"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bss"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bS"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bs"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bß"sv), 1u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSSc"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSsc"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bssc"sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bßc"sv), 1u);
        EXPECT(!string.find_byte_offset_ignoring_case("bSc"sv).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("bsc"sv).has_value());
    }
    {
        auto string = "ßSßs"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"sv), 0u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"sv, 2), 2u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"sv, 3), 3u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"sv, 5), 5u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"sv, 5), 5u);
        EXPECT(!string.find_byte_offset_ignoring_case("SS"sv, 5).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("Ss"sv, 5).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("ss"sv, 5).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("ß"sv, 5).has_value());
    }
}

TEST_CASE(repeated)
{
    {
        auto string1 = MUST(String::repeated('a', 0));
        EXPECT(string1.is_short_string());
        EXPECT(string1.is_empty());

        auto string2 = MUST(String::repeated(0x03C9U, 0));
        EXPECT(string2.is_short_string());
        EXPECT(string2.is_empty());

        auto string3 = MUST(String::repeated(0x10300, 0));
        EXPECT(string3.is_short_string());
        EXPECT(string3.is_empty());
    }
    {
        auto string1 = MUST(String::repeated('a', 1));
        EXPECT(string1.is_short_string());
        EXPECT_EQ(string1.bytes_as_string_view().length(), 1u);
        EXPECT_EQ(string1, "a"sv);

        auto string2 = MUST(String::repeated(0x03C9U, 1));
        EXPECT(string2.is_short_string());
        EXPECT_EQ(string2.bytes_as_string_view().length(), 2u);
        EXPECT_EQ(string2, "ω"sv);

        auto string3 = MUST(String::repeated(0x10300, 1));
#ifdef AK_ARCH_64_BIT
        EXPECT(string3.is_short_string());
#else
        EXPECT(!string3.is_short_string());
#endif
        EXPECT_EQ(string3.bytes_as_string_view().length(), 4u);
        EXPECT_EQ(string3, "𐌀"sv);
    }
    {
        auto string1 = MUST(String::repeated('a', 3));
        EXPECT(string1.is_short_string());
        EXPECT_EQ(string1.bytes_as_string_view().length(), 3u);
        EXPECT_EQ(string1, "aaa"sv);

        auto string2 = MUST(String::repeated(0x03C9U, 3));
#ifdef AK_ARCH_64_BIT
        EXPECT(string2.is_short_string());
#else
        EXPECT(!string2.is_short_string());
#endif
        EXPECT_EQ(string2.bytes_as_string_view().length(), 6u);
        EXPECT_EQ(string2, "ωωω"sv);

        auto string3 = MUST(String::repeated(0x10300, 3));
        EXPECT(!string3.is_short_string());
        EXPECT_EQ(string3.bytes_as_string_view().length(), 12u);
        EXPECT_EQ(string3, "𐌀𐌀𐌀"sv);
    }
    {
        auto string1 = MUST(String::repeated('a', 10));
        EXPECT(!string1.is_short_string());
        EXPECT_EQ(string1.bytes_as_string_view().length(), 10u);
        EXPECT_EQ(string1, "aaaaaaaaaa"sv);

        auto string2 = MUST(String::repeated(0x03C9U, 10));
        EXPECT(!string2.is_short_string());
        EXPECT_EQ(string2.bytes_as_string_view().length(), 20u);
        EXPECT_EQ(string2, "ωωωωωωωωωω"sv);

        auto string3 = MUST(String::repeated(0x10300, 10));
        EXPECT(!string3.is_short_string());
        EXPECT_EQ(string3.bytes_as_string_view().length(), 40u);
        EXPECT_EQ(string3, "𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀"sv);
    }

    EXPECT_CRASH("Creating a string from an invalid code point", [] {
        (void)String::repeated(0xffffffff, 1);
        return Test::Crash::Failure::DidNotCrash;
    });
}

TEST_CASE(join)
{
    auto string1 = MUST(String::join(',', Vector<i32> {}));
    EXPECT(string1.is_empty());

    auto string2 = MUST(String::join(',', Array { 1 }));
    EXPECT_EQ(string2, "1"sv);

    auto string3 = MUST(String::join(':', Array { 1 }, "[{}]"sv));
    EXPECT_EQ(string3, "[1]"sv);

    auto string4 = MUST(String::join(',', Array { 1, 2, 3 }));
    EXPECT_EQ(string4, "1,2,3"sv);

    auto string5 = MUST(String::join(',', Array { 1, 2, 3 }, "[{}]"sv));
    EXPECT_EQ(string5, "[1],[2],[3]"sv);

    auto string6 = MUST(String::join("!!!"_string, Array { "foo"sv, "bar"sv, "baz"sv }));
    EXPECT_EQ(string6, "foo!!!bar!!!baz"sv);

    auto string7 = MUST(String::join(" - "sv, Array { 1, 16, 256, 4096 }, "[{:#04x}]"sv));
    EXPECT_EQ(string7, "[0x0001] - [0x0010] - [0x0100] - [0x1000]"sv);
}

TEST_CASE(trim)
{
    {
        String string {};

        auto result = MUST(string.trim(" "sv, TrimMode::Both));
        EXPECT(result.is_empty());

        result = MUST(string.trim(" "sv, TrimMode::Left));
        EXPECT(result.is_empty());

        result = MUST(string.trim(" "sv, TrimMode::Right));
        EXPECT(result.is_empty());
    }
    {
        auto string = "word"_string;

        auto result = MUST(string.trim(" "sv, TrimMode::Both));
        EXPECT_EQ(result, "word"sv);

        result = MUST(string.trim(" "sv, TrimMode::Left));
        EXPECT_EQ(result, "word"sv);

        result = MUST(string.trim(" "sv, TrimMode::Right));
        EXPECT_EQ(result, "word"sv);
    }
    {
        auto string = "    word"_string;

        auto result = MUST(string.trim(" "sv, TrimMode::Both));
        EXPECT_EQ(result, "word"sv);

        result = MUST(string.trim(" "sv, TrimMode::Left));
        EXPECT_EQ(result, "word"sv);

        result = MUST(string.trim(" "sv, TrimMode::Right));
        EXPECT_EQ(result, "    word"sv);
    }
    {
        auto string = "word    "_string;

        auto result = MUST(string.trim(" "sv, TrimMode::Both));
        EXPECT_EQ(result, "word"sv);

        result = MUST(string.trim(" "sv, TrimMode::Left));
        EXPECT_EQ(result, "word    "sv);

        result = MUST(string.trim(" "sv, TrimMode::Right));
        EXPECT_EQ(result, "word"sv);
    }
    {
        auto string = "    word    "_string;

        auto result = MUST(string.trim(" "sv, TrimMode::Both));
        EXPECT_EQ(result, "word"sv);

        result = MUST(string.trim(" "sv, TrimMode::Left));
        EXPECT_EQ(result, "word    "sv);

        result = MUST(string.trim(" "sv, TrimMode::Right));
        EXPECT_EQ(result, "    word"sv);
    }
    {
        auto string = "    word    "_string;

        auto result = MUST(string.trim("\t"sv, TrimMode::Both));
        EXPECT_EQ(result, "    word    "sv);

        result = MUST(string.trim("\t"sv, TrimMode::Left));
        EXPECT_EQ(result, "    word    "sv);

        result = MUST(string.trim("\t"sv, TrimMode::Right));
        EXPECT_EQ(result, "    word    "sv);
    }
    {
        auto string = "ωΣωΣω"_string;

        auto result = MUST(string.trim("ω"sv, TrimMode::Both));
        EXPECT_EQ(result, "ΣωΣ"sv);

        result = MUST(string.trim("ω"sv, TrimMode::Left));
        EXPECT_EQ(result, "ΣωΣω"sv);

        result = MUST(string.trim("ω"sv, TrimMode::Right));
        EXPECT_EQ(result, "ωΣωΣ"sv);
    }
    {
        auto string = "ωΣωΣω"_string;

        auto result = MUST(string.trim("ωΣ"sv, TrimMode::Both));
        EXPECT(result.is_empty());

        result = MUST(string.trim("ωΣ"sv, TrimMode::Left));
        EXPECT(result.is_empty());

        result = MUST(string.trim("ωΣ"sv, TrimMode::Right));
        EXPECT(result.is_empty());
    }
    {
        auto string = "ωΣωΣω"_string;

        auto result = MUST(string.trim("Σω"sv, TrimMode::Both));
        EXPECT(result.is_empty());

        result = MUST(string.trim("Σω"sv, TrimMode::Left));
        EXPECT(result.is_empty());

        result = MUST(string.trim("Σω"sv, TrimMode::Right));
        EXPECT(result.is_empty());
    }
}

TEST_CASE(contains)
{
    EXPECT(!String {}.contains({}));
    EXPECT(!String {}.contains(" "sv));
    EXPECT(!String {}.contains(0));

    EXPECT("a"_string.contains("a"sv));
    EXPECT(!"a"_string.contains({}));
    EXPECT(!"a"_string.contains("b"sv));
    EXPECT(!"a"_string.contains("ab"sv));

    EXPECT("a"_string.contains(0x0061));
    EXPECT(!"a"_string.contains(0x0062));

    EXPECT("abc"_string.contains("a"sv));
    EXPECT("abc"_string.contains("b"sv));
    EXPECT("abc"_string.contains("c"sv));
    EXPECT("abc"_string.contains("ab"sv));
    EXPECT("abc"_string.contains("bc"sv));
    EXPECT("abc"_string.contains("abc"sv));
    EXPECT(!"abc"_string.contains({}));
    EXPECT(!"abc"_string.contains("ac"sv));
    EXPECT(!"abc"_string.contains("abcd"sv));

    EXPECT("abc"_string.contains(0x0061));
    EXPECT("abc"_string.contains(0x0062));
    EXPECT("abc"_string.contains(0x0063));
    EXPECT(!"abc"_string.contains(0x0064));

    auto emoji = "😀"_string;
    EXPECT(emoji.contains("\xF0"sv));
    EXPECT(emoji.contains("\x9F"sv));
    EXPECT(emoji.contains("\x98"sv));
    EXPECT(emoji.contains("\x80"sv));
    EXPECT(emoji.contains("\xF0\x9F"sv));
    EXPECT(emoji.contains("\xF0\x9F\x98"sv));
    EXPECT(emoji.contains("\xF0\x9F\x98\x80"sv));
    EXPECT(emoji.contains("\x9F\x98\x80"sv));
    EXPECT(emoji.contains("\x98\x80"sv));
    EXPECT(!emoji.contains("a"sv));
    EXPECT(!emoji.contains("🙃"sv));

    EXPECT(emoji.contains(0x1F600));
    EXPECT(!emoji.contains(0x1F643));
}

TEST_CASE(starts_with)
{
    EXPECT(String {}.starts_with_bytes({}));
    EXPECT(!String {}.starts_with_bytes(" "sv));
    EXPECT(!String {}.starts_with(0));

    EXPECT("a"_string.starts_with_bytes({}));
    EXPECT("a"_string.starts_with_bytes("a"sv));
    EXPECT(!"a"_string.starts_with_bytes("b"sv));
    EXPECT(!"a"_string.starts_with_bytes("ab"sv));

    EXPECT("a"_string.starts_with(0x0061));
    EXPECT(!"a"_string.starts_with(0x0062));

    EXPECT("abc"_string.starts_with_bytes({}));
    EXPECT("abc"_string.starts_with_bytes("a"sv));
    EXPECT("abc"_string.starts_with_bytes("ab"sv));
    EXPECT("abc"_string.starts_with_bytes("abc"sv));
    EXPECT(!"abc"_string.starts_with_bytes("b"sv));
    EXPECT(!"abc"_string.starts_with_bytes("bc"sv));

    EXPECT("abc"_string.starts_with(0x0061));
    EXPECT(!"abc"_string.starts_with(0x0062));
    EXPECT(!"abc"_string.starts_with(0x0063));

    auto emoji = "😀🙃"_string;
    EXPECT(emoji.starts_with_bytes("\xF0"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0\x9F"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0\x9F\x99"sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0\x9F\x99\x83"sv));
    EXPECT(!emoji.starts_with_bytes("a"sv));
    EXPECT(!emoji.starts_with_bytes("🙃"sv));

    EXPECT(emoji.starts_with(0x1F600));
    EXPECT(!emoji.starts_with(0x1F643));
}

TEST_CASE(ends_with)
{
    EXPECT(String {}.ends_with_bytes({}));
    EXPECT(!String {}.ends_with_bytes(" "sv));
    EXPECT(!String {}.ends_with(0));

    EXPECT("a"_string.ends_with_bytes({}));
    EXPECT("a"_string.ends_with_bytes("a"sv));
    EXPECT(!"a"_string.ends_with_bytes("b"sv));
    EXPECT(!"a"_string.ends_with_bytes("ba"sv));

    EXPECT("a"_string.ends_with(0x0061));
    EXPECT(!"a"_string.ends_with(0x0062));

    EXPECT("abc"_string.ends_with_bytes({}));
    EXPECT("abc"_string.ends_with_bytes("c"sv));
    EXPECT("abc"_string.ends_with_bytes("bc"sv));
    EXPECT("abc"_string.ends_with_bytes("abc"sv));
    EXPECT(!"abc"_string.ends_with_bytes("b"sv));
    EXPECT(!"abc"_string.ends_with_bytes("ab"sv));

    EXPECT("abc"_string.ends_with(0x0063));
    EXPECT(!"abc"_string.ends_with(0x0062));
    EXPECT(!"abc"_string.ends_with(0x0061));

    auto emoji = "😀🙃"_string;
    EXPECT(emoji.ends_with_bytes("\x83"sv));
    EXPECT(emoji.ends_with_bytes("\x99\x83"sv));
    EXPECT(emoji.ends_with_bytes("\x9F\x99\x83"sv));
    EXPECT(emoji.ends_with_bytes("\xF0\x9F\x99\x83"sv));
    EXPECT(emoji.ends_with_bytes("\x80\xF0\x9F\x99\x83"sv));
    EXPECT(emoji.ends_with_bytes("\x98\x80\xF0\x9F\x99\x83"sv));
    EXPECT(emoji.ends_with_bytes("\x9F\x98\x80\xF0\x9F\x99\x83"sv));
    EXPECT(emoji.ends_with_bytes("\xF0\x9F\x98\x80\xF0\x9F\x99\x83"sv));
    EXPECT(!emoji.ends_with_bytes("a"sv));
    EXPECT(!emoji.ends_with_bytes("😀"sv));

    EXPECT(emoji.ends_with(0x1F643));
    EXPECT(!emoji.ends_with(0x1F600));
}
