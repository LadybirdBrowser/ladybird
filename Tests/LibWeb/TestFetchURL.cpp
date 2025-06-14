/*
 * Copyright (c) 2023, Karol Kosek <krkk@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>

TEST_CASE(data_url)
{
    auto url = URL::Parser::basic_parse("data:text/html,test"_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize(), "data:text/html,test");

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/html");
    EXPECT_EQ(StringView(data_url.body.bytes()), "test"_sv);
}

TEST_CASE(data_url_default_mime_type)
{
    auto url = URL::Parser::basic_parse("data:,test"_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize(), "data:,test");

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/plain;charset=US-ASCII");
    EXPECT_EQ(StringView(data_url.body.bytes()), "test"_sv);
}

TEST_CASE(data_url_encoded)
{
    auto url = URL::Parser::basic_parse("data:text/html,Hello%20friends%2C%0X%X0"_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize(), "data:text/html,Hello%20friends%2C%0X%X0");

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/html");
    EXPECT_EQ(StringView(data_url.body.bytes()), "Hello friends,%0X%X0"_sv);
}

TEST_CASE(data_url_base64_encoded)
{
    auto url = URL::Parser::basic_parse("data:text/html;base64,dGVzdA=="_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize(), "data:text/html;base64,dGVzdA==");

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/html");
    EXPECT_EQ(StringView(data_url.body.bytes()), "test"_sv);
}

TEST_CASE(data_url_base64_encoded_default_mime_type)
{
    auto url = URL::Parser::basic_parse("data:;base64,dGVzdA=="_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize(), "data:;base64,dGVzdA==");

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/plain;charset=US-ASCII");
    EXPECT_EQ(StringView(data_url.body.bytes()), "test"_sv);
}

TEST_CASE(data_url_base64_encoded_with_whitespace)
{
    auto url = URL::Parser::basic_parse("data: text/html ;     bAsE64 , dGVz dA== "_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());
    EXPECT_EQ(url->serialize(), "data: text/html ;     bAsE64 , dGVz dA==");

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/html");
    EXPECT_EQ(StringView(data_url.body.bytes()), "test");
}

TEST_CASE(data_url_base64_encoded_with_inline_whitespace)
{
    auto url = URL::Parser::basic_parse("data:text/javascript;base64,%20ZD%20Qg%0D%0APS%20An%20Zm91cic%0D%0A%207%20"_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT(!url->host().has_value());

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/javascript");
    EXPECT_EQ(StringView(data_url.body.bytes()), "d4 = 'four';"_sv);
}

TEST_CASE(data_url_completed_with_fragment)
{
    auto url = URL::Parser::basic_parse("data:text/plain,test"_sv)->complete_url("#a"_sv);
    EXPECT(url.has_value());
    EXPECT_EQ(url->scheme(), "data");
    EXPECT_EQ(url->fragment(), "a");
    EXPECT(!url->host().has_value());

    auto data_url = TRY_OR_FAIL(Web::Fetch::Infrastructure::process_data_url(*url));
    EXPECT_EQ(data_url.mime_type.serialized(), "text/plain");
    EXPECT_EQ(StringView(data_url.body.bytes()), "test"_sv);
}
