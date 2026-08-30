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
