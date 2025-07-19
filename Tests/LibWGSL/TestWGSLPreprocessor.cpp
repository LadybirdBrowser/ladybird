/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWGSL/Preprocessor.h>

TEST_CASE(empty_input)
{
    WGSL::Preprocessor preprocessor(""sv);
    EXPECT_NO_DEATH("Empty input string should return empty string", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "");
    }());
}

TEST_CASE(no_comments)
{
    constexpr auto input = "struct Vertex { position: vec4f; }"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Input with no comments should be unchanged", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), input);
    }());
}

TEST_CASE(single_line_comment)
{
    constexpr auto input = "var x: f32; // This is a comment\nlet y: f32;"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Single line-ending comment (//) should be replaced with a space, preserving newline", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;  \nlet y: f32;");
    }());
}

TEST_CASE(multiple_line_comments)
{
    constexpr auto input = "var x: f32; // Comment 1\n// Comment 2\nlet y: f32; // Comment 3"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Multiple line-ending comments should each be replaced with a space, preserving newlines", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;  \n \nlet y: f32;  ");
    }());
}

TEST_CASE(single_block_comment)
{
    constexpr auto input = "var x: f32; /* This is a block comment */ let y: f32;"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Single block comment (/* */) should be replaced with a space", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;   let y: f32;");
    }());
}

TEST_CASE(nested_block_comments)
{
    constexpr auto input = "var x: f32; /* Outer /* Inner */ comment */ let y: f32;"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Nested block comments should be replaced with a space", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;   let y: f32;");
    }());
}

TEST_CASE(mixed_comments)
{
    constexpr auto input = "var x: f32; // Line comment\n/* Block comment */ let y: f32; /* Another */ // End"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Mixed line-ending and block comments should be replaced with spaces, preserving newlines", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;  \n  let y: f32;    ");
    }());
}

TEST_CASE(comment_at_start)
{
    constexpr auto input = "// Start comment\nvar x: f32;"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Comment at the start of input should be replaced with a space, preserving newline", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), " \nvar x: f32;");
    }());
}

TEST_CASE(comment_at_end)
{
    constexpr auto input = "var x: f32; /* End comment */"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Comment at the end of input should be replaced with a space", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;  ");
    }());
}

TEST_CASE(comments_with_whitespace)
{
    constexpr auto input = "var x: f32;   /* Comment */   let y: f32;"sv;
    WGSL::Preprocessor preprocessor(input);
    EXPECT_NO_DEATH("Comments surrounded by whitespace should be replaced with a single space", [&] {
        EXPECT_EQ(MUST(preprocessor.process()), "var x: f32;       let y: f32;");
    }());
}

TEST_CASE(unterminated_block)
{
    constexpr auto input = "var x: f32; /* Unterminated comment"sv;
    WGSL::Preprocessor preprocessor(input);
    auto result = preprocessor.process();
    if (!result.is_error()) {
        FAIL("Unterminated block comment is not valid");
        return;
    }
    EXPECT_EQ(result.error().string_literal(), "Unterminated block comment");
}

TEST_CASE(nested_unterminated_block)
{
    constexpr auto input = "var x: f32; /* Outer /* Inner unterminated */"sv;
    WGSL::Preprocessor preprocessor(input);
    auto result = preprocessor.process();
    if (!result.is_error()) {
        FAIL("Unterminated nested block comment is not valid");
        return;
    }
    EXPECT_EQ(result.error().string_literal(), "Unterminated block comment");
}

TEST_CASE(unterminated_block_at_eof)
{
    constexpr auto input = "var x: f32; /* Comment at EOF"sv;
    WGSL::Preprocessor preprocessor(input);
    auto result = preprocessor.process();
    if (!result.is_error()) {
        FAIL("Unterminated block comment at EOF is not valid");
        return;
    }
    EXPECT_EQ(result.error().string_literal(), "Unterminated block comment");
}
