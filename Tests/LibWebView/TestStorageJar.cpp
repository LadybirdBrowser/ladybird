/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibWebView/StorageJar.h>

TEST_CASE(storage_round_trips_on_fresh_database)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));

    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> {});
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16, "bar"_utf16);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> { "bar"_utf16 });

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("WebStorage"sv)), Optional<u32> { 2u });
}

TEST_CASE(newer_storage_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('WebStorage', 99);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(migrates_web_storage_table_created_before_last_access_time)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw(R"#(
        CREATE TABLE WebStorage (
            storage_endpoint INTEGER,
            storage_key TEXT,
            bottle_key TEXT,
            bottle_value TEXT,
            PRIMARY KEY(storage_endpoint, storage_key, bottle_key)
        );
    )#"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16, "bar"_utf16);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> { "bar"_utf16 });

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("WebStorage"sv)), Optional<u32> { 2u });
}

TEST_CASE(migration_to_add_last_access_time_is_idempotent)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw(R"#(
        CREATE TABLE WebStorage (
            storage_endpoint INTEGER,
            storage_key TEXT,
            bottle_key TEXT,
            bottle_value TEXT,
            last_access_time INTEGER,
            PRIMARY KEY(storage_endpoint, storage_key, bottle_key)
        );
    )#"));
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('WebStorage', 1);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16, "bar"_utf16);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_utf16), Optional<Utf16String> { "bar"_utf16 });

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("WebStorage"sv)), Optional<u32> { 2u });
}

TEST_CASE(storage_usage_tracks_item_sizes)
{
    auto jar = WebView::StorageJar::create();

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16, "value"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 8u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16, "v"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 4u);

    jar->remove_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "key"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 0u);
}

TEST_CASE(storage_usage_counts_utf8_bytes_not_code_units)
{
    auto jar = WebView::StorageJar::create();

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16, "\u00e9"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 3u);

    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "k"_utf16, "\U0001f600"_utf16);
    EXPECT_EQ(jar->usage("https://example.com"_string), 5u);
}

TEST_CASE(storage_quota_is_enforced_per_storage_key)
{
    auto jar = WebView::StorageJar::create();

    auto large_value = Utf16String::repeated('x', Web::StorageAPI::StorageEndpoint::LOCAL_STORAGE_QUOTA / 2 - 16);

    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "a"_utf16, large_value).has<Optional<Utf16String>>());
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "b"_utf16, large_value).has<Optional<Utf16String>>());

    // A third value exceeds the quota for this storage key.
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "c"_utf16, large_value).has<WebView::StorageOperationError>());

    // A different storage key has its own quota.
    EXPECT(jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://other.example"_string, "a"_utf16, large_value).has<Optional<Utf16String>>());
}
