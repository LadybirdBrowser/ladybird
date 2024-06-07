/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteBuffer.h>
#include <AK/Utf8View.h>

TEST_CASE(decode_ascii)
{
    Utf8View utf8 { "Hello World!11"sv };
    EXPECT(utf8.validate());

    u32 expected[] = { 72, 101, 108, 108, 111, 32, 87, 111, 114, 108, 100, 33, 49, 49 };
    size_t expected_size = sizeof(expected) / sizeof(expected[0]);

    size_t i = 0;
    for (u32 code_point : utf8) {
        VERIFY(i < expected_size);
        EXPECT_EQ(code_point, expected[i]);
        i++;
    }
    EXPECT_EQ(i, expected_size);
}

TEST_CASE(decode_utf8)
{
    Utf8View utf8 { "Привет, мир! 😀 γειά σου κόσμος こんにちは世界"sv };
    size_t valid_bytes;
    EXPECT(utf8.validate(valid_bytes));
    EXPECT(valid_bytes == (size_t)utf8.byte_length());

    u32 expected[] = { 1055, 1088, 1080, 1074, 1077, 1090, 44, 32, 1084, 1080, 1088, 33, 32, 128512, 32, 947, 949, 953, 940, 32, 963, 959, 965, 32, 954, 972, 963, 956, 959, 962, 32, 12371, 12435, 12395, 12385, 12399, 19990, 30028 };
    ByteString expected_underlying_bytes[] = { "П", "р", "и", "в", "е", "т", ",", " ", "м", "и", "р", "!", " ", "😀", " ", "γ", "ε", "ι", "ά", " ", "σ", "ο", "υ", " ", "κ", "ό", "σ", "μ", "ο", "ς", " ", "こ", "ん", "に", "ち", "は", "世", "界" };
    size_t expected_size = sizeof(expected) / sizeof(expected[0]);

    size_t i = 0;
    for (auto it = utf8.begin(); it != utf8.end(); ++it) {
        u32 code_point = *it;
        VERIFY(i < expected_size);
        EXPECT_EQ(code_point, expected[i]);
        EXPECT_EQ(it.underlying_code_point_bytes(), expected_underlying_bytes[i].bytes());
        i++;
    }
    EXPECT_EQ(i, expected_size);
}

TEST_CASE(validate_invalid_ut8)
{
    size_t valid_bytes;
    char invalid_utf8_1[] = { 42, 35, (char)182, 9 };
    Utf8View utf8_1 { StringView { invalid_utf8_1, 4 } };
    EXPECT(!utf8_1.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    char invalid_utf8_2[] = { 42, 35, (char)208, (char)208 };
    Utf8View utf8_2 { StringView { invalid_utf8_2, 4 } };
    EXPECT(!utf8_2.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    char invalid_utf8_3[] = { (char)208 };
    Utf8View utf8_3 { StringView { invalid_utf8_3, 1 } };
    EXPECT(!utf8_3.validate(valid_bytes));
    EXPECT(valid_bytes == 0);

    char invalid_utf8_4[] = { (char)208, 35 };
    Utf8View utf8_4 { StringView { invalid_utf8_4, 2 } };
    EXPECT(!utf8_4.validate(valid_bytes));
    EXPECT(valid_bytes == 0);

    char invalid_utf8_5[] = { (char)0xf4, (char)0x8f, (char)0xbf, (char)0xc0 }; // U+110000
    Utf8View utf8_5 { StringView { invalid_utf8_5, 4 } };
    EXPECT(!utf8_5.validate(valid_bytes));
    EXPECT(valid_bytes == 0);

    char invalid_utf8_6[] = { (char)0xf4, (char)0xa1, (char)0xb0, (char)0xbd }; // U+121c3d
    Utf8View utf8_6 { StringView { invalid_utf8_6, 4 } };
    EXPECT(!utf8_6.validate(valid_bytes));
    EXPECT(valid_bytes == 0);

    char invalid_utf8_7[] = { (char)0xed, (char)0xa0, (char)0x80 }; // U+d800
    Utf8View utf8_7 { StringView { invalid_utf8_7, 3 } };
    EXPECT(!utf8_7.validate(valid_bytes, Utf8View::AllowSurrogates::No));
    EXPECT(valid_bytes == 0);
}

TEST_CASE(validate_overlong_utf8)
{
    size_t valid_bytes = 0;

    // Overlong 2-byte encoding of U+002F
    char invalid_utf8_1[] = { 42, 35, static_cast<char>(0xc0), static_cast<char>(0xaf) };
    Utf8View utf8_1 { StringView { invalid_utf8_1, sizeof(invalid_utf8_1) } };
    EXPECT(!utf8_1.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    // Overlong 3-byte encoding of U+002F
    char invalid_utf8_2[] = { 42, 35, static_cast<char>(0xe0), static_cast<char>(0x80), static_cast<char>(0xaf) };
    Utf8View utf8_2 { StringView { invalid_utf8_2, sizeof(invalid_utf8_2) } };
    EXPECT(!utf8_2.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    // Overlong 4-byte encoding of U+002F
    char invalid_utf8_3[] = { 42, 35, static_cast<char>(0xf0), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0xaf) };
    Utf8View utf8_3 { StringView { invalid_utf8_3, sizeof(invalid_utf8_3) } };
    EXPECT(!utf8_3.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    // Overlong 3-byte encoding of U+00FF
    char invalid_utf8_4[] = { 42, 35, static_cast<char>(0xe0), static_cast<char>(0x83), static_cast<char>(0xbf) };
    Utf8View utf8_4 { StringView { invalid_utf8_4, sizeof(invalid_utf8_4) } };
    EXPECT(!utf8_4.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    // Overlong 4-byte encoding of U+00FF
    char invalid_utf8_5[] = { 42, 35, static_cast<char>(0xf0), static_cast<char>(0x80), static_cast<char>(0x83), static_cast<char>(0xbf) };
    Utf8View utf8_5 { StringView { invalid_utf8_5, sizeof(invalid_utf8_5) } };
    EXPECT(!utf8_5.validate(valid_bytes));
    EXPECT(valid_bytes == 2);

    // Overlong 4-byte encoding of U+0FFF
    char invalid_utf8_6[] = { 42, 35, static_cast<char>(0xf0), static_cast<char>(0x8f), static_cast<char>(0xbf), static_cast<char>(0xbf) };
    Utf8View utf8_6 { StringView { invalid_utf8_6, sizeof(invalid_utf8_6) } };
    EXPECT(!utf8_6.validate(valid_bytes));
    EXPECT(valid_bytes == 2);
}

TEST_CASE(iterate_utf8)
{
    Utf8View view("Some weird characters \u00A9\u266A\uA755"sv);
    Utf8CodePointIterator iterator = view.begin();

    EXPECT(*iterator == 'S');
    EXPECT(iterator.peek().has_value() && iterator.peek().value() == 'S');
    EXPECT(iterator.peek(0).has_value() && iterator.peek(0).value() == 'S');
    EXPECT(iterator.peek(1).has_value() && iterator.peek(1).value() == 'o');
    EXPECT(iterator.peek(22).has_value() && iterator.peek(22).value() == 0x00A9);
    EXPECT(iterator.peek(24).has_value() && iterator.peek(24).value() == 0xA755);
    EXPECT(!iterator.peek(25).has_value());

    ++iterator;

    EXPECT(*iterator == 'o');
    EXPECT(iterator.peek(23).has_value() && iterator.peek(23).value() == 0xA755);

    for (size_t i = 0; i < 23; ++i)
        ++iterator;

    EXPECT(!iterator.done());
    EXPECT(*iterator == 0xA755);
    EXPECT(iterator.peek().has_value() && iterator.peek().value() == 0xA755);
    EXPECT(!iterator.peek(1).has_value());

    ++iterator;

    EXPECT(iterator.done());
    EXPECT(!iterator.peek(0).has_value());
    EXPECT_CRASH("Dereferencing Utf8CodePointIterator which is already done.", [&iterator] {
        *iterator;
        return Test::Crash::Failure::DidNotCrash;
    });
}

TEST_CASE(decode_invalid_ut8)
{
    // Test case 1 : Getting an extension byte as first byte of the code point
    {
        char raw_data[] = { 'a', 'b', (char)0xA0, 'd' };
        Utf8View view { StringView { raw_data, 4 } };
        u32 expected_characters[] = { 'a', 'b', 0xFFFD, 'd' };
        ByteString expected_underlying_bytes[] = { "a", "b", "\xA0", "d" };
        size_t expected_size = sizeof(expected_characters) / sizeof(expected_characters[0]);
        size_t i = 0;
        for (auto it = view.begin(); it != view.end(); ++it) {
            u32 code_point = *it;
            VERIFY(i < expected_size);
            EXPECT_EQ(code_point, expected_characters[i]);
            EXPECT_EQ(it.underlying_code_point_bytes(), expected_underlying_bytes[i].bytes());
            i++;
        }
        VERIFY(i == expected_size);
    }

    // Test case 2 : Getting a non-extension byte when an extension byte is expected
    {
        char raw_data[] = { 'a', 'b', (char)0xC0, 'd', 'e' };
        Utf8View view { StringView { raw_data, 5 } };
        u32 expected_characters[] = { 'a', 'b', 0xFFFD, 'd', 'e' };
        ByteString expected_underlying_bytes[] = { "a", "b", "\xC0", "d", "e" };
        size_t expected_size = sizeof(expected_characters) / sizeof(expected_characters[0]);
        size_t i = 0;
        for (auto it = view.begin(); it != view.end(); ++it) {
            u32 code_point = *it;
            VERIFY(i < expected_size);
            EXPECT_EQ(code_point, expected_characters[i]);
            EXPECT_EQ(it.underlying_code_point_bytes(), expected_underlying_bytes[i].bytes());
            i++;
        }
        VERIFY(i == expected_size);
    }

    // Test case 3 : Not enough bytes before the end of the string
    {
        char raw_data[] = { 'a', 'b', (char)0x90, 'd' };
        Utf8View view { StringView { raw_data, 4 } };
        u32 expected_characters[] = { 'a', 'b', 0xFFFD, 'd' };
        ByteString expected_underlying_bytes[] = { "a", "b", "\x90", "d" };
        size_t expected_size = sizeof(expected_characters) / sizeof(expected_characters[0]);
        size_t i = 0;
        for (auto it = view.begin(); it != view.end(); ++it) {
            u32 code_point = *it;
            VERIFY(i < expected_size);
            EXPECT_EQ(code_point, expected_characters[i]);
            EXPECT_EQ(it.underlying_code_point_bytes(), expected_underlying_bytes[i].bytes());
            i++;
        }
        VERIFY(i == expected_size);
    }

    // Test case 4 : Not enough bytes at the end of the string
    {
        char raw_data[] = { 'a', 'b', 'c', (char)0x90 };
        Utf8View view { StringView { raw_data, 4 } };
        u32 expected_characters[] = { 'a', 'b', 'c', 0xFFFD };
        ByteString expected_underlying_bytes[] = { "a", "b", "c", "\x90" };
        size_t expected_size = sizeof(expected_characters) / sizeof(expected_characters[0]);
        size_t i = 0;
        for (auto it = view.begin(); it != view.end(); ++it) {
            u32 code_point = *it;
            VERIFY(i < expected_size);
            EXPECT_EQ(code_point, expected_characters[i]);
            EXPECT_EQ(it.underlying_code_point_bytes(), expected_underlying_bytes[i].bytes());
            i++;
        }
        VERIFY(i == expected_size);
    }

    // Test case 5 : Oversized four-byte sequence (e.g. U+123456)
    {
        // Want to encode: (000)1 0010 0011 0100 0101 0110
        // Into mask: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        // Shifted:        100   100011   010001   010110
        // Result:    11110100 10100011 10010001 10010110
        char raw_data[] = { 'a', (char)0xF4, (char)0xA3, (char)0x91, (char)0x96, 'b' };
        Utf8View view { StringView { raw_data, 6 } };
        // This definition seems to suggest that we should instead output multiple replacement characters:
        // https://encoding.spec.whatwg.org/#ref-for-concept-stream-prepend②
        // This is supported by the plaintext description and example collection, which annoyingly does not give an example of how to deal with this:
        // https://www.unicode.org/versions/Unicode14.0.0/ch03.pdf , section "U+FFFD Substitution of Maximal Subparts"
        // However, that would go against how we deal with several other kinds of errors, so we stick to emitting only one U+FFFD.
        u32 expected_characters[] = { 'a', 0xFFFD, 'b' };
        ByteString expected_underlying_bytes[] = { "a", "\xF4\xA3\x91\x96", "b" };
        size_t expected_size = sizeof(expected_characters) / sizeof(expected_characters[0]);
        size_t i = 0;
        for (auto it = view.begin(); it != view.end(); ++it) {
            u32 code_point = *it;
            VERIFY(i < expected_size);
            EXPECT_EQ(code_point, expected_characters[i]);
            EXPECT_EQ(it.underlying_code_point_bytes(), expected_underlying_bytes[i].bytes());
            i++;
        }
        VERIFY(i == expected_size);
    }
}

TEST_CASE(trim)
{
    Utf8View whitespace { " "sv };
    {
        Utf8View view { "word"sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both).as_string(), "word");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left).as_string(), "word");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right).as_string(), "word");
    }
    {
        Utf8View view { "   word"sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both).as_string(), "word");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left).as_string(), "word");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right).as_string(), "   word");
    }
    {
        Utf8View view { "word   "sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both).as_string(), "word");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left).as_string(), "word   ");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right).as_string(), "word");
    }
    {
        Utf8View view { "   word   "sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both).as_string(), "word");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left).as_string(), "word   ");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right).as_string(), "   word");
    }
    {
        Utf8View view { "\u180E"sv };
        EXPECT_EQ(view.trim(whitespace, TrimMode::Both).as_string(), "\u180E");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Left).as_string(), "\u180E");
        EXPECT_EQ(view.trim(whitespace, TrimMode::Right).as_string(), "\u180E");
    }
}
