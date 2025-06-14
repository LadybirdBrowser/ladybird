/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
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
    EXPECT_EQ(empty, ""_sv);

    auto empty2 = ""_string;
    EXPECT(empty2.is_empty());
    EXPECT_EQ(empty, empty2);

    auto empty3 = MUST(String::from_utf8(""_sv));
    EXPECT(empty3.is_empty());
    EXPECT_EQ(empty, empty3);
}

TEST_CASE(move_assignment)
{
    String string1 = "hello"_string;
    string1 = "friends!"_string;
    EXPECT_EQ(string1, "friends!"_sv);
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
    auto string1 = MUST(String::from_utf8("foo bar"_sv));
    EXPECT_EQ(string1.is_short_string(), true);
    EXPECT_EQ(string1.bytes().size(), 7u);
    EXPECT_EQ(string1.bytes_as_string_view(), "foo bar"_sv);
    // check for odd "pointer" value, i.e. short string flag
    EXPECT_EQ(*((uintptr_t*)&string1) % 2UL, 1U);

    auto string2 = "foo bar"_string;
    EXPECT_EQ(string2.is_short_string(), true);
    EXPECT_EQ(string2.bytes().size(), 7u);
    EXPECT_EQ(string2, string1);
    // check for odd "pointer" value, i.e. short string flag
    EXPECT_EQ(*((uintptr_t*)&string2) % 2UL, 1U);
#else
    auto string1 = MUST(String::from_utf8("foo"_sv));
    EXPECT_EQ(string1.is_short_string(), true);
    EXPECT_EQ(string1.bytes().size(), 3u);
    EXPECT_EQ(string1.bytes_as_string_view(), "foo"_sv);
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
    auto string = MUST(String::from_utf8("abcdefgh"_sv));
    EXPECT_EQ(string.is_short_string(), false);
    EXPECT_EQ(string.bytes().size(), 8u);
    EXPECT_EQ(string.bytes_as_string_view(), "abcdefgh"_sv);
}

TEST_CASE(long_streams)
{
    {
        u8 bytes[64] = {};
        constexpr auto test_view = "Well, hello friends"_sv;
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
        MUST(stream.write_until_depleted(("abc"_sv).bytes()));

        auto string = MUST(String::from_stream(stream, 3u));

        EXPECT_EQ(string.is_short_string(), true);
        EXPECT_EQ(string.bytes().size(), 3u);
        EXPECT_EQ(string.bytes_as_string_view(), "abc"_sv);
    }

    {
        AllocatingMemoryStream stream;
        MUST(stream.write_until_depleted(("0123456789"_sv).bytes()));

        auto string = MUST(String::from_stream(stream, 9u));

        EXPECT_EQ(string.is_short_string(), false);
        EXPECT_EQ(string.bytes().size(), 9u);
        EXPECT_EQ(string.bytes_as_string_view(), "012345678"_sv);
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
    auto string1 = String::from_utf8("long string \xf4\x8f\xbf\xc0"_sv); // U+110000
    EXPECT(string1.is_error());
    EXPECT(string1.error().string_literal().contains("Input was not valid UTF-8"_sv));

    auto string2 = String::from_utf8("\xf4\xa1\xb0\xbd"_sv); // U+121C3D
    EXPECT(string2.is_error());
    EXPECT(string2.error().string_literal().contains("Input was not valid UTF-8"_sv));

    AllocatingMemoryStream stream;
    MUST(stream.write_value<u8>(0xf4));
    MUST(stream.write_value<u8>(0xa1));
    MUST(stream.write_value<u8>(0xb0));
    MUST(stream.write_value<u8>(0xbd));
    auto string3 = String::from_stream(stream, stream.used_buffer_size());
    EXPECT_EQ(string3.is_error(), true);
    EXPECT(string3.error().string_literal().contains("Input was not valid UTF-8"_sv));
}

TEST_CASE(with_replacement_character)
{
    auto string1 = String::from_utf8_with_replacement_character("long string \xf4\x8f\xbf\xc0"_sv, String::WithBOMHandling::No); // U+110000
    Array<u8, 24> string1_expected { 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x20, 0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd };
    EXPECT_EQ(string1.bytes(), string1_expected);

    auto string3 = String::from_utf8_with_replacement_character("A valid string!"_sv, String::WithBOMHandling::No);
    EXPECT_EQ(string3, "A valid string!"_sv);

    auto string4 = String::from_utf8_with_replacement_character(""_sv, String::WithBOMHandling::No);
    EXPECT_EQ(string4, ""_sv);

    auto string5 = String::from_utf8_with_replacement_character("\xEF\xBB\xBFWHF!"_sv, String::WithBOMHandling::Yes);
    EXPECT_EQ(string5, "WHF!"_sv);

    auto string6 = String::from_utf8_with_replacement_character("\xEF\xBB\xBFWHF!"_sv, String::WithBOMHandling::No);
    EXPECT_EQ(string6, "\xEF\xBB\xBFWHF!"_sv);
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
    EXPECT_EQ(string, "\xF4\x8F\xBF\xBF"_sv);

    EXPECT_DEATH("Creating a string from an invalid code point", (void)String::from_code_point(0xffffffff));
}

TEST_CASE(substring)
{
    auto superstring = "Hello I am a long string"_string;
    auto short_substring = MUST(superstring.substring_from_byte_offset(0, 5));
    EXPECT_EQ(short_substring, "Hello"_sv);

    auto long_substring = MUST(superstring.substring_from_byte_offset(0, 10));
    EXPECT_EQ(long_substring, "Hello I am"_sv);
}

TEST_CASE(substring_with_shared_superstring)
{
    auto superstring = "Hello I am a long string"_string;

    auto substring1 = MUST(superstring.substring_from_byte_offset_with_shared_superstring(0, 5));
    EXPECT_EQ(substring1, "Hello"_sv);

    auto substring2 = MUST(superstring.substring_from_byte_offset_with_shared_superstring(0, 10));
    EXPECT_EQ(substring2, "Hello I am"_sv);
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
    EXPECT_EQ(string, "🦬🪒"_sv);
    EXPECT_EQ(string.bytes().size(), 8u);
}

TEST_CASE(ak_format)
{
    auto foo = MUST(String::formatted("Hello {}", "friends"_string));
    EXPECT_EQ(foo, "Hello friends"_sv);
}

TEST_CASE(replace)
{
    {
        auto haystack = "Hello enemies"_string;
        auto result = MUST(haystack.replace("enemies"_sv, "friends"_sv, ReplaceMode::All));
        EXPECT_EQ(result, "Hello friends"_sv);
    }

    {
        auto base_title = "anon@courage:~"_string;
        auto result = MUST(base_title.replace("[*]"_sv, "(*)"_sv, ReplaceMode::FirstOnly));
        EXPECT_EQ(result, "anon@courage:~"_sv);
    }
}

TEST_CASE(reverse)
{
    auto test_reverse = [](auto test, auto expected) {
        auto string = MUST(String::from_utf8(test));
        auto result = MUST(string.reverse());

        EXPECT_EQ(result, expected);
    };

    test_reverse(""_sv, ""_sv);
    test_reverse("a"_sv, "a"_sv);
    test_reverse("ab"_sv, "ba"_sv);
    test_reverse("ab cd ef"_sv, "fe dc ba"_sv);
    test_reverse("😀"_sv, "😀"_sv);
    test_reverse("ab😀cd"_sv, "dc😀ba"_sv);
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
    auto result = MUST("I"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "i"_sv);

    result = MUST("I"_string.to_lowercase("az"_sv));
    EXPECT_EQ(result, "\u0131"_sv);

    result = MUST("I"_string.to_lowercase("tr"_sv));
    EXPECT_EQ(result, "\u0131"_sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = MUST("\u0130"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "\u0069\u0307"_sv);

    result = MUST("\u0130"_string.to_lowercase("az"_sv));
    EXPECT_EQ(result, "i"_sv);

    result = MUST("\u0130"_string.to_lowercase("tr"_sv));
    EXPECT_EQ(result, "i"_sv);

    // LATIN CAPITAL LETTER I followed by COMBINING DOT ABOVE
    result = MUST("I\u0307"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "i\u0307"_sv);

    result = MUST("I\u0307"_string.to_lowercase("az"_sv));
    EXPECT_EQ(result, "i"_sv);

    result = MUST("I\u0307"_string.to_lowercase("tr"_sv));
    EXPECT_EQ(result, "i"_sv);

    // LATIN CAPITAL LETTER I followed by combining class 0 and COMBINING DOT ABOVE
    result = MUST("IA\u0307"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "ia\u0307"_sv);

    result = MUST("IA\u0307"_string.to_lowercase("az"_sv));
    EXPECT_EQ(result, "\u0131a\u0307"_sv);

    result = MUST("IA\u0307"_string.to_lowercase("tr"_sv));
    EXPECT_EQ(result, "\u0131a\u0307"_sv);
}

TEST_CASE(to_lowercase_special_casing_more_above)
{
    // LATIN CAPITAL LETTER I
    auto result = MUST("I"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "i"_sv);

    result = MUST("I"_string.to_lowercase("lt"_sv));
    EXPECT_EQ(result, "i"_sv);

    // LATIN CAPITAL LETTER J
    result = MUST("J"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "j"_sv);

    result = MUST("J"_string.to_lowercase("lt"_sv));
    EXPECT_EQ(result, "j"_sv);

    // LATIN CAPITAL LETTER I WITH OGONEK
    result = MUST("\u012e"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "\u012f"_sv);

    result = MUST("\u012e"_string.to_lowercase("lt"_sv));
    EXPECT_EQ(result, "\u012f"_sv);

    // LATIN CAPITAL LETTER I followed by COMBINING GRAVE ACCENT
    result = MUST("I\u0300"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "i\u0300"_sv);

    result = MUST("I\u0300"_string.to_lowercase("lt"_sv));
    EXPECT_EQ(result, "i\u0307\u0300"_sv);

    // LATIN CAPITAL LETTER J followed by COMBINING GRAVE ACCENT
    result = MUST("J\u0300"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "j\u0300"_sv);

    result = MUST("J\u0300"_string.to_lowercase("lt"_sv));
    EXPECT_EQ(result, "j\u0307\u0300"_sv);

    // LATIN CAPITAL LETTER I WITH OGONEK followed by COMBINING GRAVE ACCENT
    result = MUST("\u012e\u0300"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "\u012f\u0300"_sv);

    result = MUST("\u012e\u0300"_string.to_lowercase("lt"_sv));
    EXPECT_EQ(result, "\u012f\u0307\u0300"_sv);
}

TEST_CASE(to_lowercase_special_casing_not_before_dot)
{
    // LATIN CAPITAL LETTER I
    auto result = MUST("I"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "i"_sv);

    result = MUST("I"_string.to_lowercase("az"_sv));
    EXPECT_EQ(result, "\u0131"_sv);

    result = MUST("I"_string.to_lowercase("tr"_sv));
    EXPECT_EQ(result, "\u0131"_sv);

    // LATIN CAPITAL LETTER I followed by COMBINING DOT ABOVE
    result = MUST("I\u0307"_string.to_lowercase("en"_sv));
    EXPECT_EQ(result, "i\u0307"_sv);

    result = MUST("I\u0307"_string.to_lowercase("az"_sv));
    EXPECT_EQ(result, "i"_sv);

    result = MUST("I\u0307"_string.to_lowercase("tr"_sv));
    EXPECT_EQ(result, "i"_sv);
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
    auto result = MUST("i"_string.to_uppercase("en"_sv));
    EXPECT_EQ(result, "I"_sv);

    result = MUST("i"_string.to_uppercase("lt"_sv));
    EXPECT_EQ(result, "I"_sv);

    // LATIN SMALL LETTER J
    result = MUST("j"_string.to_uppercase("en"_sv));
    EXPECT_EQ(result, "J"_sv);

    result = MUST("j"_string.to_uppercase("lt"_sv));
    EXPECT_EQ(result, "J"_sv);

    // LATIN SMALL LETTER I followed by COMBINING DOT ABOVE
    result = MUST("i\u0307"_string.to_uppercase("en"_sv));
    EXPECT_EQ(result, "I\u0307"_sv);

    result = MUST("i\u0307"_string.to_uppercase("lt"_sv));
    EXPECT_EQ(result, "I"_sv);

    // LATIN SMALL LETTER J followed by COMBINING DOT ABOVE
    result = MUST("j\u0307"_string.to_uppercase("en"_sv));
    EXPECT_EQ(result, "J\u0307"_sv);

    result = MUST("j\u0307"_string.to_uppercase("lt"_sv));
    EXPECT_EQ(result, "J"_sv);
}

TEST_CASE(to_titlecase)
{
    EXPECT_EQ(MUST(""_string.to_titlecase()), ""_sv);
    EXPECT_EQ(MUST(" "_string.to_titlecase()), " "_sv);
    EXPECT_EQ(MUST(" - "_string.to_titlecase()), " - "_sv);

    EXPECT_EQ(MUST("a"_string.to_titlecase()), "A"_sv);
    EXPECT_EQ(MUST("A"_string.to_titlecase()), "A"_sv);
    EXPECT_EQ(MUST(" a"_string.to_titlecase()), " A"_sv);
    EXPECT_EQ(MUST("a "_string.to_titlecase()), "A "_sv);

    EXPECT_EQ(MUST("ab"_string.to_titlecase()), "Ab"_sv);
    EXPECT_EQ(MUST("Ab"_string.to_titlecase()), "Ab"_sv);
    EXPECT_EQ(MUST("aB"_string.to_titlecase()), "Ab"_sv);
    EXPECT_EQ(MUST("AB"_string.to_titlecase()), "Ab"_sv);
    EXPECT_EQ(MUST(" ab"_string.to_titlecase()), " Ab"_sv);
    EXPECT_EQ(MUST("ab "_string.to_titlecase()), "Ab "_sv);

    EXPECT_EQ(MUST("foo bar baz"_string.to_titlecase()), "Foo Bar Baz"_sv);
    EXPECT_EQ(MUST("foo \n \r bar \t baz"_string.to_titlecase()), "Foo \n \r Bar \t Baz"_sv);
    EXPECT_EQ(MUST("f\"oo\" b'ar'"_string.to_titlecase()), "F\"Oo\" B'ar'"_sv);
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
    EXPECT_EQ(result, "\u0073\u0073"_sv);

    // GREEK SMALL LETTER ALPHA WITH YPOGEGRAMMENI
    result = MUST("\u1FB3"_string.to_casefold());
    EXPECT_EQ(result, "\u03B1\u03B9"_sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI
    result = MUST("\u1FB6"_string.to_casefold());
    EXPECT_EQ(result, "\u03B1\u0342"_sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FB7"_string.to_casefold());
    EXPECT_EQ(result, "\u03B1\u0342\u03B9"_sv);
}

TEST_CASE(to_titlecase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = MUST("\u00DF"_string.to_titlecase());
    EXPECT_EQ(result, "\u0053\u0073"_sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = MUST("\u0130"_string.to_titlecase());
    EXPECT_EQ(result, "\u0130"_sv);

    // LATIN SMALL LIGATURE FF
    result = MUST("\uFB00"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0066"_sv);

    // LATIN SMALL LIGATURE FI
    result = MUST("\uFB01"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0069"_sv);

    // LATIN SMALL LIGATURE FL
    result = MUST("\uFB02"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u006C"_sv);

    // LATIN SMALL LIGATURE FFI
    result = MUST("\uFB03"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0066\u0069"_sv);

    // LATIN SMALL LIGATURE FFL
    result = MUST("\uFB04"_string.to_titlecase());
    EXPECT_EQ(result, "\u0046\u0066\u006C"_sv);

    // LATIN SMALL LIGATURE LONG S T
    result = MUST("\uFB05"_string.to_titlecase());
    EXPECT_EQ(result, "\u0053\u0074"_sv);

    // LATIN SMALL LIGATURE ST
    result = MUST("\uFB06"_string.to_titlecase());
    EXPECT_EQ(result, "\u0053\u0074"_sv);

    // GREEK SMALL LETTER IOTA WITH DIALYTIKA AND TONOS
    result = MUST("\u0390"_string.to_titlecase());
    EXPECT_EQ(result, "\u0399\u0308\u0301"_sv);

    // GREEK SMALL LETTER UPSILON WITH DIALYTIKA AND TONOS
    result = MUST("\u03B0"_string.to_titlecase());
    EXPECT_EQ(result, "\u03A5\u0308\u0301"_sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FB7"_string.to_titlecase());
    EXPECT_EQ(result, "\u0391\u0342\u0345"_sv);

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FC7"_string.to_titlecase());
    EXPECT_EQ(result, "\u0397\u0342\u0345"_sv);

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = MUST("\u1FF7"_string.to_titlecase());
    EXPECT_EQ(result, "\u03A9\u0342\u0345"_sv);
}

TEST_CASE(to_titlecase_special_casing_i)
{
    // LATIN SMALL LETTER I
    auto result = MUST("i"_string.to_titlecase("en"_sv));
    EXPECT_EQ(result, "I"_sv);

    result = MUST("i"_string.to_titlecase("az"_sv));
    EXPECT_EQ(result, "\u0130"_sv);

    result = MUST("i"_string.to_titlecase("tr"_sv));
    EXPECT_EQ(result, "\u0130"_sv);
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

    EXPECT(!bar.is_one_of("foo"_sv));
    EXPECT(bar.is_one_of("foo"_sv, "bar"_sv));
    EXPECT(bar.is_one_of("bar"_sv, "foo"_sv));
    EXPECT(bar.is_one_of("bar"_sv));
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
        EXPECT_EQ(parts[0], "ω"_sv);
        EXPECT_EQ(parts[1], "2ω"_sv);
        EXPECT_EQ(parts[2], "ω"_sv);
    }
}

TEST_CASE(find_byte_offset)
{
    {
        String string {};
        auto index1 = string.find_byte_offset(0);
        EXPECT(!index1.has_value());

        auto index2 = string.find_byte_offset(""_sv);
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

        auto index1 = string.find_byte_offset("fo"_sv);
        EXPECT_EQ(index1, 0u);

        auto index2 = string.find_byte_offset("oo"_sv);
        EXPECT_EQ(index2, 1u);

        auto index3 = string.find_byte_offset("o"_sv, *index2 + 1);
        EXPECT_EQ(index3, 2u);

        auto index4 = string.find_byte_offset("fooo"_sv);
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

        auto index1 = string.find_byte_offset("ω"_sv);
        EXPECT_EQ(index1, 0u);

        auto index2 = string.find_byte_offset("Σ"_sv);
        EXPECT_EQ(index2, 2u);

        auto index3 = string.find_byte_offset("ω"_sv, 2);
        EXPECT_EQ(index3, 4u);

        auto index4 = string.find_byte_offset("Σ"_sv, 4);
        EXPECT_EQ(index4, 6u);

        auto index5 = string.find_byte_offset("ω"_sv, 6);
        EXPECT_EQ(index5, 8u);
    }
}

TEST_CASE(find_byte_offset_ignoring_case)
{
    {
        auto string = ""_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case(""_sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("1"_sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("2"_sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("23"_sv).has_value(), false);
    }
    {
        auto string = "1234567"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case(""_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("1"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("2"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("3"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("4"_sv), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("5"_sv), 4u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("6"_sv), 5u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("7"_sv), 6u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("34"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("45"_sv), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("56"_sv), 4u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("67"_sv), 5u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("a"_sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("8"_sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("78"_sv).has_value(), false);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("46"_sv).has_value(), false);
    }
    {
        auto string = "abCDef"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("A"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("B"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("c"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("d"_sv), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("e"_sv), 4u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("f"_sv), 5u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("AbC"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("BcdE"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("cd"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("cD"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("E"_sv), 4u);
    }
    {
        auto string = "abßcd"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"_sv), 2u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSS"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSs"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bss"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bS"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bs"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bß"_sv), 1u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSSc"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSsc"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bssc"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bßc"_sv), 1u);
        EXPECT(!string.find_byte_offset_ignoring_case("bSc"_sv).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("bsc"_sv).has_value());
    }
    {
        auto string = "abSScd"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"_sv), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"_sv), 2u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSS"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSs"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bss"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bS"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bs"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bß"_sv), 1u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSSc"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bSsc"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bssc"_sv), 1u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("bßc"_sv), 1u);
        EXPECT(!string.find_byte_offset_ignoring_case("bSc"_sv).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("bsc"_sv).has_value());
    }
    {
        auto string = "ßSßs"_string;

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"_sv), 0u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"_sv), 0u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"_sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"_sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"_sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"_sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"_sv, 2), 2u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"_sv, 2), 2u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("SS"_sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("Ss"_sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ss"_sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"_sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"_sv, 3), 3u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("ß"_sv, 3), 3u);

        EXPECT_EQ(string.find_byte_offset_ignoring_case("S"_sv, 5), 5u);
        EXPECT_EQ(string.find_byte_offset_ignoring_case("s"_sv, 5), 5u);
        EXPECT(!string.find_byte_offset_ignoring_case("SS"_sv, 5).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("Ss"_sv, 5).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("ss"_sv, 5).has_value());
        EXPECT(!string.find_byte_offset_ignoring_case("ß"_sv, 5).has_value());
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
        EXPECT_EQ(string1, "a"_sv);

        auto string2 = MUST(String::repeated(0x03C9U, 1));
        EXPECT(string2.is_short_string());
        EXPECT_EQ(string2.bytes_as_string_view().length(), 2u);
        EXPECT_EQ(string2, "ω"_sv);

        auto string3 = MUST(String::repeated(0x10300, 1));
#ifdef AK_ARCH_64_BIT
        EXPECT(string3.is_short_string());
#else
        EXPECT(!string3.is_short_string());
#endif
        EXPECT_EQ(string3.bytes_as_string_view().length(), 4u);
        EXPECT_EQ(string3, "𐌀"_sv);
    }
    {
        auto string1 = MUST(String::repeated('a', 3));
        EXPECT(string1.is_short_string());
        EXPECT_EQ(string1.bytes_as_string_view().length(), 3u);
        EXPECT_EQ(string1, "aaa"_sv);

        auto string2 = MUST(String::repeated(0x03C9U, 3));
#ifdef AK_ARCH_64_BIT
        EXPECT(string2.is_short_string());
#else
        EXPECT(!string2.is_short_string());
#endif
        EXPECT_EQ(string2.bytes_as_string_view().length(), 6u);
        EXPECT_EQ(string2, "ωωω"_sv);

        auto string3 = MUST(String::repeated(0x10300, 3));
        EXPECT(!string3.is_short_string());
        EXPECT_EQ(string3.bytes_as_string_view().length(), 12u);
        EXPECT_EQ(string3, "𐌀𐌀𐌀"_sv);
    }
    {
        auto string1 = MUST(String::repeated('a', 10));
        EXPECT(!string1.is_short_string());
        EXPECT_EQ(string1.bytes_as_string_view().length(), 10u);
        EXPECT_EQ(string1, "aaaaaaaaaa"_sv);

        auto string2 = MUST(String::repeated(0x03C9U, 10));
        EXPECT(!string2.is_short_string());
        EXPECT_EQ(string2.bytes_as_string_view().length(), 20u);
        EXPECT_EQ(string2, "ωωωωωωωωωω"_sv);

        auto string3 = MUST(String::repeated(0x10300, 10));
        EXPECT(!string3.is_short_string());
        EXPECT_EQ(string3.bytes_as_string_view().length(), 40u);
        EXPECT_EQ(string3, "𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀"_sv);
    }

    EXPECT_DEATH("Creating a string from an invalid code point", (void)String::repeated(0xffffffff, 1));
}

TEST_CASE(join)
{
    auto string1 = MUST(String::join(',', Vector<i32> {}));
    EXPECT(string1.is_empty());

    auto string2 = MUST(String::join(',', Array { 1 }));
    EXPECT_EQ(string2, "1"_sv);

    auto string3 = MUST(String::join(':', Array { 1 }, "[{}]"_sv));
    EXPECT_EQ(string3, "[1]"_sv);

    auto string4 = MUST(String::join(',', Array { 1, 2, 3 }));
    EXPECT_EQ(string4, "1,2,3"_sv);

    auto string5 = MUST(String::join(',', Array { 1, 2, 3 }, "[{}]"_sv));
    EXPECT_EQ(string5, "[1],[2],[3]"_sv);

    auto string6 = MUST(String::join("!!!"_string, Array { "foo"_sv, "bar"_sv, "baz"_sv }));
    EXPECT_EQ(string6, "foo!!!bar!!!baz"_sv);

    auto string7 = MUST(String::join(" - "_sv, Array { 1, 16, 256, 4096 }, "[{:#04x}]"_sv));
    EXPECT_EQ(string7, "[0x0001] - [0x0010] - [0x0100] - [0x1000]"_sv);
}

TEST_CASE(trim)
{
    {
        String string {};

        auto result = MUST(string.trim(" "_sv, TrimMode::Both));
        EXPECT(result.is_empty());

        result = MUST(string.trim(" "_sv, TrimMode::Left));
        EXPECT(result.is_empty());

        result = MUST(string.trim(" "_sv, TrimMode::Right));
        EXPECT(result.is_empty());
    }
    {
        auto string = "word"_string;

        auto result = MUST(string.trim(" "_sv, TrimMode::Both));
        EXPECT_EQ(result, "word"_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Left));
        EXPECT_EQ(result, "word"_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Right));
        EXPECT_EQ(result, "word"_sv);
    }
    {
        auto string = "    word"_string;

        auto result = MUST(string.trim(" "_sv, TrimMode::Both));
        EXPECT_EQ(result, "word"_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Left));
        EXPECT_EQ(result, "word"_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Right));
        EXPECT_EQ(result, "    word"_sv);
    }
    {
        auto string = "word    "_string;

        auto result = MUST(string.trim(" "_sv, TrimMode::Both));
        EXPECT_EQ(result, "word"_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Left));
        EXPECT_EQ(result, "word    "_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Right));
        EXPECT_EQ(result, "word"_sv);
    }
    {
        auto string = "    word    "_string;

        auto result = MUST(string.trim(" "_sv, TrimMode::Both));
        EXPECT_EQ(result, "word"_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Left));
        EXPECT_EQ(result, "word    "_sv);

        result = MUST(string.trim(" "_sv, TrimMode::Right));
        EXPECT_EQ(result, "    word"_sv);
    }
    {
        auto string = "    word    "_string;

        auto result = MUST(string.trim("\t"_sv, TrimMode::Both));
        EXPECT_EQ(result, "    word    "_sv);

        result = MUST(string.trim("\t"_sv, TrimMode::Left));
        EXPECT_EQ(result, "    word    "_sv);

        result = MUST(string.trim("\t"_sv, TrimMode::Right));
        EXPECT_EQ(result, "    word    "_sv);
    }
    {
        auto string = "ωΣωΣω"_string;

        auto result = MUST(string.trim("ω"_sv, TrimMode::Both));
        EXPECT_EQ(result, "ΣωΣ"_sv);

        result = MUST(string.trim("ω"_sv, TrimMode::Left));
        EXPECT_EQ(result, "ΣωΣω"_sv);

        result = MUST(string.trim("ω"_sv, TrimMode::Right));
        EXPECT_EQ(result, "ωΣωΣ"_sv);
    }
    {
        auto string = "ωΣωΣω"_string;

        auto result = MUST(string.trim("ωΣ"_sv, TrimMode::Both));
        EXPECT(result.is_empty());

        result = MUST(string.trim("ωΣ"_sv, TrimMode::Left));
        EXPECT(result.is_empty());

        result = MUST(string.trim("ωΣ"_sv, TrimMode::Right));
        EXPECT(result.is_empty());
    }
    {
        auto string = "ωΣωΣω"_string;

        auto result = MUST(string.trim("Σω"_sv, TrimMode::Both));
        EXPECT(result.is_empty());

        result = MUST(string.trim("Σω"_sv, TrimMode::Left));
        EXPECT(result.is_empty());

        result = MUST(string.trim("Σω"_sv, TrimMode::Right));
        EXPECT(result.is_empty());
    }
}

TEST_CASE(trim_whitespace)
{
    {
        String string {};
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), String {});
    }
    {
        auto string = " "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), String {});
    }
    {
        auto string = "   "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), String {});
    }
    {
        auto string = " \t \n \r \u00A0 \u202F "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), String {});
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), String {});
    }
    {
        auto string = "abcdef"_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "abcdef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "abcdef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), "abcdef"_string);
    }
    {
        auto string = " \u00A0 abcdef"_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "abcdef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "abcdef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), " \u00A0 abcdef"_string);
    }
    {
        auto string = "abcdef \u202F "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "abcdef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "abcdef \u202F "_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), "abcdef"_string);
    }
    {
        auto string = " \u00A0 abcdef \u202F "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "abcdef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "abcdef \u202F "_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), " \u00A0 abcdef"_string);
    }
    {
        auto string = "ab \t cd \n ef"_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "ab \t cd \n ef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "ab \t cd \n ef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), "ab \t cd \n ef"_string);
    }
    {
        auto string = " \u00A0 ab \t cd \n ef"_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "ab \t cd \n ef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "ab \t cd \n ef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), " \u00A0 ab \t cd \n ef"_string);
    }
    {
        auto string = "ab \t cd \n ef \u202F "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "ab \t cd \n ef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "ab \t cd \n ef \u202F "_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), "ab \t cd \n ef"_string);
    }
    {
        auto string = " \u00A0 ab \t cd \n ef \u202F "_string;
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Both)), "ab \t cd \n ef"_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Left)), "ab \t cd \n ef \u202F "_string);
        EXPECT_EQ(MUST(string.trim_whitespace(TrimMode::Right)), " \u00A0 ab \t cd \n ef"_string);
    }
}

TEST_CASE(contains)
{
    EXPECT(!String {}.contains({}));
    EXPECT(!String {}.contains(" "_sv));
    EXPECT(!String {}.contains(0));

    EXPECT("a"_string.contains("a"_sv));
    EXPECT(!"a"_string.contains({}));
    EXPECT(!"a"_string.contains("b"_sv));
    EXPECT(!"a"_string.contains("ab"_sv));

    EXPECT("a"_string.contains(0x0061));
    EXPECT(!"a"_string.contains(0x0062));

    EXPECT("abc"_string.contains("a"_sv));
    EXPECT("abc"_string.contains("b"_sv));
    EXPECT("abc"_string.contains("c"_sv));
    EXPECT("abc"_string.contains("ab"_sv));
    EXPECT("abc"_string.contains("bc"_sv));
    EXPECT("abc"_string.contains("abc"_sv));
    EXPECT(!"abc"_string.contains({}));
    EXPECT(!"abc"_string.contains("ac"_sv));
    EXPECT(!"abc"_string.contains("abcd"_sv));

    EXPECT("abc"_string.contains(0x0061));
    EXPECT("abc"_string.contains(0x0062));
    EXPECT("abc"_string.contains(0x0063));
    EXPECT(!"abc"_string.contains(0x0064));

    auto emoji = "😀"_string;
    EXPECT(emoji.contains("\xF0"_sv));
    EXPECT(emoji.contains("\x9F"_sv));
    EXPECT(emoji.contains("\x98"_sv));
    EXPECT(emoji.contains("\x80"_sv));
    EXPECT(emoji.contains("\xF0\x9F"_sv));
    EXPECT(emoji.contains("\xF0\x9F\x98"_sv));
    EXPECT(emoji.contains("\xF0\x9F\x98\x80"_sv));
    EXPECT(emoji.contains("\x9F\x98\x80"_sv));
    EXPECT(emoji.contains("\x98\x80"_sv));
    EXPECT(!emoji.contains("a"_sv));
    EXPECT(!emoji.contains("🙃"_sv));

    EXPECT(emoji.contains(0x1F600));
    EXPECT(!emoji.contains(0x1F643));
}

TEST_CASE(starts_with)
{
    EXPECT(String {}.starts_with_bytes({}));
    EXPECT(!String {}.starts_with_bytes(" "_sv));
    EXPECT(!String {}.starts_with(0));

    EXPECT("a"_string.starts_with_bytes({}));
    EXPECT("a"_string.starts_with_bytes("a"_sv));
    EXPECT(!"a"_string.starts_with_bytes("b"_sv));
    EXPECT(!"a"_string.starts_with_bytes("ab"_sv));

    EXPECT("a"_string.starts_with(0x0061));
    EXPECT(!"a"_string.starts_with(0x0062));

    EXPECT("abc"_string.starts_with_bytes({}));
    EXPECT("abc"_string.starts_with_bytes("a"_sv));
    EXPECT("abc"_string.starts_with_bytes("ab"_sv));
    EXPECT("abc"_string.starts_with_bytes("abc"_sv));
    EXPECT(!"abc"_string.starts_with_bytes("b"_sv));
    EXPECT(!"abc"_string.starts_with_bytes("bc"_sv));

    EXPECT("abc"_string.starts_with(0x0061));
    EXPECT(!"abc"_string.starts_with(0x0062));
    EXPECT(!"abc"_string.starts_with(0x0063));

    auto emoji = "😀🙃"_string;
    EXPECT(emoji.starts_with_bytes("\xF0"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0\x9F"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0\x9F\x99"_sv));
    EXPECT(emoji.starts_with_bytes("\xF0\x9F\x98\x80\xF0\x9F\x99\x83"_sv));
    EXPECT(!emoji.starts_with_bytes("a"_sv));
    EXPECT(!emoji.starts_with_bytes("🙃"_sv));

    EXPECT(emoji.starts_with(0x1F600));
    EXPECT(!emoji.starts_with(0x1F643));
}

TEST_CASE(ends_with)
{
    EXPECT(String {}.ends_with_bytes({}));
    EXPECT(!String {}.ends_with_bytes(" "_sv));
    EXPECT(!String {}.ends_with(0));

    EXPECT("a"_string.ends_with_bytes({}));
    EXPECT("a"_string.ends_with_bytes("a"_sv));
    EXPECT(!"a"_string.ends_with_bytes("b"_sv));
    EXPECT(!"a"_string.ends_with_bytes("ba"_sv));

    EXPECT("a"_string.ends_with(0x0061));
    EXPECT(!"a"_string.ends_with(0x0062));

    EXPECT("abc"_string.ends_with_bytes({}));
    EXPECT("abc"_string.ends_with_bytes("c"_sv));
    EXPECT("abc"_string.ends_with_bytes("bc"_sv));
    EXPECT("abc"_string.ends_with_bytes("abc"_sv));
    EXPECT(!"abc"_string.ends_with_bytes("b"_sv));
    EXPECT(!"abc"_string.ends_with_bytes("ab"_sv));

    EXPECT("abc"_string.ends_with(0x0063));
    EXPECT(!"abc"_string.ends_with(0x0062));
    EXPECT(!"abc"_string.ends_with(0x0061));

    auto emoji = "😀🙃"_string;
    EXPECT(emoji.ends_with_bytes("\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\x99\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\x9F\x99\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\xF0\x9F\x99\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\x80\xF0\x9F\x99\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\x98\x80\xF0\x9F\x99\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\x9F\x98\x80\xF0\x9F\x99\x83"_sv));
    EXPECT(emoji.ends_with_bytes("\xF0\x9F\x98\x80\xF0\x9F\x99\x83"_sv));
    EXPECT(!emoji.ends_with_bytes("a"_sv));
    EXPECT(!emoji.ends_with_bytes("😀"_sv));

    EXPECT(emoji.ends_with(0x1F643));
    EXPECT(!emoji.ends_with(0x1F600));
}

TEST_CASE(to_ascii_lowercase)
{
    EXPECT_EQ("foobar"_string.to_ascii_lowercase(), "foobar"_string);
    EXPECT_EQ("FooBar"_string.to_ascii_lowercase(), "foobar"_string);
    EXPECT_EQ("FOOBAR"_string.to_ascii_lowercase(), "foobar"_string);

    // NOTE: We expect to_ascii_lowercase() to return the same underlying string if no changes are needed.
    auto long_string = "this is a long string that cannot use the short string optimization"_string;
    auto lowercased = long_string.to_ascii_lowercase();
    EXPECT_EQ(long_string.bytes().data(), lowercased.bytes().data());
}

TEST_CASE(to_ascii_uppercase)
{
    EXPECT_EQ("foobar"_string.to_ascii_uppercase(), "FOOBAR"_string);
    EXPECT_EQ("FooBar"_string.to_ascii_uppercase(), "FOOBAR"_string);
    EXPECT_EQ("FOOBAR"_string.to_ascii_uppercase(), "FOOBAR"_string);

    // NOTE: We expect to_ascii_uppercase() to return the same underlying string if no changes are needed.
    auto long_string = "THIS IS A LONG STRING THAT CANNOT USE THE SHORT STRING OPTIMIZATION"_string;
    auto uppercased = long_string.to_ascii_uppercase();
    EXPECT_EQ(long_string.bytes().data(), uppercased.bytes().data());
}

TEST_CASE(is_ascii)
{
    EXPECT(String {}.is_ascii());
    EXPECT(" "_string.is_ascii());
    EXPECT("abc"_string.is_ascii());
    EXPECT("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()"_string.is_ascii());

    EXPECT(!"€"_string.is_ascii());
    EXPECT(!"😀"_string.is_ascii());
    EXPECT(!"abcdefghijklmnopqrstuvwxyz😀ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789😀!@#$%^&*()"_string.is_ascii());
}

TEST_CASE(bijective_base)
{
    EXPECT_EQ(String::bijective_base_from(0, String::Case::Upper), "A"_sv);
    EXPECT_EQ(String::bijective_base_from(25, String::Case::Upper), "Z"_sv);
    EXPECT_EQ(String::bijective_base_from(26, String::Case::Upper), "AA"_sv);
    EXPECT_EQ(String::bijective_base_from(52, String::Case::Upper), "BA"_sv);
    EXPECT_EQ(String::bijective_base_from(701, String::Case::Upper), "ZZ"_sv);
    EXPECT_EQ(String::bijective_base_from(702, String::Case::Upper), "AAA"_sv);
    EXPECT_EQ(String::bijective_base_from(730, String::Case::Upper), "ABC"_sv);
    EXPECT_EQ(String::bijective_base_from(18277, String::Case::Upper), "ZZZ"_sv);
}

TEST_CASE(roman_numerals)
{
    auto zero = String::roman_number_from(0, String::Case::Upper);
    EXPECT_EQ(zero, ""_sv);

    auto one = String::roman_number_from(1, String::Case::Upper);
    EXPECT_EQ(one, "I"_sv);

    auto nine = String::roman_number_from(9, String::Case::Upper);
    EXPECT_EQ(nine, "IX"_sv);

    auto fourty_eight = String::roman_number_from(48, String::Case::Upper);
    EXPECT_EQ(fourty_eight, "XLVIII"_sv);

    auto one_thousand_nine_hundred_ninety_eight = String::roman_number_from(1998, String::Case::Upper);
    EXPECT_EQ(one_thousand_nine_hundred_ninety_eight, "MCMXCVIII"_sv);

    auto four_thousand = String::roman_number_from(4000, String::Case::Upper);
    EXPECT_EQ(four_thousand, "4000"_sv);
}

BENCHMARK_CASE(string_number_u16)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        (void)String::number(static_cast<u16>(12345));
    }
}

BENCHMARK_CASE(string_number_u32)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        (void)String::number(static_cast<u32>(123456789));
    }
}

BENCHMARK_CASE(string_number_u64)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        (void)String::number(static_cast<u64>(123456789));
    }
}

BENCHMARK_CASE(string_number_i16)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        (void)String::number(static_cast<i16>(-12345));
    }
}

BENCHMARK_CASE(string_number_i32)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        (void)String::number(static_cast<i32>(-123456789));
    }
}

BENCHMARK_CASE(string_number_i64)
{
    for (size_t i = 0; i < 10'000'000; ++i) {
        (void)String::number(static_cast<i64>(-123456789));
    }
}
