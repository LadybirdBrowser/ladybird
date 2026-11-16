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

TEST_CASE(declaring_the_xml_prefix_as_itself_is_an_attribute)
{
    struct Listener final : XML::Listener {
        Vector<ByteString> attributes;
        virtual void element_start(Utf16FlyString const&, Vector<XML::ListenerAttribute> const& list) override
        {
            for (auto const& attribute : list)
                attributes.append(ByteString::formatted("{}={}", attribute.name, attribute.value));
        }
    } listener;

    XML::Parser parser("<a b=\"c\" xmlns:xml=\"http://www.w3.org/XML/1998/namespace\" xml:lang=\"en\"/>"sv);
    EXPECT(!parser.parse_with_listener(listener).is_error());
    EXPECT_EQ(listener.attributes.size(), 3u);
    EXPECT(listener.attributes.contains_slow("xmlns:xml=http://www.w3.org/XML/1998/namespace"sv));
    EXPECT(listener.attributes.contains_slow("xml:lang=en"sv));

    EXPECT(!parses("<a xmlns:xml=\"urn:other\"/>"sv));

    // The same goes for a declaration the DTD supplies.
    listener.attributes.clear();
    XML::Parser defaulted("<!DOCTYPE a [<!ATTLIST a xmlns:xml CDATA \"http://www.w3.org/XML/1998/namespace\" b CDATA \"c\">]><a/>"sv);
    EXPECT(!defaulted.parse_with_listener(listener).is_error());
    EXPECT_EQ(listener.attributes.size(), 2u);
    EXPECT(listener.attributes.contains_slow("xmlns:xml=http://www.w3.org/XML/1998/namespace"sv));
}

TEST_CASE(comments_in_the_internal_subset_are_not_content)
{
    struct Listener final : XML::Listener {
        Vector<ByteString> comments;
        virtual void comment(StringView data) override { comments.append(data); }
    } listener;

    XML::Parser parser("<!DOCTYPE a [<!-- in the subset -->]><!-- before --><a><!-- inside --></a>"sv, { .preserve_comments = true });
    EXPECT(!parser.parse_with_listener(listener).is_error());
    EXPECT_EQ(listener.comments.size(), 2u);
    EXPECT_EQ(listener.comments[0], " before "sv);
    EXPECT_EQ(listener.comments[1], " inside "sv);
}

TEST_CASE(an_entity_declared_nowhere_we_read_is_an_error)
{
    struct Listener final : XML::Listener {
    } listener;

    // The external subset isn't read, so it may declare the entity, but what it stands for is unknown either way.
    for (auto source : { "<a>&b;</a>"sv, "<!DOCTYPE a SYSTEM 'a.dtd'><a>&b;</a>"sv, "<!DOCTYPE a SYSTEM 'a.dtd'><a c='&b;'/>"sv }) {
        XML::Parser parser(source);
        EXPECT(parser.parse_with_listener(listener).is_error());
    }
}

TEST_CASE(a_namespace_name_need_not_be_a_uri)
{
    struct Listener final : XML::Listener {
    } listener;

    for (auto source : { "<a xmlns='not a uri'/>"sv, "<p:a xmlns:p='http://a/%zz'/>"sv, "<!DOCTYPE a [<!ATTLIST a xmlns CDATA 'not a uri'>]><a/>"sv }) {
        XML::Parser parser(source);
        EXPECT(!parser.parse_with_listener(listener).is_error());
    }
}

TEST_CASE(an_error_in_a_run_of_text_comes_between_its_pieces)
{
    struct Listener final : XML::Listener {
        Vector<ByteString> events;
        virtual void text(StringView data) override { events.append(ByteString::formatted("text {}", data)); }
        virtual void error(XML::ParseError const&) override { events.append("error"sv); }
    } listener;

    XML::Parser parser("<!DOCTYPE a SYSTEM 'a.dtd'><a>A&amp;B&missing;C&amp;D</a>"sv);
    EXPECT(parser.parse_with_listener(listener).is_error());
    EXPECT_EQ(listener.events, (Vector<ByteString> { "text A&B"sv, "error"sv, "text C&D"sv }));
}

TEST_CASE(line_ends_in_the_identifiers_of_a_doctype_are_line_feeds)
{
    struct Listener final : XML::Listener {
        Optional<XML::Doctype> doctype;
        virtual void set_doctype(XML::Doctype declaration) override { doctype = move(declaration); }
    } listener;

    XML::Parser parser("<!DOCTYPE a PUBLIC \"p\r\nq\" \"s\rt\"><a/>"sv);
    EXPECT(!parser.parse_with_listener(listener).is_error());
    auto const& id = listener.doctype->external_id.value();
    EXPECT_EQ(id.public_id->public_literal, "p\nq"sv);
    EXPECT_EQ(id.system_id.system_literal, "s\nt"sv);
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

TEST_CASE(dtd_defaulted_namespace_declaration_must_respect_reserved_bindings)
{
    // The XML namespace name belongs to the xml prefix alone.
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:p CDATA \"http://www.w3.org/XML/1998/namespace\">]><svg p:lang=\"e\"/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:p CDATA #FIXED \"http://www.w3.org/XML/1998/namespace\">]><svg p:lang=\"e\"/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns CDATA \"http://www.w3.org/XML/1998/namespace\">]><svg/>"sv));
    EXPECT_EQ(first_error("<!DOCTYPE svg [<!ATTLIST svg xmlns:p CDATA \"http://www.w3.org/XML/1998/namespace\">]><svg p:lang=\"e\"/>"sv), "Namespace prefix 'p' must not be bound to the XML namespace"sv);
    // The xmlns prefix is never declared, and its namespace name is never bound.
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:xmlns CDATA \"urn:x\">]><svg/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:p CDATA \"http://www.w3.org/2000/xmlns/\">]><svg/>"sv));
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns CDATA \"http://www.w3.org/2000/xmlns/\">]><svg/>"sv));
    // A prefix can't be bound to an empty namespace name; the default namespace can be.
    EXPECT(!parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:p CDATA \"\">]><svg/>"sv));
    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg xmlns CDATA \"\">]><svg/>"sv));

    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg xmlns:p CDATA \"http://www.w3.org/XML/1998/namespacX\">]><svg p:lang=\"e\"/>"sv));
    EXPECT(parses("<!DOCTYPE svg [<!ATTLIST svg xmlns CDATA \"urn:x\">]><svg/>"sv));
    // Declaring the xml prefix with its own namespace name is allowed.
    EXPECT(parses("<svg xmlns:xml=\"http://www.w3.org/XML/1998/namespace\" xml:lang=\"e\"/>"sv));
}

TEST_CASE(default_namespace_is_no_binding_of_the_xmlns_prefix)
{
    EXPECT(!parses("<xmlns:a xmlns=\"urn:x\"/>"sv));
    EXPECT(!parses("<b xmlns=\"urn:x\"><xmlns:a/></b>"sv));
    EXPECT_EQ(first_error("<xmlns:a xmlns=\"urn:x\"/>"sv), "Namespace prefix xmlns on a is not defined"sv);
}
