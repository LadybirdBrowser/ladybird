/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibWebView/DownloadStore.h>

static NonnullRefPtr<Database::Database> create_database()
{
    return MUST(Database::Database::create_memory_backed());
}

static NonnullOwnPtr<WebView::DownloadStore> create_store(Database::Database& database)
{
    EXPECT_EQ(MUST(WebView::DownloadStore::migrate_schema(database)), Database::MigrationOutcome::Success);
    return MUST(WebView::DownloadStore::create(database));
}

static WebView::DownloadRecord example_download(u64 id = 7)
{
    return WebView::DownloadRecord {
        .id = id,
        .url = "https://cdn.example.com/big.iso"_string,
        .display_url = "https://example.com/big.iso"_string,
        .destination = "/home/anon/Downloads/big.iso"_string,
        .temporary_destination = "/home/anon/Downloads/big.iso.7.abc.download"_string,
        .total_size = 4096,
        .etag = "\"abc123\""_string,
        .last_modified = {},
        .segments = { { 0, 2047, 1024 }, { 2048, 4095, 2048 } },
        .created_time = UnixDateTime::from_seconds_since_epoch(1000),
        .can_restart_from_zero = true,
    };
}

TEST_CASE(downloads_round_trip)
{
    auto database = create_database();
    auto store = create_store(database);

    store->save_download(example_download());

    auto downloads = store->resumable_downloads();
    EXPECT_EQ(downloads.size(), 1u);

    auto const& download = downloads.first();
    EXPECT_EQ(download.id, 7u);
    EXPECT_EQ(download.url, "https://cdn.example.com/big.iso"_string);
    EXPECT_EQ(download.display_url, "https://example.com/big.iso"_string);
    EXPECT_EQ(download.destination, "/home/anon/Downloads/big.iso"_string);
    EXPECT_EQ(download.temporary_destination, "/home/anon/Downloads/big.iso.7.abc.download"_string);
    EXPECT_EQ(download.total_size, 4096u);
    EXPECT_EQ(download.etag, "\"abc123\""_string);
    EXPECT(!download.last_modified.has_value());
    EXPECT_EQ(download.created_time, UnixDateTime::from_seconds_since_epoch(1000));
    EXPECT(download.can_restart_from_zero);

    EXPECT_EQ(download.segments.size(), 2u);
    EXPECT_EQ(download.segments[0].start_offset, 0u);
    EXPECT_EQ(download.segments[0].end_offset, 2047u);
    EXPECT_EQ(download.segments[0].next_offset, 1024u);
    EXPECT_EQ(download.segments[1].start_offset, 2048u);
    EXPECT_EQ(download.segments[1].end_offset, 4095u);
    EXPECT_EQ(download.segments[1].next_offset, 2048u);
}

TEST_CASE(saving_a_download_again_replaces_its_segments)
{
    auto database = create_database();
    auto store = create_store(database);

    store->save_download(example_download());

    auto updated = example_download();
    updated.segments = { { 0, 4095, 3000 } };
    updated.can_restart_from_zero = false;
    store->save_download(updated);

    auto downloads = store->resumable_downloads();
    EXPECT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads.first().segments.size(), 1u);
    EXPECT_EQ(downloads.first().segments[0].next_offset, 3000u);
    EXPECT(!downloads.first().can_restart_from_zero);
}

TEST_CASE(downloads_can_be_removed)
{
    auto database = create_database();
    auto store = create_store(database);

    store->save_download(example_download(1));
    store->save_download(example_download(2));
    store->remove_download(1);

    auto downloads = store->resumable_downloads();
    EXPECT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads.first().id, 2u);

    store->remove_download(99);
    EXPECT_EQ(store->resumable_downloads().size(), 1u);
}

TEST_CASE(maximum_download_id_outlives_the_rows_it_counted)
{
    auto database = create_database();
    auto store = create_store(database);

    EXPECT_EQ(store->maximum_download_id(), 0u);

    store->save_download(example_download(4));
    store->save_download(example_download(9));
    EXPECT_EQ(store->maximum_download_id(), 9u);

    store->remove_download(9);
    EXPECT_EQ(store->maximum_download_id(), 4u);
}

TEST_CASE(a_download_with_unreadable_segments_is_skipped)
{
    auto database = create_database();
    auto store = create_store(database);

    store->save_download(example_download(1));
    store->save_download(example_download(2));

    auto corrupt = MUST(database->prepare_statement("UPDATE Downloads SET segments = ? WHERE id = ?;"sv));
    database->execute_statement(corrupt, {}, "not json at all"_string, static_cast<u64>(1));

    auto downloads = store->resumable_downloads();
    EXPECT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads.first().id, 2u);

    database->execute_statement(corrupt, {}, "[]"_string, static_cast<u64>(2));
    EXPECT(store->resumable_downloads().is_empty());
}

TEST_CASE(a_download_with_nonsensical_segment_offsets_is_skipped)
{
    auto database = create_database();
    auto store = create_store(database);

    auto save_with_segments = [&](u64 id, Vector<WebView::DownloadSegmentRecord> segments) {
        auto download = example_download(id);
        download.segments = move(segments);
        store->save_download(download);
    };

    save_with_segments(1, { { 0, 2047, 1024 }, { 2048, 4095, 2048 } });
    save_with_segments(2, { { 0, 2047, 1024 }, { 1024, 4095, 2048 } });
    save_with_segments(3, { { 0, 2047, 1024 }, { 4096, 4095, 4096 } });
    save_with_segments(4, { { 2047, 0, 0 } });
    save_with_segments(5, { { 1, 4095, 1 } });
    save_with_segments(6, { { 0, 2047, 4000 } });
    save_with_segments(7, { { 2048, 4095, 1024 } });

    auto downloads = store->resumable_downloads();
    EXPECT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads.first().id, 1u);
}

TEST_CASE(a_disabled_store_remembers_nothing)
{
    auto store = WebView::DownloadStore::create_disabled();

    store->save_download(example_download());

    EXPECT(store->resumable_downloads().is_empty());
    EXPECT_EQ(store->maximum_download_id(), 0u);

    store->remove_download(7);
}

TEST_CASE(a_database_left_at_an_older_schema_is_brought_forward)
{
    auto database = create_database();

    MUST(database->execute_raw(R"#(
        CREATE TABLE Downloads (
            id INTEGER PRIMARY KEY,
            url TEXT NOT NULL,
            display_url TEXT NOT NULL,
            destination TEXT NOT NULL,
            temporary_destination TEXT NOT NULL,
            total_size INTEGER NOT NULL,
            etag TEXT NOT NULL,
            last_modified TEXT NOT NULL,
            segments TEXT NOT NULL,
            created_time INTEGER NOT NULL,
            updated_time INTEGER NOT NULL
        );
        CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);
        INSERT INTO SchemaVersions (store, version) VALUES ('Downloads', 1);
        INSERT INTO Downloads VALUES (5, 'https://example.com/a', 'https://example.com/a', '/tmp/a', '/tmp/a.download', 4096, '"e"', '', '[{"start":0,"end":4095,"next":100}]', 10, 10);
    )#"));

    EXPECT_EQ(MUST(WebView::DownloadStore::migrate_schema(database)), Database::MigrationOutcome::Success);

    auto store = MUST(WebView::DownloadStore::create(database));

    auto downloads = store->resumable_downloads();
    EXPECT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads.first().id, 5u);

    EXPECT(!downloads.first().can_restart_from_zero);

    store->save_download(example_download(6));
    EXPECT_EQ(store->resumable_downloads().size(), 2u);
}

TEST_CASE(migrating_an_already_migrated_schema_is_a_no_op)
{
    auto database = create_database();

    EXPECT_EQ(MUST(WebView::DownloadStore::migrate_schema(database)), Database::MigrationOutcome::Success);
    EXPECT_EQ(MUST(WebView::DownloadStore::migrate_schema(database)), Database::MigrationOutcome::Success);
    EXPECT_EQ(MUST(WebView::DownloadStore::migrate_schema(database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::Success);
}
