/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <RequestServer/AlternativeServices.h>

using namespace RequestServer;

static UnixDateTime const s_now = UnixDateTime::from_seconds_since_epoch(1'000'000);

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

TEST_CASE(parse_alt_svc)
{
    auto alternatives = parse_alt_svc("h3=\":443\"; ma=86400, h3=\"alt.example:8443\"; persist=1"sv, s_now);
    EXPECT(alternatives.has_value());
    EXPECT_EQ(alternatives->size(), 2u);
    EXPECT_EQ(alternatives->at(0).host, ""sv);
    EXPECT_EQ(alternatives->at(0).port, 443);
    EXPECT_EQ(alternatives->at(0).expires_at, s_now + AK::Duration::from_seconds(86400));
    EXPECT_EQ(alternatives->at(1).host, "alt.example"sv);
    EXPECT_EQ(alternatives->at(1).port, 8443);

    alternatives = parse_alt_svc("h3=\"[2001:db8::1]:443\""sv, s_now, AK::Duration::from_seconds(60));
    EXPECT(alternatives.has_value());
    EXPECT_EQ(alternatives->at(0).host, "2001:db8::1"sv);
    EXPECT_EQ(alternatives->at(0).expires_at, s_now + AK::Duration::from_seconds(86400 - 60));

    alternatives = parse_alt_svc("%68%33=\":443\"; ma=\"60\""sv, s_now);
    EXPECT(alternatives.has_value());
    EXPECT_EQ(alternatives->size(), 1u);
    EXPECT_EQ(alternatives->at(0).expires_at, s_now + AK::Duration::from_seconds(60));
}

TEST_CASE(parse_alt_svc_leaves_out_other_protocols)
{
    auto alternatives = parse_alt_svc("h2=\":443\", h3-29=\":443\", h3=\":8443\""sv, s_now);
    EXPECT(alternatives.has_value());
    EXPECT_EQ(alternatives->size(), 1u);
    EXPECT_EQ(alternatives->at(0).port, 8443);

    alternatives = parse_alt_svc("h2=\":443\""sv, s_now);
    EXPECT(alternatives.has_value());
    EXPECT(alternatives->is_empty());

    alternatives = parse_alt_svc("clear"sv, s_now);
    EXPECT(alternatives.has_value());
    EXPECT(alternatives->is_empty());
}

TEST_CASE(parse_alt_svc_rejects_malformed_values)
{
    EXPECT(!parse_alt_svc(""sv, s_now).has_value());
    EXPECT(!parse_alt_svc("Clear"sv, s_now).has_value());
    EXPECT(!parse_alt_svc("h3=:443"sv, s_now).has_value());
    EXPECT(!parse_alt_svc("h3=\":443"sv, s_now).has_value());
    EXPECT(!parse_alt_svc("h3=\":443\"; ma=-1"sv, s_now).has_value());
    EXPECT(!parse_alt_svc("h3=\":443\"; ma"sv, s_now).has_value());
    EXPECT(!parse_alt_svc("h3=\":443\" junk"sv, s_now).has_value());
    EXPECT(!parse_alt_svc("%6=\":443\""sv, s_now).has_value());

    auto alternatives = parse_alt_svc("h3=\"no-port\", h3=\"a b:1\", h3=\":0\", h3=\":443\""sv, s_now);
    EXPECT(alternatives.has_value());
    EXPECT_EQ(alternatives->size(), 1u);
}

TEST_CASE(alternative_service_cache)
{
    AlternativeServiceCache cache;
    auto origin = url("https://example.com/path"sv);

    cache.update(IsPrivate::No, origin, "h3=\":443\"; ma=60"sv, s_now);
    EXPECT(cache.find(IsPrivate::No, origin, s_now).has_value());
    EXPECT(cache.find(IsPrivate::No, url("https://example.com:8443/"sv), s_now) == OptionalNone {});
    EXPECT(cache.find(IsPrivate::Yes, origin, s_now) == OptionalNone {});

    EXPECT(cache.find(IsPrivate::No, origin, s_now + AK::Duration::from_seconds(60)) == OptionalNone {});

    cache.update(IsPrivate::No, origin, "h3=\":443\""sv, s_now);
    cache.update(IsPrivate::No, origin, "h3=\"alt.example:443\""sv, s_now);
    EXPECT_EQ(cache.find(IsPrivate::No, origin, s_now)->host, "alt.example"sv);
    cache.update(IsPrivate::No, origin, "clear"sv, s_now);
    EXPECT(cache.find(IsPrivate::No, origin, s_now) == OptionalNone {});

    cache.update(IsPrivate::No, origin, "h3=\":443\""sv, s_now);
    cache.update(IsPrivate::No, origin, "h3="sv, s_now);
    EXPECT(cache.find(IsPrivate::No, origin, s_now).has_value());

    cache.update(IsPrivate::No, origin, "h3=\":443\", h3=\":8443\""sv, s_now);
    cache.mark_broken(IsPrivate::No, origin, *cache.find(IsPrivate::No, origin, s_now), s_now);
    EXPECT_EQ(cache.find(IsPrivate::No, origin, s_now)->port, 8443);
    cache.update(IsPrivate::No, origin, "h3=\":443\""sv, s_now);
    EXPECT(cache.find(IsPrivate::No, origin, s_now) == OptionalNone {});
    auto later = s_now + AlternativeServiceCache::broken_alternative_timeout;
    cache.update(IsPrivate::No, origin, "h3=\":443\""sv, later);
    EXPECT_EQ(cache.find(IsPrivate::No, origin, later)->port, 443);
}

TEST_CASE(alternative_service_cache_clearing)
{
    AlternativeServiceCache cache;
    auto first = url("https://first.example/"sv);
    auto second = url("https://second.example/"sv);

    cache.update(IsPrivate::No, first, "h3=\":443\""sv, s_now);
    cache.update(IsPrivate::No, second, "h3=\":443\""sv, s_now + AK::Duration::from_seconds(10));
    cache.update(IsPrivate::Yes, first, "h3=\":443\""sv, s_now);

    cache.remove_entries_received_since(s_now + AK::Duration::from_seconds(5));
    EXPECT(cache.find(IsPrivate::No, first, s_now).has_value());
    EXPECT(cache.find(IsPrivate::No, second, s_now) == OptionalNone {});

    cache.clear(IsPrivate::Yes);
    EXPECT(cache.find(IsPrivate::Yes, first, s_now) == OptionalNone {});
    EXPECT(cache.find(IsPrivate::No, first, s_now).has_value());
}

TEST_CASE(alternative_service_cache_is_bounded)
{
    AlternativeServiceCache cache;

    StringBuilder many;
    for (size_t i = 0; i < AlternativeServiceCache::maximum_alternatives_per_origin + 4; ++i)
        many.appendff("{}h3=\":{}\"", i == 0 ? "" : ", ", 1000 + i);
    cache.update(IsPrivate::No, url("https://example.com/"sv), many.string_view(), s_now);

    for (size_t i = 0; i < AlternativeServiceCache::maximum_origins + 1; ++i)
        cache.update(IsPrivate::No, url(ByteString::formatted("https://host{}.example/", i)), "h3=\":443\""sv, s_now + AK::Duration::from_seconds(i + 1));

    EXPECT(cache.find(IsPrivate::No, url("https://example.com/"sv), s_now) == OptionalNone {});
    EXPECT(cache.find(IsPrivate::No, url(ByteString::formatted("https://host{}.example/", AlternativeServiceCache::maximum_origins)), s_now).has_value());
}
