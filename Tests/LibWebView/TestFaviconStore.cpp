/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibWebView/FaviconStore.h>

static void expect_store_behaviors(WebView::FaviconStore& store)
{
    auto favicon = MUST(ByteBuffer::copy("not really a png"sv.bytes()));
    auto hash = store.add_favicon(favicon);
    VERIFY(hash.has_value());

    auto stored_favicon = store.favicon_png(*hash);
    VERIFY(stored_favicon.has_value());
    EXPECT(stored_favicon->bytes() == favicon.bytes());
    EXPECT_EQ(store.add_favicon(favicon), hash);
    EXPECT(!store.favicon_png("unknown"sv).has_value());
    EXPECT(!store.add_favicon({}).has_value());

    auto oversized = MUST(ByteBuffer::create_zeroed(WebView::FaviconStore::MAXIMUM_FAVICON_BYTE_COUNT + 1));
    EXPECT(!store.add_favicon(move(oversized)).has_value());
}

static void expect_unreferenced_favicons_are_removed(WebView::FaviconStore& store)
{
    auto first = store.add_favicon(MUST(ByteBuffer::copy("first"sv.bytes())));
    auto second = store.add_favicon(MUST(ByteBuffer::copy("second"sv.bytes())));
    auto third = store.add_favicon(MUST(ByteBuffer::copy("third"sv.bytes())));
    VERIFY(first.has_value());
    VERIFY(second.has_value());
    VERIFY(third.has_value());

    HashTable<String> referenced_hashes;
    referenced_hashes.set(*first);
    store.remove_unreferenced_favicons(referenced_hashes);

    EXPECT(store.favicon_png(*first).has_value());
    EXPECT(!store.favicon_png(*second).has_value());
    EXPECT(!store.favicon_png(*third).has_value());

    store.remove_unreferenced_favicons({});
    EXPECT(!store.favicon_png(*first).has_value());
}

TEST_CASE(persisted_favicons_round_trip_and_deduplicate)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::FaviconStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto store = TRY_OR_FAIL(WebView::FaviconStore::create(*database));

    expect_store_behaviors(*store);

    auto row_count_statement = TRY_OR_FAIL(database->prepare_statement("SELECT COUNT(*) FROM Favicons;"sv));
    i64 row_count = 0;
    database->execute_statement(row_count_statement, [&](auto statement_id) -> ErrorOr<void> {
        row_count = database->result_column<i64>(statement_id, 0);
        return {};
    });
    EXPECT_EQ(row_count, 1);
}

TEST_CASE(transient_favicons_round_trip_and_deduplicate)
{
    auto store = WebView::FaviconStore::create();
    expect_store_behaviors(*store);
}

TEST_CASE(persisted_unreferenced_favicons_are_removed)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::FaviconStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto store = TRY_OR_FAIL(WebView::FaviconStore::create(*database));
    expect_unreferenced_favicons_are_removed(*store);
}

TEST_CASE(transient_unreferenced_favicons_are_removed)
{
    auto store = WebView::FaviconStore::create();
    expect_unreferenced_favicons_are_removed(*store);
}

TEST_CASE(newer_favicon_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('Favicons', 99);"sv));

    EXPECT_EQ(TRY_OR_FAIL(WebView::FaviconStore::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(WebView::FaviconStore::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}
