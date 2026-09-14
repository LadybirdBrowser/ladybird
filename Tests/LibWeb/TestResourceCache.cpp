/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Page/ResourceCache.h>

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

TEST_CASE(entry_that_grows_past_the_memory_limit_is_evicted)
{
    HashMap<int, size_t> value_sizes;
    Web::ResourceCache<int> cache { 32, 1024, [&](int const& value) { return value_sizes.get(value).value(); } };
    auto growing_url = parse_url("data:,growing"sv);
    auto other_url = parse_url("data:,other"sv);

    value_sizes.set(1, 100);
    value_sizes.set(2, 100);
    cache.set(growing_url, 1);
    cache.set(other_url, 2);

    value_sizes.set(1, 2048);
    EXPECT(cache.get(other_url).has_value());
    EXPECT(!cache.get(growing_url).has_value());
}

TEST_CASE(entries_that_grow_past_the_memory_limit_evict_the_least_recently_used)
{
    HashMap<int, size_t> value_sizes;
    Web::ResourceCache<int> cache { 32, 1024, [&](int const& value) { return value_sizes.get(value).value(); } };
    auto older_url = parse_url("data:,older"sv);
    auto newer_url = parse_url("data:,newer"sv);

    value_sizes.set(1, 100);
    value_sizes.set(2, 100);
    cache.set(older_url, 1);
    cache.set(newer_url, 2);

    value_sizes.set(1, 600);
    value_sizes.set(2, 600);
    EXPECT(cache.get(newer_url).has_value());
    EXPECT(!cache.get(older_url).has_value());
}
