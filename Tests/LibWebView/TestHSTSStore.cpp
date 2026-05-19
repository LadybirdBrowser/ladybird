/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HSTSStore.h>

// HSTSStore is the dynamic, per-profile store only; the built-in preload list is consulted
// separately at the fetch layer (see Tests/LibHTTP/TestHSTSPreloadData.cpp). These tests use
// example.test, which is not a preloaded host, so they exercise the dynamic store in isolation.

TEST_CASE(congruent_match)
{
    auto store = WebView::HSTSStore::create();
    store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });
    EXPECT(store->is_known_hsts_host("example.test"sv));
}

TEST_CASE(superdomain_match_via_include_subdomains)
{
    auto store = WebView::HSTSStore::create();
    store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), true });
    EXPECT(store->is_known_hsts_host("sub.example.test"sv));
}

TEST_CASE(superdomain_not_matched_without_include_subdomains)
{
    auto store = WebView::HSTSStore::create();
    store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });
    EXPECT(!store->is_known_hsts_host("sub.example.test"sv));
}

TEST_CASE(unknown_host_is_not_hsts_known)
{
    auto store = WebView::HSTSStore::create();
    EXPECT(!store->is_known_hsts_host("example.test"sv));
}

TEST_CASE(max_age_zero_removes_dynamic_policy)
{
    auto store = WebView::HSTSStore::create();
    store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });
    EXPECT(store->is_known_hsts_host("example.test"sv));

    store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::zero(), false });
    EXPECT(!store->is_known_hsts_host("example.test"sv));
}

TEST_CASE(remove_policies_observed_since_clears_dynamic_data)
{
    auto store = WebView::HSTSStore::create();
    store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });
    store->remove_policies_observed_since(UnixDateTime::earliest());
    EXPECT(!store->is_known_hsts_host("example.test"sv));
}
