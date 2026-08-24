/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>

namespace Web::CSS::Parser {

static void compare_parsed_syntax_dump_to_string(Utf16View source, StringView expected)
{
    auto syntax = parse_as_syntax(source);
    EXPECT(syntax != nullptr);
    if (syntax)
        EXPECT_EQ(syntax->dump(), expected);
}

static void expect_dumps_equal(Utf16View lhs_source, Utf16View rhs_source)
{
    auto lhs = parse_as_syntax(lhs_source);
    auto rhs = parse_as_syntax(rhs_source);
    EXPECT(lhs != nullptr);
    EXPECT(rhs != nullptr);
    if (lhs && rhs)
        EXPECT_EQ(lhs->dump(), rhs->dump());
}

TEST_CASE(single_universal)
{
    compare_parsed_syntax_dump_to_string("*"_utf16, "Universal\n"sv);
}

TEST_CASE(single_ident)
{
    compare_parsed_syntax_dump_to_string("thing"_utf16, "Ident: thing\n"sv);
}

TEST_CASE(single_type)
{
    for (auto type : { "angle"sv, "color"sv, "custom-ident"sv, "image"sv, "integer"sv, "length"sv,
             "length-percentage"sv, "number"sv, "percentage"sv, "resolution"sv, "string"sv, "time"sv,
             "url"sv, "transform-function"sv }) {
        auto source = Utf16String::formatted("<{}>", type);
        auto expected = MUST(String::formatted("Type: {}\n", type));
        compare_parsed_syntax_dump_to_string(source, expected);
    }
}

TEST_CASE(multiple_keywords)
{
    compare_parsed_syntax_dump_to_string("well|hello|friends"_utf16,
        R"~~~(Alternatives:
  Ident: well
  Ident: hello
  Ident: friends
)~~~"sv);
}

TEST_CASE(repeated_type)
{
    compare_parsed_syntax_dump_to_string("<number>+"_utf16,
        R"~~~(Multiplier:
  Type: number
)~~~"sv);
}

TEST_CASE(repeated_with_commas)
{
    compare_parsed_syntax_dump_to_string("<number>#"_utf16,
        R"~~~(CommaSeparatedMultiplier:
  Type: number
)~~~"sv);
}

TEST_CASE(complex)
{
    compare_parsed_syntax_dump_to_string("well|<number>+|<string>#"_utf16,
        R"~~~(Alternatives:
  Ident: well
  Multiplier:
    Type: number
  CommaSeparatedMultiplier:
    Type: string
)~~~"sv);
}

TEST_CASE(syntax_string)
{
    expect_dumps_equal("<number>"_utf16, "\"<number>\""_utf16);
    expect_dumps_equal("well | <number>+ | <string>#"_utf16, "\"well | <number>+ | <string>#\""_utf16);
}

TEST_CASE(invalid)
{
    for (auto source : { ""sv, " "sv, "<number"sv, "thing|"sv, "*|*"sv, "<transform-list>+"sv,
             "<transform-list>#"sv, "<woozle>"sv, "<number> <integer>"sv, "thingy whatsit"sv,
             "<number> +"sv, "<number> #"sv }) {
        EXPECT(!parse_as_syntax(Utf16String::from_utf8_without_validation(source)));
    }
}

}
