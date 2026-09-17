/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibXML/Parser/Parser.h>

TEST_CASE(char_data_ending)
{
    EXPECT_NO_DEATH("parsing character data ending by itself should not crash", [] {
        // After seeing `<C>`, the parser will start parsing the content of the element. The content parser will then parse any character data it sees.
        // The character parser would see the first two `]]` and consume them. Then, it would see the `>` and set the state machine to say we have seen this,
        // but it did _not_ consume it and would instead tell GenericLexer that it should stop consuming characters. Therefore, we only consumed 2 characters.
        // Then, it would see that we are in the state where we've seen the full `]]>` and try to take off three characters from the end of the consumed
        // input when we only have 2 characters, causing an assertion failure as we are asking to take off more characters than there really is.
        XML::Parser parser("<C>]]>"sv);
        (void)parser.parse();
    }());
}

TEST_CASE(character_reference_integer_overflow)
{
    EXPECT_NO_DEATH("parsing character references that do not fit in 32 bits should not crash", [] {
        XML::Parser parser("<G>&#6666666666"sv);
        (void)parser.parse();
    }());
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

static bool parses(StringView source)
{
    XML::Parser parser(source);
    return !parser.parse().is_error();
}

static Optional<ByteString> first_error(StringView source)
{
    XML::Parser parser(source);
    auto result = parser.parse();
    if (!result.is_error())
        return {};
    return result.error().error.get<ByteString>();
}

TEST_CASE(dtd_defaulted_attribute_name_must_be_a_qualified_name)
{
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg a::b CDATA \"\">]><svg xmlns:a=\"urn:a\"/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg a: CDATA \"\">]><svg xmlns:a=\"urn:a\"/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg :b CDATA \"\">]><svg/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg a:b:c CDATA \"\">]><svg xmlns:a=\"urn:a\"/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg a:1b CDATA \"\">]><svg xmlns:a=\"urn:a\"/>"sv));
    EXPECT_EQ(first_error("<!DOCTYPE svg [<!ATTLIST svg a::b CDATA \"\">]><svg xmlns:a=\"urn:a\"/>"sv), "Attribute name 'a::b' is not a qualified name"sv);

    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg a:b CDATA \"\">]><svg xmlns:a=\"urn:a\"/>"sv));
    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg b CDATA \"\">]><svg/>"sv));
    // U+1200 became a NameStartChar in XML 1.0 fifth edition: the parser and the NCName check have to agree on it.
    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg \xE1\x88\x80 CDATA \"\">]><svg/>"sv));
}

TEST_CASE(dtd_defaulted_namespace_prefix_must_be_an_ncname)
{
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:a::b CDATA \"urn:a\">]><svg/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:a:b CDATA \"urn:a\">]><svg/>"sv));
    EXPECT_EQ(first_error("<!DOCTYPE svg [<!ATTLIST svg xmlns:a::b CDATA \"urn:a\">]><svg/>"sv), "Namespace prefix 'a::b' is not an NCName"sv);

    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:a CDATA \"urn:a\">]><svg a:b=\"\"/>"sv));
}

TEST_CASE(dtd_defaulted_name_errors_reach_the_listener_before_the_element)
{
    struct Listener final : XML::Listener {
        size_t elements_started { 0 };
        size_t errors { 0 };
        virtual void element_start(Utf16FlyString const&, Vector<XML::ListenerAttribute> const&) override { elements_started++; }
        virtual void error(XML::ParseError const&) override { errors++; }
    } listener;

    XML::Parser parser("<!DOCTYPE svg [<!ATTLIST svg a::b CDATA \"\">]><svg xmlns:a=\"urn:a\"><g/></svg>"sv);
    EXPECT(parser.parse_with_listener(listener).is_error());
    EXPECT_EQ(listener.errors, 1u);
    EXPECT_EQ(listener.elements_started, 0u);
}
