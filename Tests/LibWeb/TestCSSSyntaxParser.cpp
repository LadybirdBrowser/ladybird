/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefPtr.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>

namespace Web::CSS::Parser {

static void compare_parsed_syntax_serialization(Utf16View source, Utf16View expected)
{
    auto syntax = parse_as_syntax(source);
    EXPECT(syntax.has_value());
    if (syntax.has_value())
        EXPECT_EQ(syntax->serialize(), expected);
}

static void expect_serializations_equal(Utf16View lhs_source, Utf16View rhs_source)
{
    auto lhs = parse_as_syntax(lhs_source);
    auto rhs = parse_as_syntax(rhs_source);
    EXPECT(lhs.has_value());
    EXPECT(rhs.has_value());
    if (lhs.has_value() && rhs.has_value())
        EXPECT_EQ(lhs->serialize(), rhs->serialize());
}

TEST_CASE(single_universal)
{
    compare_parsed_syntax_serialization("*"_utf16, "*"_utf16);
}

TEST_CASE(single_ident)
{
    compare_parsed_syntax_serialization("thing"_utf16, "thing"_utf16);
}

TEST_CASE(single_type)
{
    for (auto type : { "angle"sv, "color"sv, "custom-ident"sv, "image"sv, "integer"sv, "length"sv,
             "length-percentage"sv, "number"sv, "percentage"sv, "resolution"sv, "string"sv, "time"sv,
             "url"sv, "transform-function"sv }) {
        auto source = Utf16String::formatted("<{}>", type);
        compare_parsed_syntax_serialization(source, source);
    }
}

TEST_CASE(multiple_keywords)
{
    compare_parsed_syntax_serialization("well|hello|friends"_utf16, "well | hello | friends"_utf16);
}

TEST_CASE(repeated_type)
{
    compare_parsed_syntax_serialization("<number>+"_utf16, "<number>+"_utf16);
}

TEST_CASE(repeated_with_commas)
{
    compare_parsed_syntax_serialization("<number>#"_utf16, "<number>#"_utf16);
}

TEST_CASE(complex)
{
    compare_parsed_syntax_serialization("well|<number>+|<string>#"_utf16, "well | <number>+ | <string>#"_utf16);
}

TEST_CASE(syntax_string)
{
    expect_serializations_equal("<number>"_utf16, "\"<number>\""_utf16);
    expect_serializations_equal("well | <number>+ | <string>#"_utf16, "\"well | <number>+ | <string>#\""_utf16);
}

TEST_CASE(invalid)
{
    for (auto source : { ""sv, " "sv, "<number"sv, "thing|"sv, "*|*"sv, "<transform-list>+"sv,
             "<transform-list>#"sv, "<woozle>"sv, "<number> <integer>"sv, "thingy whatsit"sv,
             "<number> +"sv, "<number> #"sv }) {
        EXPECT(!parse_as_syntax(Utf16String::from_utf8_without_validation(source)).has_value());
    }
}

TEST_CASE(devtools_declaration_metadata)
{
    auto source = u"COLOR: red !important; --custom: token stream; unknown-property: 1px; -webkit-unknown: 2px; -webkit-box-orient: horizontal; -webkit-box-orient: vertical; -webkit-box-orient: invalid; color: nonsense;"sv;

    auto declarations = parse_css_declaration_block_for_devtools(ParsingParams {}, source);
    EXPECT_EQ(declarations.size(), 8u);

    auto expect_declaration = [&](size_t index, Utf16View name, Utf16View value, Important important,
                                  bool is_custom_property, bool is_name_valid, bool is_valid) {
        EXPECT_EQ(declarations[index].name, name);
        EXPECT_EQ(declarations[index].value, value);
        EXPECT_EQ(declarations[index].important, important);
        EXPECT_EQ(declarations[index].is_custom_property, is_custom_property);
        EXPECT_EQ(declarations[index].is_name_valid, is_name_valid);
        EXPECT_EQ(declarations[index].is_valid, is_valid);
    };
    expect_declaration(0, u"COLOR"sv, u"red"sv, Important::Yes, false, true, true);
    expect_declaration(1, u"--custom"sv, u"token stream"sv, Important::No, true, true, true);
    expect_declaration(2, u"unknown-property"sv, u"1px"sv, Important::No, false, false, false);
    expect_declaration(3, u"-webkit-unknown"sv, u"2px"sv, Important::No, false, false, false);
    expect_declaration(4, u"-webkit-box-orient"sv, u"horizontal"sv, Important::No, false, true, true);
    expect_declaration(5, u"-webkit-box-orient"sv, u"vertical"sv, Important::No, false, true, true);
    expect_declaration(6, u"-webkit-box-orient"sv, u"invalid"sv, Important::No, false, true, false);
    expect_declaration(7, u"color"sv, u"nonsense"sv, Important::No, false, true, false);
}

}
