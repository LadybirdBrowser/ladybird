/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibDatabase/Database.h>
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

TEST_CASE(persisted_policies_round_trip_on_fresh_database)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::HSTSStore::migrate_schema(*database)), Database::MigrationOutcome::Success);

    {
        auto store = TRY_OR_FAIL(WebView::HSTSStore::create(*database));
        store->store_policy("example.test"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });

        // The store flushes dirty policies to the database on destruction.
    }

    auto store = TRY_OR_FAIL(WebView::HSTSStore::create(*database));
    EXPECT(store->is_known_hsts_host("example.test"sv));
}

TEST_CASE(newer_hsts_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('HSTSPolicies', 99);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::HSTSStore::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::HSTSStore::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}
