/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibHTTP/StructuredFields.h>

using namespace HTTP::StructuredFields;

static BareItem parse_bare_item(StringView input)
{
    auto item = MUST(parse<Item>(input));
    EXPECT(item.parameters.is_empty());
    return move(item.bare_item);
}

static BareItem string(StringView value)
{
    return ByteString { value };
}

static BareItem token(StringView value)
{
    return Token { value };
}

static ByteBuffer bytes(StringView value)
{
    return MUST(ByteBuffer::copy(value.bytes()));
}

TEST_CASE(integers)
{
    EXPECT(parse_bare_item("42"sv) == BareItem { i64 { 42 } });
    EXPECT(parse_bare_item("-42"sv) == BareItem { i64 { -42 } });
    EXPECT(parse_bare_item("0042"sv) == BareItem { i64 { 42 } });
    EXPECT(parse_bare_item("999999999999999"sv) == BareItem { i64 { 999'999'999'999'999 } });
    EXPECT(parse_bare_item("-999999999999999"sv) == BareItem { i64 { -999'999'999'999'999 } });

    EXPECT(parse<Item>("1000000000000000"sv).is_error());
    EXPECT(parse<Item>("-"sv).is_error());
    EXPECT(parse<Item>("-a"sv).is_error());
    EXPECT(parse<Item>("- 1"sv).is_error());
}

TEST_CASE(decimals)
{
    EXPECT(parse_bare_item("1.5"sv) == BareItem { 1.5 });
    EXPECT(parse_bare_item("-0.25"sv) == BareItem { -0.25 });
    EXPECT(parse_bare_item("123456789012.125"sv) == BareItem { 123456789012.125 });

    EXPECT(parse<Item>("1234567890123.0"sv).is_error());
    EXPECT(parse<Item>("1.2345"sv).is_error());
    EXPECT(parse<Item>("1."sv).is_error());
    EXPECT(parse<Item>("1.2.3"sv).is_error());
}

TEST_CASE(strings)
{
    EXPECT(parse_bare_item("\"foo bar\""sv) == string("foo bar"sv));
    EXPECT(parse_bare_item("\"\""sv) == string(""sv));
    EXPECT(parse_bare_item(R"("a \"quoted\" \\ string")"sv) == string(R"(a "quoted" \ string)"sv));

    EXPECT(parse<Item>("\"foo"sv).is_error());
    EXPECT(parse<Item>(R"("\a")"sv).is_error());
    EXPECT(parse<Item>("\"\\"sv).is_error());
    EXPECT(parse<Item>("\"foo\tbar\""sv).is_error());
    EXPECT(parse<Item>("\"foo\x7f\""sv).is_error());
}

TEST_CASE(tokens)
{
    EXPECT(parse_bare_item("same-origin"sv) == token("same-origin"sv));
    EXPECT(parse_bare_item("*foo"sv) == token("*foo"sv));
    EXPECT(parse_bare_item("text/html:x!#$%&'*+-.^_`|~"sv) == token("text/html:x!#$%&'*+-.^_`|~"sv));
    EXPECT(parse_bare_item("SAME-ORIGIN"sv) == token("SAME-ORIGIN"sv));

    EXPECT(parse<Item>("$same-origin"sv).is_error());
    EXPECT(parse<Item>("same-origin same-origin"sv).is_error());
    EXPECT(parse<Item>("same(origin"sv).is_error());
}

TEST_CASE(byte_sequences)
{
    EXPECT(parse_bare_item(":aGVsbG8=:"sv) == BareItem { bytes("hello"sv) });
    EXPECT(parse_bare_item("::"sv) == BareItem { ByteBuffer {} });

    // Recipients should neither require "=" padding nor reject non-zero pad bits.
    EXPECT(parse_bare_item(":aGVsbG8:"sv) == BareItem { bytes("hello"sv) });
    EXPECT(!parse<Item>(":iZ==:"sv).is_error());

    EXPECT(parse<Item>(":aGVsbG8="sv).is_error());
    EXPECT(parse<Item>(":aGVs bG8=:"sv).is_error());
    EXPECT(parse<Item>(":aGVsbG8_:"sv).is_error());
}

TEST_CASE(booleans)
{
    EXPECT(parse_bare_item("?1"sv) == BareItem { true });
    EXPECT(parse_bare_item("?0"sv) == BareItem { false });

    EXPECT(parse<Item>("?"sv).is_error());
    EXPECT(parse<Item>("?2"sv).is_error());
    EXPECT(parse<Item>("?true"sv).is_error());
}

TEST_CASE(dates)
{
    EXPECT(parse_bare_item("@1659578233"sv) == BareItem { Date { 1659578233 } });
    EXPECT(parse_bare_item("@-1659578233"sv) == BareItem { Date { -1659578233 } });

    EXPECT(parse<Item>("@1659578233.12"sv).is_error());
    EXPECT(parse<Item>("@"sv).is_error());
    EXPECT(parse<Item>("@foo"sv).is_error());
}

TEST_CASE(display_strings)
{
    EXPECT(parse_bare_item(R"(%"f%c3%bc%c3%bc")"sv) == BareItem { DisplayString { "füü"_string } });
    EXPECT(parse_bare_item(R"(%"")"sv) == BareItem { DisplayString { String {} } });

    EXPECT(parse<Item>(R"(%"f%C3%BC")"sv).is_error());
    EXPECT(parse<Item>(R"(%"%c3%28")"sv).is_error());
    EXPECT(parse<Item>(R"(%"%ed%a0%80")"sv).is_error());
    EXPECT(parse<Item>(R"(%"%c3")"sv).is_error());
    EXPECT(parse<Item>(R"(%"%c")"sv).is_error());
    EXPECT(parse<Item>(R"(%"foo)"sv).is_error());
    EXPECT(parse<Item>(R"(%foo)"sv).is_error());
}

TEST_CASE(parameters)
{
    auto item = MUST(parse<Item>("same-origin; report-to=\"endpoint\";a;b=?0;c=1"sv));
    EXPECT(item.bare_item == token("same-origin"sv));
    EXPECT_EQ(item.parameters.keys(), (Vector<ByteString> { "report-to"sv, "a"sv, "b"sv, "c"sv }));
    EXPECT(item.parameters.get("report-to"sv) == string("endpoint"sv));
    EXPECT(item.parameters.get("a"sv) == BareItem { true });
    EXPECT(item.parameters.get("b"sv) == BareItem { false });
    EXPECT(item.parameters.get("c"sv) == BareItem { i64 { 1 } });

    // A duplicate key overwrites the earlier value, but keeps its position.
    auto duplicate = MUST(parse<Item>("foo;a=1;b=2;a=3"sv));
    EXPECT_EQ(duplicate.parameters.keys(), (Vector<ByteString> { "a"sv, "b"sv }));
    EXPECT(duplicate.parameters.get("a"sv) == BareItem { i64 { 3 } });

    EXPECT(parse<Item>("same-origin;"sv).is_error());
    EXPECT(parse<Item>("same-origin;\tfoo=bar"sv).is_error());
    EXPECT(parse<Item>("same-origin ;foo=bar"sv).is_error());
    EXPECT(parse<Item>("same-origin;Foo=bar"sv).is_error());
    EXPECT(parse<Item>("same-origin;foo=(bar)"sv).is_error());
}

TEST_CASE(lists)
{
    EXPECT(MUST(parse<List>(""sv)).is_empty());
    EXPECT(MUST(parse<List>("   "sv)).is_empty());

    auto list = MUST(parse<List>("1, \"two\",\t three;x, (a b);y=?1, ()"sv));
    EXPECT_EQ(list.size(), 5u);
    EXPECT(list[0].get<Item>().bare_item == BareItem { i64 { 1 } });
    EXPECT(list[1].get<Item>().bare_item == string("two"sv));
    EXPECT(list[2].get<Item>().bare_item == token("three"sv));
    EXPECT(list[2].get<Item>().parameters.get("x"sv) == BareItem { true });

    auto const& inner_list = list[3].get<InnerList>();
    EXPECT_EQ(inner_list.items.size(), 2u);
    EXPECT(inner_list.items[0].bare_item == token("a"sv));
    EXPECT(inner_list.items[1].bare_item == token("b"sv));
    EXPECT(inner_list.parameters.get("y"sv) == BareItem { true });

    EXPECT(list[4].get<InnerList>().items.is_empty());

    EXPECT(parse<List>("1,"sv).is_error());
    EXPECT(parse<List>(",1"sv).is_error());
    EXPECT(parse<List>("1,,2"sv).is_error());
    EXPECT(parse<List>("1 2"sv).is_error());
    EXPECT(parse<List>("(a b"sv).is_error());
    EXPECT(parse<List>("(a,b)"sv).is_error());
}

TEST_CASE(dictionaries)
{
    EXPECT(MUST(parse<Dictionary>(""sv)).is_empty());

    auto dictionary = MUST(parse<Dictionary>("a=1, b, c;x=?0, d=(1 2), *e=\"f\""sv));
    EXPECT_EQ(dictionary.keys(), (Vector<ByteString> { "a"sv, "b"sv, "c"sv, "d"sv, "*e"sv }));
    EXPECT(dictionary.get("a"sv)->get<Item>().bare_item == BareItem { i64 { 1 } });
    EXPECT(dictionary.get("b"sv)->get<Item>().bare_item == BareItem { true });
    EXPECT(dictionary.get("c"sv)->get<Item>().bare_item == BareItem { true });
    EXPECT(dictionary.get("c"sv)->get<Item>().parameters.get("x"sv) == BareItem { false });
    EXPECT_EQ(dictionary.get("d"sv)->get<InnerList>().items.size(), 2u);
    EXPECT(dictionary.get("*e"sv)->get<Item>().bare_item == string("f"sv));

    // A duplicate key overwrites the earlier member, but keeps its position.
    auto duplicate = MUST(parse<Dictionary>("a=1, b=2, a=3"sv));
    EXPECT_EQ(duplicate.keys(), (Vector<ByteString> { "a"sv, "b"sv }));
    EXPECT(duplicate.get("a"sv)->get<Item>().bare_item == BareItem { i64 { 3 } });

    EXPECT(parse<Dictionary>("a=1,"sv).is_error());
    EXPECT(parse<Dictionary>("A=1"sv).is_error());
    EXPECT(parse<Dictionary>("a = 1"sv).is_error());
    EXPECT(parse<Dictionary>("a=1 b=2"sv).is_error());
}

TEST_CASE(field_values)
{
    EXPECT(parse_bare_item("  same-origin  "sv) == token("same-origin"sv));

    EXPECT(parse<Item>(""sv).is_error());
    EXPECT(parse<Item>("\tsame-origin"sv).is_error());
    EXPECT(parse<Item>("same-origin\t"sv).is_error());
    EXPECT(parse<Item>("same-origin, same-origin"sv).is_error());
    EXPECT(parse<Item>("\"f\xc3\xbc\""sv).is_error());
    EXPECT(parse<List>("a, b\xff"sv).is_error());
}
