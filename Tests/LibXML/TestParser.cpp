/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibXML/Parser/Parser.h>

TEST_CASE(char_data_ending)
{
    EXPECT_NO_CRASH("parsing character data ending by itself should not crash", [] {
        // After seeing `<C>`, the parser will start parsing the content of the element. The content parser will then parse any character data it sees.
        // The character parser would see the first two `]]` and consume them. Then, it would see the `>` and set the state machine to say we have seen this,
        // but it did _not_ consume it and would instead tell GenericLexer that it should stop consuming characters. Therefore, we only consumed 2 characters.
        // Then, it would see that we are in the state where we've seen the full `]]>` and try to take off three characters from the end of the consumed
        // input when we only have 2 characters, causing an assertion failure as we are asking to take off more characters than there really is.
        XML::Parser parser("<C>]]>"sv);
        (void)parser.parse();
        return Test::Crash::Failure::DidNotCrash;
    });
}

TEST_CASE(character_reference_integer_overflow)
{
    EXPECT_NO_CRASH("parsing character references that do not fit in 32 bits should not crash", [] {
        XML::Parser parser("<G>&#6666666666"sv);
        (void)parser.parse();
        return Test::Crash::Failure::DidNotCrash;
    });
}

TEST_CASE(predefined_character_reference)
{
    XML::Parser parser("<a>Well hello &amp;, &lt;, &gt;, &apos;, and &quot;!</a>"sv);
    auto document = MUST(parser.parse());

    auto const& node = document.root().content.get<XML::Node::Element>();
    EXPECT_EQ(node.name, "a");

    auto const& content = node.children[0]->content.get<XML::Node::Text>();
    EXPECT_EQ(content.builder.string_view(), "Well hello &, <, >, ', and \"!");
}

TEST_CASE(unicode_name)
{
    XML::Parser parser("<div 中文=\"\"></div>"sv);
    TRY_OR_FAIL(parser.parse());
}
