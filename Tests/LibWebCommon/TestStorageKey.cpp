/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

static URL::Origin origin_of(StringView url)
{
    return URL::Parser::basic_parse(url)->origin();
}

TEST_CASE(a_tuple_origin_is_its_own_storage_key)
{
    auto key = Web::StorageAPI::obtain_a_storage_key(origin_of("http://example.com:8080/path"sv));
    EXPECT(key.has_value());
    EXPECT_EQ(key->to_string(), "http://example.com:8080"_string);

    key = Web::StorageAPI::obtain_a_storage_key(origin_of("https://example.com:443/"sv));
    EXPECT(key.has_value());
    EXPECT_EQ(key->to_string(), "https://example.com"_string);
}

// Every "file:" document shares one bottle, whatever its path: the opaque file origin maps to the "file:" origin.
TEST_CASE(a_file_origin_gets_the_shared_file_storage_key)
{
    auto key = Web::StorageAPI::obtain_a_storage_key(origin_of("file:///Users/someone/page.html"sv));
    EXPECT(key.has_value());
    EXPECT_EQ(key->to_string(), "file://"_string);

    auto other = Web::StorageAPI::obtain_a_storage_key(origin_of("file:///tmp/other.html"sv));
    EXPECT(other.has_value());
    EXPECT_EQ(*key, *other);
}

TEST_CASE(an_opaque_origin_has_no_storage_key)
{
    EXPECT(!Web::StorageAPI::obtain_a_storage_key(origin_of("data:text/html,hi"sv)).has_value());
    EXPECT(!Web::StorageAPI::obtain_a_storage_key(URL::Origin::create_opaque()).has_value());
}
