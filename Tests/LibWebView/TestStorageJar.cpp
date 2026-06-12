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

    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_string), Optional<String> {});
    jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_string, "bar"_string);
    EXPECT_EQ(jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://example.com"_string, "foo"_string), Optional<String> { "bar"_string });

    EXPECT_EQ(TRY_OR_FAIL(database->schema_version("WebStorage"sv)), Optional<u32> { 1u });
}

TEST_CASE(newer_storage_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('WebStorage', 99);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}
