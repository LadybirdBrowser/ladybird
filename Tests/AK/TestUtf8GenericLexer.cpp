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

// FIXME: Add remaining tests
