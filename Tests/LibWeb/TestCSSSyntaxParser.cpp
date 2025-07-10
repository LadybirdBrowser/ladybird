/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>

namespace Web::CSS::Parser {

static void compare_parsed_syntax_dump_to_string(Vector<ComponentValue> const& syntax_values, StringView expected)
{
    auto syntax = parse_as_syntax(syntax_values);
    EXPECT(syntax != nullptr);
    if (syntax)
        EXPECT_EQ(syntax->dump(), expected);
}

static void expect_dumps_equal(Vector<ComponentValue> const& lhs_values, Vector<ComponentValue> const& rhs_values)
{
    auto lhs = parse_as_syntax(lhs_values);
    auto rhs = parse_as_syntax(rhs_values);
    EXPECT(lhs != nullptr);
    EXPECT(rhs != nullptr);
    if (lhs && rhs)
        EXPECT_EQ(lhs->dump(), rhs->dump());
}

#define TYPE_TOKENS(name) Token::create_delim('<'), Token::create_ident(name ""_fly_string), Token::create_delim('>')

TEST_CASE(single_universal)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { Token::create_delim('*') }, "Universal\n"sv);
}

TEST_CASE(single_ident)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { Token::create_ident("thing"_fly_string) }, "Ident: thing\n"sv);
}

TEST_CASE(single_type)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("angle") }, "Type: angle\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("color") }, "Type: color\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("custom-ident") }, "Type: custom-ident\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("image") }, "Type: image\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("integer") }, "Type: integer\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("length") }, "Type: length\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("length-percentage") }, "Type: length-percentage\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("number") }, "Type: number\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("percentage") }, "Type: percentage\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("resolution") }, "Type: resolution\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("string") }, "Type: string\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("time") }, "Type: time\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("url") }, "Type: url\n"sv);
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("transform-function") }, "Type: transform-function\n"sv);
}

TEST_CASE(multiple_keywords)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> {
                                             Token::create_ident("well"_fly_string),
                                             Token::create_delim('|'),
                                             Token::create_ident("hello"_fly_string),
                                             Token::create_delim('|'),
                                             Token::create_ident("friends"_fly_string) },
        R"~~~(Alternatives:
  Ident: well
  Ident: hello
  Ident: friends
)~~~"sv);
}

TEST_CASE(repeated_type)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("number"), Token::create_delim('+') },
        R"~~~(Multiplier:
  Type: number
)~~~"sv);
}

TEST_CASE(repeated_with_commas)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> { TYPE_TOKENS("number"), Token::create_delim('#') },
        R"~~~(CommaSeparatedMultiplier:
  Type: number
)~~~"sv);
}

TEST_CASE(complex)
{
    compare_parsed_syntax_dump_to_string(Vector<ComponentValue> {
                                             Token::create_ident("well"_fly_string),
                                             Token::create_delim('|'),
                                             TYPE_TOKENS("number"), Token::create_delim('+'),
                                             Token::create_delim('|'),
                                             TYPE_TOKENS("string"), Token::create_delim('#') },
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
    // A single string token's contents are parsed as if it was unquoted

    expect_dumps_equal(Vector<ComponentValue> { TYPE_TOKENS("number") }, Vector<ComponentValue> { Token::create_string("<number>"_fly_string) });

    expect_dumps_equal(Vector<ComponentValue> {
                           Token::create_ident("well"_fly_string),
                           Token::create_delim('|'),
                           TYPE_TOKENS("number"), Token::create_delim('+'),
                           Token::create_delim('|'),
                           TYPE_TOKENS("string"), Token::create_delim('#') },
        Vector<ComponentValue> { Token::create_string("well | <number>+ | <string>#"_fly_string) });
}

TEST_CASE(invalid)
{
    // Empty
    EXPECT(!parse_as_syntax(Vector<ComponentValue> {}));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create_whitespace() }));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create(Token::Type::EndOfFile) }));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create(Token::Type::Invalid) }));

    // Incomplete
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create_delim('<'), Token::create_ident("number"_fly_string) }));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create_ident("thing"_fly_string), Token::create_delim('|') }));

    // '*' is only allowed on its own
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create_delim('*'), Token::create_delim('|'), Token::create_delim('*') }));

    // <transform-list> cannot have multipliers
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { TYPE_TOKENS("transform-list"), Token::create_delim('+') }));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { TYPE_TOKENS("transform-list"), Token::create_delim('#') }));

    // For <syntax>, only predefined types are allowed
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { TYPE_TOKENS("woozle") }));

    // <syntax> doesn't allow multiple types/keywords without a combinator
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { TYPE_TOKENS("number"), Token::create_whitespace(), TYPE_TOKENS("integer") }));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { Token::create_ident("thingy"_fly_string), Token::create_whitespace(), Token::create_ident("whatsit"_fly_string) }));

    // Whitespace isn't allowed between a type and its multiplier
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { TYPE_TOKENS("number"), Token::create_whitespace(), Token::create_delim('+') }));
    EXPECT(!parse_as_syntax(Vector<ComponentValue> { TYPE_TOKENS("number"), Token::create_whitespace(), Token::create_delim('#') }));
}

}
