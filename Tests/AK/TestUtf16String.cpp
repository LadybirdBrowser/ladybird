/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/Enumerate.h>
#include <AK/MemoryStream.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <AK/Utf32View.h>

static_assert(AK::Concepts::HashCompatible<Utf16String, Utf16View>);
static_assert(AK::Concepts::HashCompatible<Utf16View, Utf16String>);

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
            u"\xd800\xdc00"sv,
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
        EXPECT_EQ(string.length_in_code_units(), 178uz);
        EXPECT_EQ(string.length_in_code_points(), 175uz);
        EXPECT_EQ(string, u"abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ--😀--abcdefghijklmnopqrstuvwxyz--𐀀--ABCDEFGHIJKLMNOPQRSTUVWXYZ--🍕--abcdefghijklmnopqrstuvwxyz--ABCDEFGHIJKLMNOPQRSTUVWXYZ"sv);
    }
}

TEST_CASE(repeated)
{
    {
        auto string1 = Utf16String::repeated('a', 0);
        EXPECT(string1.is_empty());

        auto string2 = Utf16String::repeated(0x03C9U, 0);
        EXPECT(string2.is_empty());

        auto string3 = Utf16String::repeated(0x10300, 0);
        EXPECT(string3.is_empty());
    }
    {
        auto string1 = Utf16String::repeated('a', 1);
        EXPECT_EQ(string1.length_in_code_units(), 1uz);
        EXPECT_EQ(string1, u"a"sv);

        auto string2 = Utf16String::repeated(0x03C9U, 1);
        EXPECT_EQ(string2.length_in_code_units(), 1uz);
        EXPECT_EQ(string2, u"ω"sv);

        auto string3 = Utf16String::repeated(0x10300, 1);
        EXPECT_EQ(string3.length_in_code_units(), 2uz);
        EXPECT_EQ(string3, u"𐌀"sv);
    }
    {
        auto string1 = Utf16String::repeated('a', 3);
        EXPECT_EQ(string1.length_in_code_units(), 3uz);
        EXPECT_EQ(string1, u"aaa"sv);

        auto string2 = Utf16String::repeated(0x03C9U, 3);
        EXPECT_EQ(string2.length_in_code_units(), 3uz);
        EXPECT_EQ(string2, u"ωωω"sv);

        auto string3 = Utf16String::repeated(0x10300, 3);
        EXPECT_EQ(string3.length_in_code_units(), 6uz);
        EXPECT_EQ(string3, u"𐌀𐌀𐌀"sv);
    }
    {
        auto string1 = Utf16String::repeated('a', 10);
        EXPECT_EQ(string1.length_in_code_units(), 10uz);
        EXPECT_EQ(string1, u"aaaaaaaaaa"sv);

        auto string2 = Utf16String::repeated(0x03C9U, 10);
        EXPECT_EQ(string2.length_in_code_units(), 10uz);
        EXPECT_EQ(string2, u"ωωωωωωωωωω"sv);

        auto string3 = Utf16String::repeated(0x10300, 10);
        EXPECT_EQ(string3.length_in_code_units(), 20uz);
        EXPECT_EQ(string3, u"𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀𐌀"sv);
    }

    EXPECT_DEATH("Creating a string from an invalid code point", (void)Utf16String::repeated(0xffffffff, 1));
}

TEST_CASE(from_string_builder)
{
    StringBuilder builder(StringBuilder::Mode::UTF16);
    builder.append_code_point('a');
    builder.append_code_point('b');
    builder.append_code_point(0x1f600);
    builder.append_code_point(0x10000);
    builder.append_code_point(0x1f355);
    builder.append_code_point('c');
    builder.append_code_point('d');

    auto string = builder.to_utf16_string();
    EXPECT_EQ(string.length_in_code_units(), 10uz);
    EXPECT_EQ(string.length_in_code_points(), 7uz);
    EXPECT_EQ(string, "ab😀𐀀🍕cd"sv);
}

TEST_CASE(from_ipc_stream)
{
    {
        auto data = "abc"sv;
        FixedMemoryStream stream { data.bytes() };

        auto string = TRY_OR_FAIL(Utf16String::from_ipc_stream(stream, data.length(), true));
        EXPECT(string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 3uz);
        EXPECT_EQ(string, data);
    }
    {
        auto data = "abcdefghijklmnopqrstuvwxyz"sv;
        FixedMemoryStream stream { data.bytes() };

        auto string = TRY_OR_FAIL(Utf16String::from_ipc_stream(stream, data.length(), true));
        EXPECT(string.is_ascii());
        EXPECT(string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 26uz);
        EXPECT_EQ(string, data);
    }
    {
        auto data = u"hello 😀 there!"sv;

        StringBuilder builder(StringBuilder::Mode::UTF16);
        builder.append(data);

        auto buffer = MUST(builder.to_byte_buffer());
        FixedMemoryStream stream { buffer.bytes() };

        auto string = TRY_OR_FAIL(Utf16String::from_ipc_stream(stream, data.length_in_code_units(), false));
        EXPECT(!string.is_ascii());
        EXPECT(!string.has_long_ascii_storage());
        EXPECT(!string.has_short_ascii_storage());
        EXPECT_EQ(string.length_in_code_units(), 15uz);
        EXPECT_EQ(string, data);
    }
    {
        auto data = "abc"sv;
        FixedMemoryStream stream { data.bytes() };

        auto result = Utf16String::from_ipc_stream(stream, data.length() + 1, true);
        EXPECT(result.is_error());
    }
    {
        auto data = u"😀"sv;

        StringBuilder builder(StringBuilder::Mode::UTF16);
        builder.append(data);

        auto buffer = MUST(builder.to_byte_buffer());
        FixedMemoryStream stream { buffer.bytes() };

        auto result = Utf16String::from_ipc_stream(stream, data.length_in_code_units(), true);
        EXPECT(result.is_error());
    }
    {
        auto data = u"hello 😀 there!"sv;

        StringBuilder builder(StringBuilder::Mode::UTF16);
        builder.append(data);

        auto buffer = MUST(builder.to_byte_buffer());
        FixedMemoryStream stream { buffer.bytes() };

        auto result = Utf16String::from_ipc_stream(stream, data.length_in_code_units(), true);
        EXPECT(result.is_error());
    }
}

TEST_CASE(to_lowercase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = "\u00DF"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u00DF"sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = "\u0130"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u0069\u0307"sv);

    // LATIN SMALL LIGATURE FF
    result = "\uFB00"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB00"sv);

    // LATIN SMALL LIGATURE FI
    result = "\uFB01"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB01"sv);

    // LATIN SMALL LIGATURE FL
    result = "\uFB02"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB02"sv);

    // LATIN SMALL LIGATURE FFI
    result = "\uFB03"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB03"sv);

    // LATIN SMALL LIGATURE FFL
    result = "\uFB04"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB04"sv);

    // LATIN SMALL LIGATURE LONG S T
    result = "\uFB05"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB05"sv);

    // LATIN SMALL LIGATURE ST
    result = "\uFB06"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\uFB06"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FB7"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u1FB7"sv);

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FC7"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u1FC7"sv);

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FF7"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u1FF7"sv);
}

TEST_CASE(to_lowercase_special_casing_sigma)
{
    auto result = "ABCI"_utf16.to_lowercase();
    EXPECT_EQ(result, u"abci"sv);

    // Sigma preceded by A
    result = "A\u03A3"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u03C2"sv);

    // Sigma preceded by FEMININE ORDINAL INDICATOR
    result = "\u00AA\u03A3"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u00AA\u03C2"sv);

    // Sigma preceded by ROMAN NUMERAL ONE
    result = "\u2160\u03A3"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u2170\u03C2"sv);

    // Sigma preceded by COMBINING GREEK YPOGEGRAMMENI
    result = "\u0345\u03A3"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u0345\u03C3"sv);

    // Sigma preceded by A and FULL STOP
    result = "A.\u03A3"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a.\u03C2"sv);

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR
    result = "A\u180E\u03A3"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u180E\u03C2"sv);

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR, followed by B
    result = "A\u180E\u03A3B"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u180E\u03C3b"sv);

    // Sigma followed by A
    result = "\u03A3A"_utf16.to_lowercase();
    EXPECT_EQ(result, u"\u03C3a"sv);

    // Sigma preceded by A, followed by MONGOLIAN VOWEL SEPARATOR
    result = "A\u03A3\u180E"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u03C2\u180E"sv);

    // Sigma preceded by A, followed by MONGOLIAN VOWEL SEPARATOR and B
    result = "A\u03A3\u180EB"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u03C3\u180Eb"sv);

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR, followed by MONGOLIAN VOWEL SEPARATOR
    result = "A\u180E\u03A3\u180E"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u180E\u03C2\u180E"sv);

    // Sigma preceded by A and MONGOLIAN VOWEL SEPARATOR, followed by MONGOLIAN VOWEL SEPARATOR and B
    result = "A\u180E\u03A3\u180EB"_utf16.to_lowercase();
    EXPECT_EQ(result, u"a\u180E\u03C3\u180Eb"sv);
}

TEST_CASE(to_lowercase_special_casing_i)
{
    // LATIN CAPITAL LETTER I
    auto result = "I"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"i"sv);

    result = "I"_utf16.to_lowercase("az"sv);
    EXPECT_EQ(result, u"\u0131"sv);

    result = "I"_utf16.to_lowercase("tr"sv);
    EXPECT_EQ(result, u"\u0131"sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = "\u0130"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"\u0069\u0307"sv);

    result = "\u0130"_utf16.to_lowercase("az"sv);
    EXPECT_EQ(result, u"i"sv);

    result = "\u0130"_utf16.to_lowercase("tr"sv);
    EXPECT_EQ(result, u"i"sv);

    // LATIN CAPITAL LETTER I followed by COMBINING DOT ABOVE
    result = "I\u0307"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"i\u0307"sv);

    result = "I\u0307"_utf16.to_lowercase("az"sv);
    EXPECT_EQ(result, u"i"sv);

    result = "I\u0307"_utf16.to_lowercase("tr"sv);
    EXPECT_EQ(result, u"i"sv);

    // LATIN CAPITAL LETTER I followed by combining class 0 and COMBINING DOT ABOVE
    result = "IA\u0307"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"ia\u0307"sv);

    result = "IA\u0307"_utf16.to_lowercase("az"sv);
    EXPECT_EQ(result, u"\u0131a\u0307"sv);

    result = "IA\u0307"_utf16.to_lowercase("tr"sv);
    EXPECT_EQ(result, u"\u0131a\u0307"sv);
}

TEST_CASE(to_lowercase_special_casing_more_above)
{
    // LATIN CAPITAL LETTER I
    auto result = "I"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"i"sv);

    result = "I"_utf16.to_lowercase("lt"sv);
    EXPECT_EQ(result, u"i"sv);

    // LATIN CAPITAL LETTER J
    result = "J"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"j"sv);

    result = "J"_utf16.to_lowercase("lt"sv);
    EXPECT_EQ(result, u"j"sv);

    // LATIN CAPITAL LETTER I WITH OGONEK
    result = "\u012e"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"\u012f"sv);

    result = "\u012e"_utf16.to_lowercase("lt"sv);
    EXPECT_EQ(result, u"\u012f"sv);

    // LATIN CAPITAL LETTER I followed by COMBINING GRAVE ACCENT
    result = "I\u0300"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"i\u0300"sv);

    result = "I\u0300"_utf16.to_lowercase("lt"sv);
    EXPECT_EQ(result, u"i\u0307\u0300"sv);

    // LATIN CAPITAL LETTER J followed by COMBINING GRAVE ACCENT
    result = "J\u0300"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"j\u0300"sv);

    result = "J\u0300"_utf16.to_lowercase("lt"sv);
    EXPECT_EQ(result, u"j\u0307\u0300"sv);

    // LATIN CAPITAL LETTER I WITH OGONEK followed by COMBINING GRAVE ACCENT
    result = "\u012e\u0300"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"\u012f\u0300"sv);

    result = "\u012e\u0300"_utf16.to_lowercase("lt"sv);
    EXPECT_EQ(result, u"\u012f\u0307\u0300"sv);
}

TEST_CASE(to_lowercase_special_casing_not_before_dot)
{
    // LATIN CAPITAL LETTER I
    auto result = "I"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"i"sv);

    result = "I"_utf16.to_lowercase("az"sv);
    EXPECT_EQ(result, u"\u0131"sv);

    result = "I"_utf16.to_lowercase("tr"sv);
    EXPECT_EQ(result, u"\u0131"sv);

    // LATIN CAPITAL LETTER I followed by COMBINING DOT ABOVE
    result = "I\u0307"_utf16.to_lowercase("en"sv);
    EXPECT_EQ(result, u"i\u0307"sv);

    result = "I\u0307"_utf16.to_lowercase("az"sv);
    EXPECT_EQ(result, u"i"sv);

    result = "I\u0307"_utf16.to_lowercase("tr"sv);
    EXPECT_EQ(result, u"i"sv);
}

TEST_CASE(to_uppercase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = "\u00DF"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0053\u0053"sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = "\u0130"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0130"sv);

    // LATIN SMALL LIGATURE FF
    result = "\uFB00"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0046\u0046"sv);

    // LATIN SMALL LIGATURE FI
    result = "\uFB01"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0046\u0049"sv);

    // LATIN SMALL LIGATURE FL
    result = "\uFB02"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0046\u004C"sv);

    // LATIN SMALL LIGATURE FFI
    result = "\uFB03"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0046\u0046\u0049"sv);

    // LATIN SMALL LIGATURE FFL
    result = "\uFB04"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0046\u0046\u004C"sv);

    // LATIN SMALL LIGATURE LONG S T
    result = "\uFB05"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0053\u0054"sv);

    // LATIN SMALL LIGATURE ST
    result = "\uFB06"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0053\u0054"sv);

    // GREEK SMALL LETTER IOTA WITH DIALYTIKA AND TONOS
    result = "\u0390"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0399\u0308\u0301"sv);

    // GREEK SMALL LETTER UPSILON WITH DIALYTIKA AND TONOS
    result = "\u03B0"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u03A5\u0308\u0301"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FB7"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0391\u0342\u0399"sv);

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FC7"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u0397\u0342\u0399"sv);

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FF7"_utf16.to_uppercase();
    EXPECT_EQ(result, u"\u03A9\u0342\u0399"sv);
}

TEST_CASE(to_uppercase_special_casing_soft_dotted)
{
    // LATIN SMALL LETTER I
    auto result = "i"_utf16.to_uppercase("en"sv);
    EXPECT_EQ(result, u"I"sv);

    result = "i"_utf16.to_uppercase("lt"sv);
    EXPECT_EQ(result, u"I"sv);

    // LATIN SMALL LETTER J
    result = "j"_utf16.to_uppercase("en"sv);
    EXPECT_EQ(result, u"J"sv);

    result = "j"_utf16.to_uppercase("lt"sv);
    EXPECT_EQ(result, u"J"sv);

    // LATIN SMALL LETTER I followed by COMBINING DOT ABOVE
    result = "i\u0307"_utf16.to_uppercase("en"sv);
    EXPECT_EQ(result, u"I\u0307"sv);

    result = "i\u0307"_utf16.to_uppercase("lt"sv);
    EXPECT_EQ(result, u"I"sv);

    // LATIN SMALL LETTER J followed by COMBINING DOT ABOVE
    result = "j\u0307"_utf16.to_uppercase("en"sv);
    EXPECT_EQ(result, u"J\u0307"sv);

    result = "j\u0307"_utf16.to_uppercase("lt"sv);
    EXPECT_EQ(result, u"J"sv);
}

TEST_CASE(to_titlecase)
{
    EXPECT_EQ(""_utf16.to_titlecase(), ""sv);
    EXPECT_EQ(" "_utf16.to_titlecase(), " "sv);
    EXPECT_EQ(" - "_utf16.to_titlecase(), " - "sv);

    EXPECT_EQ("a"_utf16.to_titlecase(), "A"sv);
    EXPECT_EQ("A"_utf16.to_titlecase(), "A"sv);
    EXPECT_EQ(" a"_utf16.to_titlecase(), " A"sv);
    EXPECT_EQ("a "_utf16.to_titlecase(), "A "sv);

    EXPECT_EQ("ab"_utf16.to_titlecase(), "Ab"sv);
    EXPECT_EQ("Ab"_utf16.to_titlecase(), "Ab"sv);
    EXPECT_EQ("aB"_utf16.to_titlecase(), "Ab"sv);
    EXPECT_EQ("AB"_utf16.to_titlecase(), "Ab"sv);
    EXPECT_EQ(" ab"_utf16.to_titlecase(), " Ab"sv);
    EXPECT_EQ("ab "_utf16.to_titlecase(), "Ab "sv);

    EXPECT_EQ("foo bar baz"_utf16.to_titlecase(), "Foo Bar Baz"sv);
    EXPECT_EQ("foo \n \r bar \t baz"_utf16.to_titlecase(), "Foo \n \r Bar \t Baz"sv);
    EXPECT_EQ("f\"oo\" b'ar'"_utf16.to_titlecase(), "F\"Oo\" B'ar'"sv);
}

TEST_CASE(to_titlecase_unconditional_special_casing)
{
    // LATIN SMALL LETTER SHARP S
    auto result = "\u00DF"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0053\u0073"sv);

    // LATIN CAPITAL LETTER I WITH DOT ABOVE
    result = "\u0130"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0130"sv);

    // LATIN SMALL LIGATURE FF
    result = "\uFB00"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0046\u0066"sv);

    // LATIN SMALL LIGATURE FI
    result = "\uFB01"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0046\u0069"sv);

    // LATIN SMALL LIGATURE FL
    result = "\uFB02"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0046\u006C"sv);

    // LATIN SMALL LIGATURE FFI
    result = "\uFB03"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0046\u0066\u0069"sv);

    // LATIN SMALL LIGATURE FFL
    result = "\uFB04"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0046\u0066\u006C"sv);

    // LATIN SMALL LIGATURE LONG S T
    result = "\uFB05"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0053\u0074"sv);

    // LATIN SMALL LIGATURE ST
    result = "\uFB06"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0053\u0074"sv);

    // GREEK SMALL LETTER IOTA WITH DIALYTIKA AND TONOS
    result = "\u0390"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0399\u0308\u0301"sv);

    // GREEK SMALL LETTER UPSILON WITH DIALYTIKA AND TONOS
    result = "\u03B0"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u03A5\u0308\u0301"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FB7"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0391\u0342\u0345"sv);

    // GREEK SMALL LETTER ETA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FC7"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u0397\u0342\u0345"sv);

    // GREEK SMALL LETTER OMEGA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FF7"_utf16.to_titlecase();
    EXPECT_EQ(result, u"\u03A9\u0342\u0345"sv);
}

TEST_CASE(to_titlecase_special_casing_i)
{
    // LATIN SMALL LETTER I
    auto result = "i"_utf16.to_titlecase("en"sv);
    EXPECT_EQ(result, u"I"sv);

    result = "i"_utf16.to_titlecase("az"sv);
    EXPECT_EQ(result, u"\u0130"sv);

    result = "i"_utf16.to_titlecase("tr"sv);
    EXPECT_EQ(result, u"\u0130"sv);
}

TEST_CASE(to_casefold)
{
    for (u8 code_point = 0; code_point < 0x80; ++code_point) {
        auto ascii = to_ascii_lowercase(code_point);
        auto unicode = Utf16String::from_code_point(code_point).to_casefold();

        EXPECT_EQ(unicode.length_in_code_units(), 1uz);
        EXPECT_EQ(unicode.code_unit_at(0), ascii);
    }

    // LATIN SMALL LETTER SHARP S
    auto result = "\u00DF"_utf16.to_casefold();
    EXPECT_EQ(result, u"\u0073\u0073"sv);

    // GREEK SMALL LETTER ALPHA WITH YPOGEGRAMMENI
    result = "\u1FB3"_utf16.to_casefold();
    EXPECT_EQ(result, u"\u03B1\u03B9"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI
    result = "\u1FB6"_utf16.to_casefold();
    EXPECT_EQ(result, u"\u03B1\u0342"sv);

    // GREEK SMALL LETTER ALPHA WITH PERISPOMENI AND YPOGEGRAMMENI
    result = "\u1FB7"_utf16.to_casefold();
    EXPECT_EQ(result, u"\u03B1\u0342\u03B9"sv);
}

TEST_CASE(trim)
{
    auto expect_same_string = [](Utf16String const& string, Utf16String const& result) {
        EXPECT_EQ(string, result);

        VERIFY(string.has_ascii_storage() == result.has_ascii_storage());
        auto string_view = string.utf16_view();
        auto result_view = result.utf16_view();

        if (string.has_ascii_storage())
            EXPECT_EQ(string_view.ascii_span().data(), result_view.ascii_span().data());
        else
            EXPECT_EQ(string_view.utf16_span().data(), result_view.utf16_span().data());
    };

    Utf16View whitespace { u" "sv };
    {
        auto string = u"looooong word"_utf16;
        expect_same_string(string, string.trim(whitespace, TrimMode::Both));
        expect_same_string(string, string.trim(whitespace, TrimMode::Left));
        expect_same_string(string, string.trim(whitespace, TrimMode::Right));
    }
    {
        auto string = u"   looooong word"_utf16;
        EXPECT_EQ(string.trim(whitespace, TrimMode::Both), u"looooong word"sv);
        EXPECT_EQ(string.trim(whitespace, TrimMode::Left), u"looooong word"sv);
        expect_same_string(string, string.trim(whitespace, TrimMode::Right));
    }
    {
        auto string = u"looooong word   "_utf16;
        EXPECT_EQ(string.trim(whitespace, TrimMode::Both), u"looooong word"sv);
        expect_same_string(string, string.trim(whitespace, TrimMode::Left));
        EXPECT_EQ(string.trim(whitespace, TrimMode::Right), u"looooong word"sv);
    }
    {
        auto string = u"   looooong word   "_utf16;
        EXPECT_EQ(string.trim(whitespace, TrimMode::Both), u"looooong word"sv);
        EXPECT_EQ(string.trim(whitespace, TrimMode::Left), u"looooong word   "sv);
        EXPECT_EQ(string.trim(whitespace, TrimMode::Right), u"   looooong word"sv);
    }
    {
        auto string = u"   \u180E   "_utf16;
        EXPECT_EQ(string.trim(whitespace, TrimMode::Both), u"\u180E"sv);
        EXPECT_EQ(string.trim(whitespace, TrimMode::Left), u"\u180E   "sv);
        EXPECT_EQ(string.trim(whitespace, TrimMode::Right), u"   \u180E"sv);
    }
    {
        auto string = u"😀wfh😀"_utf16;
        EXPECT_EQ(string.trim(u"😀"sv, TrimMode::Both), u"wfh"sv);
        EXPECT_EQ(string.trim(u"😀"sv, TrimMode::Left), u"wfh😀"sv);
        EXPECT_EQ(string.trim(u"😀"sv, TrimMode::Right), u"😀wfh"sv);

        expect_same_string(string, string.trim(whitespace, TrimMode::Both));
        expect_same_string(string, string.trim(whitespace, TrimMode::Left));
        expect_same_string(string, string.trim(whitespace, TrimMode::Right));
    }
}

TEST_CASE(copy_operations)
{
    auto test = [](Utf16String const& string1) {
        auto original = make_copy(string1);

        // Copy constructor.
        Utf16String const& string2(string1);

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
    EXPECT_EQ("😀"sv, "😀"_utf16);
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

TEST_CASE(optional)
{
    static_assert(AssertSize<Optional<Utf16String>, sizeof(Utf16String)>());

    Optional<Utf16String> string;
    EXPECT(!string.has_value());

    string = "ascii"_utf16;
    EXPECT(string.has_value());
    EXPECT_EQ(string.value(), "ascii"sv);

    auto released = string.release_value();
    EXPECT(!string.has_value());
    EXPECT_EQ(released, "ascii"sv);

    string = u"well 😀 hello"_utf16;
    EXPECT(string.has_value());
    EXPECT_EQ(string.value(), u"well 😀 hello"sv);

    released = string.release_value();
    EXPECT(!string.has_value());
    EXPECT_EQ(released, u"well 😀 hello"sv);
}
