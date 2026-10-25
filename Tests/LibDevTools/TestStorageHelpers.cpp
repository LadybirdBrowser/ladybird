/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/StorageHelpers.h>
#include <LibTest/TestCase.h>

// The host DevTools shows for a page and the storage key the browser keeps that page's storage under are two
// different namings of one origin; a storage edit made through DevTools has to land in the page's own bottle.
TEST_CASE(storage_key_for_host_names_the_bottle_of_an_http_host)
{
    EXPECT_EQ(DevTools::storage_key_for_host("http://example.com:8080"_string), Optional<String> { "http://example.com:8080"_string });
    EXPECT_EQ(DevTools::storage_key_for_host("https://example.com"_string), Optional<String> { "https://example.com"_string });

    // Round trip from a page URL: what the host becomes is what the key is made from.
    auto host = DevTools::storage_host_for_url("http://example.com:8080/some/page.html"_string);
    EXPECT(host.has_value());
    EXPECT_EQ(DevTools::storage_key_for_host(*host), Optional<String> { "http://example.com:8080"_string });
}

TEST_CASE(storage_key_for_host_maps_every_file_host_to_the_shared_file_bottle)
{
    auto host = DevTools::storage_host_for_url("file:///Users/someone/page.html"_string);
    EXPECT(host.has_value());
    EXPECT_EQ(DevTools::storage_key_for_host(*host), Optional<String> { "file://"_string });
    EXPECT_EQ(DevTools::storage_key_for_host("file:///tmp/other.html"_string), Optional<String> { "file://"_string });
}

TEST_CASE(storage_key_for_host_has_none_for_hosts_without_storage)
{
    EXPECT(!DevTools::storage_key_for_host("about:blank"_string).has_value());
    EXPECT(!DevTools::storage_key_for_host("javascript:void(0)"_string).has_value());
    EXPECT(!DevTools::storage_key_for_host("not a url"_string).has_value());
}
