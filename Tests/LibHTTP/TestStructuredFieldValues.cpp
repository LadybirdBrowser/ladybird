/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/HeaderList.h>
#include <LibHTTP/StructuredFieldValues.h>
#include <LibTest/TestCase.h>

using namespace HTTP::StructuredFieldValues;

TEST_CASE(tokens)
{
    auto item = parse_item("same-origin"sv);
    EXPECT(item.has_value());
    EXPECT_EQ(item->token(), "same-origin"sv);
    EXPECT(item->parameters.entries.is_empty());

    item = parse_item("  require-corp  "sv);
    EXPECT(item.has_value());
    EXPECT_EQ(item->token(), "require-corp"sv);

    item = parse_item("*token:/with/extras"sv);
    EXPECT(item.has_value());
    EXPECT_EQ(item->token(), "*token:/with/extras"sv);

    EXPECT(!parse_item(""sv).has_value());
    EXPECT(!parse_item("same-origin, unsafe-none"sv).has_value());
    EXPECT(!parse_item("same-origin unsafe-none"sv).has_value());
    EXPECT(!parse_item("(list)"sv).has_value());
}

TEST_CASE(parameters)
{
    auto item = parse_item("same-origin; report-to=\"coop-endpoint\"; flag; count=3"sv);
    EXPECT(item.has_value());
    EXPECT_EQ(item->token(), "same-origin"sv);
    EXPECT_EQ(item->parameters.entries.size(), 3u);

    auto report_to = item->parameters.get("report-to"sv);
    EXPECT(report_to.has_value());
    EXPECT_EQ(report_to->get<String>(), "coop-endpoint"_string);

    auto flag = item->parameters.get("flag"sv);
    EXPECT(flag.has_value());
    EXPECT_EQ(flag->get<bool>(), true);

    auto count = item->parameters.get("count"sv);
    EXPECT(count.has_value());
    EXPECT_EQ(count->get<i64>(), 3);

    EXPECT(!item->parameters.get("missing"sv).has_value());

    // Later parameters with the same key overwrite earlier ones.
    item = parse_item("a; k=1; k=2"sv);
    EXPECT(item.has_value());
    EXPECT_EQ(item->parameters.entries.size(), 1u);
    EXPECT_EQ(item->parameters.get("k"sv)->get<i64>(), 2);

    // Keys must start with a lowercase letter or "*", and a dangling ";" is an error.
    EXPECT(!parse_item("a; Key=1"sv).has_value());
    EXPECT(!parse_item("a;"sv).has_value());
    EXPECT(!parse_item("a; k="sv).has_value());
}

TEST_CASE(numbers)
{
    EXPECT_EQ(parse_item("42"sv)->value.get<i64>(), 42);
    EXPECT_EQ(parse_item("-17"sv)->value.get<i64>(), -17);
    EXPECT_EQ(parse_item("123456789012345"sv)->value.get<i64>(), 123456789012345);
    EXPECT(!parse_item("1234567890123456"sv).has_value());
    EXPECT(!parse_item("-"sv).has_value());

    EXPECT_EQ(parse_item("4.5"sv)->value.get<double>(), 4.5);
    EXPECT_EQ(parse_item("-0.125"sv)->value.get<double>(), -0.125);
    EXPECT(!parse_item("4."sv).has_value());
    EXPECT(!parse_item("4.1234"sv).has_value());
    EXPECT(!parse_item("1234567890123.0"sv).has_value());
}

TEST_CASE(strings_byte_sequences_and_booleans)
{
    EXPECT_EQ(parse_item("\"hello world\""sv)->value.get<String>(), "hello world"_string);
    EXPECT_EQ(parse_item("\"quote \\\" and backslash \\\\\""sv)->value.get<String>(), "quote \" and backslash \\"_string);
    EXPECT(!parse_item("\"unterminated"sv).has_value());
    EXPECT(!parse_item("\"bad \\n escape\""sv).has_value());
    EXPECT(!parse_item("\"control \x01 char\""sv).has_value());

    auto bytes = parse_item(":aGVsbG8=:"sv);
    EXPECT(bytes.has_value());
    EXPECT_EQ(StringView { bytes->value.get<ByteSequence>().value }, "hello"sv);
    EXPECT(!parse_item(":aGVsbG8="sv).has_value());
    EXPECT(!parse_item(":not base64!:"sv).has_value());

    EXPECT_EQ(parse_item("?1"sv)->value.get<bool>(), true);
    EXPECT_EQ(parse_item("?0"sv)->value.get<bool>(), false);
    EXPECT(!parse_item("?2"sv).has_value());
    EXPECT(!parse_item("?"sv).has_value());
}

TEST_CASE(header_list_lookup)
{
    auto headers = HTTP::HeaderList::create();
    EXPECT(!headers->get_structured_field_item("Cross-Origin-Opener-Policy"sv).has_value());

    headers->append(HTTP::Header::isomorphic_encode("Cross-Origin-Opener-Policy"sv, "same-origin; report-to=\"default\""sv));
    auto item = headers->get_structured_field_item("cross-origin-opener-policy"sv);
    EXPECT(item.has_value());
    EXPECT_EQ(item->token(), "same-origin"sv);
    EXPECT_EQ(item->parameters.get("report-to"sv)->get<String>(), "default"_string);

    // Multiple headers get combined into a list, which is not a valid item.
    headers->append(HTTP::Header::isomorphic_encode("Cross-Origin-Opener-Policy"sv, "unsafe-none"sv));
    EXPECT(!headers->get_structured_field_item("Cross-Origin-Opener-Policy"sv).has_value());
}
