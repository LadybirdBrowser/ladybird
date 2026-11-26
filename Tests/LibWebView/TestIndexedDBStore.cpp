/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibDatabase/Database.h>
#include <LibTest/TestCase.h>
#include <LibWebView/IndexedDBStore.h>

namespace {

struct Row {
    ByteBuffer database;
    u64 store { 0 };
    ByteBuffer key;
    ByteBuffer value;
    ByteBuffer index_keys;
};

struct Rows {
    Vector<ByteBuffer> database_names;
    Vector<ByteBuffer> metadata;
    Vector<Row> records;
};

}

static ReadonlyBytes bytes(StringView string)
{
    return string.bytes();
}

static Rows load(WebView::IndexedDBStore& store, StringView storage_key)
{
    Rows rows;
    store.load(
        storage_key,
        [&](ReadonlyBytes name, ReadonlyBytes metadata) {
            rows.database_names.append(MUST(ByteBuffer::copy(name)));
            rows.metadata.append(MUST(ByteBuffer::copy(metadata)));
        },
        [&](ReadonlyBytes database, u64 object_store, ReadonlyBytes key, ReadonlyBytes value, ReadonlyBytes index_keys) {
            rows.records.append({ MUST(ByteBuffer::copy(database)), object_store, MUST(ByteBuffer::copy(key)), MUST(ByteBuffer::copy(value)), MUST(ByteBuffer::copy(index_keys)) });
        });
    return rows;
}

static void write_database(WebView::IndexedDBStore& store, StringView storage_key, StringView database)
{
    store.begin_write(storage_key, bytes(database));
    store.put_metadata(bytes("shape"sv));
    store.put_record(1, bytes("a"sv), bytes("first"sv), bytes("index keys"sv));
    store.put_record(1, bytes("b"sv), bytes("second"sv), bytes(""sv));
    store.put_record(2, bytes("a"sv), bytes("other store"sv), bytes(""sv));
    store.end_write();
}

TEST_CASE(rows_outlive_the_store_that_wrote_them)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::IndexedDBStore::migrate_schema(*database)), Database::MigrationOutcome::Success);

    {
        auto store = TRY_OR_FAIL(WebView::IndexedDBStore::create(*database));
        write_database(*store, "https://example.com"sv, "db"sv);
        write_database(*store, "https://other.example"sv, "db"sv);

        // A later write replaces a record, and deletes another.
        store->begin_write("https://example.com"sv, bytes("db"sv));
        store->put_record(1, bytes("a"sv), bytes("replaced"sv), bytes(""sv));
        store->delete_record(1, bytes("b"sv));
        store->end_write();
    }

    auto store = TRY_OR_FAIL(WebView::IndexedDBStore::create(*database));
    auto rows = load(*store, "https://example.com"sv);
    EXPECT_EQ(rows.database_names.size(), 1u);
    EXPECT_EQ(rows.database_names[0].bytes(), bytes("db"sv));
    EXPECT_EQ(rows.metadata[0].bytes(), bytes("shape"sv));
    EXPECT_EQ(rows.records.size(), 2u);
    for (auto const& record : rows.records) {
        EXPECT_EQ(record.key.bytes(), bytes("a"sv));
        EXPECT_EQ(record.value.bytes(), bytes(record.store == 1 ? "replaced"sv : "other store"sv));
    }

    // Another storage key's rows are its own.
    EXPECT_EQ(load(*store, "https://other.example"sv).records.size(), 3u);
    EXPECT_EQ(load(*store, "https://nowhere.example"sv).database_names.size(), 0u);
}

TEST_CASE(deleting_an_object_store_or_a_database_drops_its_rows)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::IndexedDBStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto store = TRY_OR_FAIL(WebView::IndexedDBStore::create(*database));
    write_database(*store, "https://example.com"sv, "db"sv);
    write_database(*store, "https://example.com"sv, "kept"sv);

    store->begin_write("https://example.com"sv, bytes("db"sv));
    store->delete_store(1);
    store->end_write();
    auto rows = load(*store, "https://example.com"sv);
    EXPECT_EQ(rows.records.size(), 4u);
    for (auto const& record : rows.records)
        EXPECT(record.database.bytes() == bytes("kept"sv) || record.store == 2);

    store->begin_write("https://example.com"sv, bytes("db"sv));
    store->delete_database();
    store->end_write();
    rows = load(*store, "https://example.com"sv);
    EXPECT_EQ(rows.database_names.size(), 1u);
    EXPECT_EQ(rows.database_names[0].bytes(), bytes("kept"sv));
    EXPECT_EQ(rows.records.size(), 3u);
}

TEST_CASE(newer_indexed_db_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('IndexedDB', 99);"));

    EXPECT_EQ(TRY_OR_FAIL(WebView::IndexedDBStore::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(clearing_site_data_finds_and_deletes_the_storage_keys_written_since)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    EXPECT_EQ(TRY_OR_FAIL(WebView::IndexedDBStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
    auto store = TRY_OR_FAIL(WebView::IndexedDBStore::create(*database));

    auto before = UnixDateTime::now();
    write_database(*store, "https://example.com"sv, "db"sv);
    write_database(*store, "https://other.example"sv, "db"sv);

    auto storage_keys = store->storage_keys_written_since(before);
    EXPECT_EQ(storage_keys.size(), 2u);
    EXPECT(store->storage_keys_written_since(UnixDateTime::now() + AK::Duration::from_seconds(60)).is_empty());

    auto sizes = store->estimate_size_written_since(before);
    EXPECT(sizes.total > 0);
    EXPECT_EQ(sizes.since_requested_time, sizes.total);

    // Deleting one storage key's rows leaves the other's.
    store->delete_storage_key("https://example.com"sv);
    EXPECT_EQ(load(*store, "https://example.com"sv).database_names.size(), 0u);
    EXPECT_EQ(load(*store, "https://example.com"sv).records.size(), 0u);
    EXPECT_EQ(load(*store, "https://other.example"sv).records.size(), 3u);
    EXPECT_EQ(store->storage_keys_written_since(before), Vector<String> { "https://other.example"_string });
    EXPECT(store->estimate_size_written_since(before).total < sizes.total);
}
