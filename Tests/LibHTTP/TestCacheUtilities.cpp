/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>

TEST_CASE(is_cacheable_must_understand_ignores_no_store_for_understood_status)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "must-understand, no-store, max-age=3600" } });
    EXPECT(HTTP::is_cacheable(200, *headers));
}

TEST_CASE(is_cacheable_must_understand_rejects_unknown_status)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "must-understand, no-store, max-age=3600" } });
    EXPECT(!HTTP::is_cacheable(202, *headers));
}

TEST_CASE(is_cacheable_no_store_without_must_understand)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "no-store, max-age=3600" } });
    EXPECT(!HTTP::is_cacheable(200, *headers));
}

TEST_CASE(is_cacheable_must_understand_without_no_store_understood_status)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "must-understand, max-age=3600" } });
    EXPECT(HTTP::is_cacheable(200, *headers));
}

TEST_CASE(is_cacheable_must_understand_without_no_store_unknown_status)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "must-understand, max-age=3600" } });
    EXPECT(!HTTP::is_cacheable(299, *headers));
}

TEST_CASE(is_cacheable_must_understand_accepts_304_status)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "must-understand, no-store, max-age=3600" } });
    EXPECT(HTTP::is_cacheable(304, *headers));
}

TEST_CASE(is_cacheable_rejects_valued_must_understand)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "must-understand=x, no-store, max-age=3600" } });
    EXPECT(!HTTP::is_cacheable(200, *headers));
}

TEST_CASE(permanent_redirects_have_a_long_heuristic_freshness_lifetime)
{
    auto headers = HTTP::HeaderList::create();
    auto one_year = AK::Duration::from_seconds(365 * 24 * 60 * 60);

    EXPECT_EQ(HTTP::calculate_freshness_lifetime(301, *headers), one_year);
    EXPECT_EQ(HTTP::calculate_freshness_lifetime(308, *headers), one_year);
    EXPECT_EQ(HTTP::calculate_freshness_lifetime(302, *headers), AK::Duration {});
}

TEST_CASE(explicit_freshness_overrides_permanent_redirect_heuristic)
{
    auto headers = HTTP::HeaderList::create({ { "Cache-Control", "max-age=42" } });
    EXPECT_EQ(HTTP::calculate_freshness_lifetime(301, *headers), AK::Duration::from_seconds(42));
}

TEST_CASE(overflowing_age_saturates)
{
    auto headers = HTTP::HeaderList::create({ { "Age", "9223372036854775808" } });
    auto now = UnixDateTime::now();

    auto age = HTTP::calculate_age(*headers, now, now);
    EXPECT_EQ(age.to_truncated_seconds(), 2'147'483'648);
}

TEST_CASE(state_changing_response_fields_are_not_stored)
{
    auto response_headers = HTTP::HeaderList::create({
        { "Content-Type", "text/html" },
        { "Set-Cookie", "session=stale" },
        { "Strict-Transport-Security", "max-age=0" },
    });

    auto stored_headers = HTTP::HeaderList::create();
    HTTP::store_header_and_trailer_fields(*stored_headers, *response_headers);

    EXPECT(stored_headers->contains("Content-Type"sv));
    EXPECT(!stored_headers->contains("Set-Cookie"sv));
    EXPECT(!stored_headers->contains("Strict-Transport-Security"sv));
}

TEST_CASE(must_revalidate_takes_precedence_over_stale_while_revalidate)
{
    auto request_headers = HTTP::HeaderList::create();
    auto freshness_lifetime = AK::Duration::from_seconds(60);
    auto current_age = AK::Duration::from_seconds(70);

    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control", "max-age=60, stale-while-revalidate=30, must-revalidate" },
        { "ETag", "\"v1\"" },
    });
    EXPECT_EQ(HTTP::cache_lifetime_status(*request_headers, *response_headers, freshness_lifetime, current_age), HTTP::CacheLifetimeStatus::MustRevalidate);

    response_headers = HTTP::HeaderList::create({
        { "Cache-Control", "max-age=60, stale-while-revalidate=30" },
        { "ETag", "\"v1\"" },
    });
    EXPECT_EQ(HTTP::cache_lifetime_status(*request_headers, *response_headers, freshness_lifetime, current_age), HTTP::CacheLifetimeStatus::StaleWhileRevalidate);
}

TEST_CASE(vary_wildcard_never_produces_a_vary_key)
{
    auto request_headers = HTTP::HeaderList::create({ { "Accept", "text/html" } });

    EXPECT(!HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create({ { "Vary", "*" } })).has_value());
    EXPECT(!HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create({ { "Vary", "Accept, *" } })).has_value());
    EXPECT(!HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create({ { "Vary", "," }, { "Vary", "*" } })).has_value());

    EXPECT_EQ(HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create()), Optional<u64> { 0 });
}

TEST_CASE(vary_key_does_not_depend_on_order_or_case_of_nominated_fields)
{
    auto request_headers = HTTP::HeaderList::create({ { "Accept", "text/html" }, { "X-Variant", "a" } });

    auto vary_key = HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create({ { "Vary", "Accept, X-Variant" } }));
    EXPECT_EQ(HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create({ { "Vary", "x-variant, ACCEPT" } })), vary_key);
    EXPECT_EQ(HTTP::create_vary_key(*request_headers, HTTP::HeaderList::create({ { "Vary", "X-Variant" }, { "Vary", "Accept, X-Variant, accept" } })), vary_key);
}

TEST_CASE(vary_key_frames_each_nominated_field)
{
    auto response_headers = HTTP::HeaderList::create({ { "Vary", "Accept, Accept-Language, Authorization" } });

    auto first_request_headers = HTTP::HeaderList::create({ { "Accept", "x" }, { "Accept-Language", "b" }, { "Authorization", "A" } });
    auto second_request_headers = HTTP::HeaderList::create({ { "Accept", "x" }, { "Accept-Language", "" }, { "Authorization", "bA" } });
    EXPECT_NE(HTTP::create_vary_key(*first_request_headers, *response_headers), HTTP::create_vary_key(*second_request_headers, *response_headers));

    auto absent_request_headers = HTTP::HeaderList::create({ { "Accept", "x" }, { "Authorization", "bA" } });
    EXPECT_NE(HTTP::create_vary_key(*second_request_headers, *response_headers), HTTP::create_vary_key(*absent_request_headers, *response_headers));
}

TEST_CASE(vary_key_normalizes_list_based_fields)
{
    auto response_headers = HTTP::HeaderList::create({ { "Vary", "Accept" } });

    auto vary_key = HTTP::create_vary_key(*HTTP::HeaderList::create({ { "Accept", "text/html, application/json;q=0.5" } }), *response_headers);
    EXPECT_EQ(HTTP::create_vary_key(*HTTP::HeaderList::create({ { "Accept", "Application/JSON;q=0.5,text/html" } }), *response_headers), vary_key);
    EXPECT_EQ(HTTP::create_vary_key(*HTTP::HeaderList::create({ { "Accept", "application/json;q=0.5" }, { "Accept", "text/html" } }), *response_headers), vary_key);
}

TEST_CASE(vary_key_keeps_quoted_strings_intact)
{
    auto response_headers = HTTP::HeaderList::create({ { "Vary", "Accept" } });

    // Comma splitting loses the association between parameters and media types.
    auto first_request_headers = HTTP::HeaderList::create({ { "Accept", "text/a;p=\"x,y\", text/b;p=\"z,w\"" } });
    auto second_request_headers = HTTP::HeaderList::create({ { "Accept", "text/a;p=\"x,w\", text/b;p=\"z,y\"" } });
    EXPECT_NE(HTTP::create_vary_key(*first_request_headers, *response_headers), HTTP::create_vary_key(*second_request_headers, *response_headers));

    auto lowercase_request_headers = HTTP::HeaderList::create({ { "Accept", "text/html;p=\"value\"" } });
    auto uppercase_request_headers = HTTP::HeaderList::create({ { "Accept", "TEXT/HTML;p=\"VALUE\"" } });
    EXPECT_NE(HTTP::create_vary_key(*lowercase_request_headers, *response_headers), HTTP::create_vary_key(*uppercase_request_headers, *response_headers));
}
