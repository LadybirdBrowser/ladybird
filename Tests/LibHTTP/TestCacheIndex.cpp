/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/NumericLimits.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibDatabase/Database.h>
#include <LibHTTP/Cache/CacheIndex.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibTest/TestCase.h>

struct CacheIndexTestState {
    NonnullRefPtr<Database::Database> database;
    HTTP::CacheIndex index;
};

static LexicalPath cache_directory()
{
    auto cache_directory = LexicalPath { Core::StandardPaths::cache_directory() };
    MUST(Core::Directory::create(cache_directory, Core::Directory::CreateDirectories::Yes));
    return cache_directory;
}

static CacheIndexTestState create_cache_index()
{
    auto database = MUST(Database::Database::create_memory_backed());
    VERIFY(MUST(HTTP::CacheIndex::migrate_schema(*database)) == Database::MigrationOutcome::Success);
    auto index = MUST(HTTP::CacheIndex::create(*database, cache_directory()));
    return { move(database), move(index) };
}

TEST_CASE(create_entry_replaces_loaded_entry)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers_v1 = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "v1"sv },
    });
    auto response_headers_v2 = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "ETag"sv, "v2"sv },
    });

    auto cache_key = 1u;
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers_v1).value();
    auto now = UnixDateTime::now();

    TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com"_string, request_headers, response_headers_v1, 10, now, now));

    auto entry = state.index.find_entry(cache_key, *request_headers);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->data_size, 10u);
    EXPECT_EQ(entry->response_headers->get("ETag"sv), Optional<ByteString> { ByteString { "v1"sv } });

    TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com"_string, request_headers, response_headers_v2, 20, now, now));

    entry = state.index.find_entry(cache_key, *request_headers);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->data_size, 20u);
    EXPECT_EQ(entry->response_headers->get("ETag"sv), Optional<ByteString> { ByteString { "v2"sv } });
}

TEST_CASE(remove_entries_exceeding_cache_limit_is_noop_when_under_limit)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create();
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();
    auto now = UnixDateTime::now();

    state.index.set_maximum_disk_cache_size(80);

    for (u64 cache_key = 1; cache_key <= 5; ++cache_key)
        TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com"_string, request_headers, response_headers, 10, now, now));

    Vector<u64> removed_entries;
    state.index.remove_entries_exceeding_cache_limit([&](auto removed_cache_key, auto) {
        removed_entries.append(removed_cache_key);
    });

    EXPECT_EQ(removed_entries.size(), 0u);
    EXPECT_EQ(state.index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 50u);
}

TEST_CASE(remove_entries_exceeding_cache_limit_tolerates_replaced_unloaded_entries)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create();
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();
    auto now = UnixDateTime::now();

    state.index.set_maximum_disk_cache_size(80);

    for (u64 cache_key = 1; cache_key <= 8; ++cache_key)
        TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com"_string, request_headers, response_headers, 10, now, now));

    auto reloaded_index = MUST(HTTP::CacheIndex::create(*state.database, cache_directory()));
    TRY_OR_FAIL(reloaded_index.create_entry(1, vary_key, "https://example.com"_string, request_headers, response_headers, 10, now, now));

    Vector<u64> removed_entries;
    reloaded_index.remove_entries_exceeding_cache_limit([&](auto removed_cache_key, auto) {
        removed_entries.append(removed_cache_key);
    });

    EXPECT_EQ(removed_entries.size(), 0u);
    EXPECT_EQ(reloaded_index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 80u);
}

TEST_CASE(associated_data_counts_toward_cache_size)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create();
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();
    auto now = UnixDateTime::now();

    state.index.set_maximum_disk_cache_size(80);

    for (u64 cache_key = 1; cache_key <= 5; ++cache_key)
        TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com/script.js"_string, request_headers, response_headers, 10, now, now));
    TRY_OR_FAIL(state.index.update_associated_data_size(1, vary_key, 50));

    EXPECT_EQ(state.index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 100u);

    Vector<u64> removed_entries;
    state.index.remove_entries_exceeding_cache_limit([&](auto removed_cache_key, auto) {
        removed_entries.append(removed_cache_key);
    });

    EXPECT(removed_entries.size() > 0);
    EXPECT(state.index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total <= 80u);
}

TEST_CASE(newer_cache_index_schema_reports_database_too_new)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('CacheIndex', 99);"sv));

    EXPECT_EQ(TRY_OR_FAIL(HTTP::CacheIndex::migrate_schema(*database)), Database::MigrationOutcome::DatabaseTooNew);
    EXPECT_EQ(TRY_OR_FAIL(HTTP::CacheIndex::migrate_schema(*database, Database::MigrationMode::CheckOnly)), Database::MigrationOutcome::DatabaseTooNew);
}

TEST_CASE(entries_with_previous_vary_keys_are_dropped_on_migration)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());

    TRY_OR_FAIL(database->execute_raw("CREATE TABLE SchemaVersions (store TEXT PRIMARY KEY, version INTEGER NOT NULL);"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO SchemaVersions (store, version) VALUES ('CacheIndex', 2);"sv));
    TRY_OR_FAIL(database->execute_raw("CREATE TABLE CacheIndex (cache_key INTEGER, vary_key INTEGER, url TEXT, request_headers BLOB, response_headers BLOB, data_size INTEGER, associated_data_size INTEGER, request_time INTEGER, response_time INTEGER, last_access_time INTEGER, PRIMARY KEY(cache_key, vary_key));"sv));
    TRY_OR_FAIL(database->execute_raw("INSERT INTO CacheIndex VALUES (1, 0, 'https://example.com', x'', x'', 10, 0, 0, 0, 0);"sv));

    EXPECT_EQ(TRY_OR_FAIL(HTTP::CacheIndex::migrate_schema(*database)), Database::MigrationOutcome::Success);

    auto index = MUST(HTTP::CacheIndex::create(*database, cache_directory()));
    EXPECT(!index.find_entry(1, *HTTP::HeaderList::create()).has_value());
    EXPECT_EQ(index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 0u);
}

TEST_CASE(full_range_cache_keys_round_trip)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create({ { "Cache-Control"sv, "max-age=60"sv } });
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();
    auto now = UnixDateTime::now();

    for (u64 cache_key : { static_cast<u64>(NumericLimits<i64>::max()) + 1, NumericLimits<u64>::max() }) {
        TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com"_string, request_headers, response_headers, 10, now, now));

        auto entry = state.index.find_entry(cache_key, *request_headers);
        EXPECT(entry.has_value());
        EXPECT_EQ(entry->vary_key, vary_key);
        EXPECT(state.index.has_entry(cache_key, vary_key));
    }
}

TEST_CASE(negative_stored_sizes_are_skipped_as_corrupt)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create({ { "Cache-Control"sv, "max-age=60"sv } });
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();
    auto now = UnixDateTime::now();

    TRY_OR_FAIL(state.index.create_entry(1, vary_key, "https://example.com"_string, request_headers, response_headers, 10, now, now));
    TRY_OR_FAIL(state.database->execute_raw("UPDATE CacheIndex SET data_size = -5;"sv));

    auto reloaded_index = MUST(HTTP::CacheIndex::create(*state.database, cache_directory()));
    auto entry = reloaded_index.find_entry(1, *request_headers);
    EXPECT(!entry.has_value());
}

TEST_CASE(creation_returns_error_for_corrupted_database)
{
    auto database = TRY_OR_FAIL(Database::Database::create_memory_backed());
    TRY_OR_FAIL(HTTP::CacheIndex::migrate_schema(*database));

    // SQLite stores the root page of each b-tree in sqlite_schema. CacheIndex's table and primary-key
    // index use separate b-trees with different page layouts. Point the table's rootpage at the index's
    // b-tree, then reload the schema with writable_schema = RESET.
    // The column definitions remain valid, so statement preparation succeeds. When CacheIndex::create()
    // scans the table to estimate its size, SQLite finds an index page where it expects a table page
    // and reports SQLITE_CORRUPT ("database disk image is malformed").
    TRY_OR_FAIL(database->execute_raw(R"#(
        PRAGMA writable_schema = ON;
        UPDATE sqlite_schema
        SET rootpage = (SELECT rootpage FROM sqlite_schema WHERE type = 'index' AND tbl_name = 'CacheIndex')
        WHERE name = 'CacheIndex';
        PRAGMA writable_schema = RESET;
    )#"sv));

    auto result = HTTP::CacheIndex::create(*database, cache_directory());
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().string_literal(), "database disk image is malformed"sv);
}

TEST_CASE(variants_per_cache_key_are_limited)
{
    auto state = create_cache_index();

    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "X-Variant"sv },
    });
    auto now = UnixDateTime::now();

    auto request_headers_for_variant = [](size_t variant) {
        return HTTP::HeaderList::create({ { "X-Variant"sv, ByteString::number(variant) } });
    };

    auto variant_count = HTTP::MAXIMUM_CACHE_ENTRY_VARIANT_COUNT + 4;
    for (size_t variant = 0; variant < variant_count; ++variant) {
        auto request_headers = request_headers_for_variant(variant);
        auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers).value();
        TRY_OR_FAIL(state.index.create_entry(1, vary_key, "https://example.com"_string, request_headers, response_headers, 10, now, now));
    }

    // Variants persisted by an earlier index count too.
    auto reloaded_index = MUST(HTTP::CacheIndex::create(*state.database, cache_directory()));

    auto newest_request_headers = request_headers_for_variant(variant_count);
    auto newest_vary_key = HTTP::create_vary_key(*newest_request_headers, *response_headers).value();
    TRY_OR_FAIL(reloaded_index.create_entry(1, newest_vary_key, "https://example.com"_string, newest_request_headers, response_headers, 10, now, now));

    size_t removed_count = 0;
    reloaded_index.remove_variants_exceeding_limit(1, newest_vary_key, [&](auto, auto) { ++removed_count; });

    EXPECT_EQ(removed_count, variant_count + 1 - HTTP::MAXIMUM_CACHE_ENTRY_VARIANT_COUNT);
    EXPECT(reloaded_index.find_entry(1, *newest_request_headers).has_value());

    size_t remaining_count = 0;
    for (size_t variant = 0; variant <= variant_count; ++variant) {
        if (reloaded_index.find_entry(1, *request_headers_for_variant(variant)).has_value())
            ++remaining_count;
    }
    EXPECT_EQ(remaining_count, HTTP::MAXIMUM_CACHE_ENTRY_VARIANT_COUNT);
}

TEST_CASE(most_recent_matching_variant_is_selected)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create({ { "Accept"sv, "text/html"sv }, { "X-Variant"sv, "a"sv } });
    auto newer_response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "Accept"sv },
        { "ETag"sv, "newer"sv },
    });
    auto older_response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "Accept, X-Variant"sv },
        { "ETag"sv, "older"sv },
    });
    auto now = UnixDateTime::now();

    TRY_OR_FAIL(state.index.create_entry(1, HTTP::create_vary_key(*request_headers, *older_response_headers).value(), "https://example.com"_string, request_headers, older_response_headers, 10, now - AK::Duration::from_seconds(10), now - AK::Duration::from_seconds(10)));
    TRY_OR_FAIL(state.index.create_entry(1, HTTP::create_vary_key(*request_headers, *newer_response_headers).value(), "https://example.com"_string, request_headers, newer_response_headers, 10, now, now));

    auto entry = state.index.find_entry(1, *request_headers);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->response_headers->get("ETag"sv), Optional<ByteString> { ByteString { "newer"sv } });
}

TEST_CASE(updated_response_headers_respect_entry_size_limit)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create({ { "ETag"sv, "v1"sv } });
    auto now = UnixDateTime::now();

    state.index.set_maximum_disk_cache_size(800);
    TRY_OR_FAIL(state.index.create_entry(1, 0, "https://example.com"_string, request_headers, response_headers, 10, now, now));

    auto small_response_headers = HTTP::HeaderList::create({ { "ETag"sv, "v1"sv }, { "X-Small"sv, "x"sv } });
    TRY_OR_FAIL(state.index.update_response_headers(1, 0, small_response_headers));

    auto large_response_headers = HTTP::HeaderList::create({ { "ETag"sv, "v1"sv }, { "X-Large"sv, ByteString::repeated('x', 200) } });
    EXPECT(state.index.update_response_headers(1, 0, large_response_headers).is_error());

    auto entry = state.index.find_entry(1, *request_headers);
    VERIFY(entry.has_value());
    EXPECT(!entry->response_headers->contains("X-Large"sv));
}

TEST_CASE(persisted_entries_are_found_before_they_are_looked_up)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create({ { "Cache-Control"sv, "max-age=60"sv } });
    auto now = UnixDateTime::now();

    TRY_OR_FAIL(state.index.create_entry(1, 0, "https://example.com"_string, request_headers, response_headers, 10, now, now));

    auto reloaded_index = MUST(HTTP::CacheIndex::create(*state.database, cache_directory()));
    EXPECT(reloaded_index.has_entry(1, 0));

    TRY_OR_FAIL(reloaded_index.update_associated_data_size(1, 0, 5));
    EXPECT_EQ(reloaded_index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 10u + 5u + 25u);
}

TEST_CASE(stored_vary_wildcard_does_not_match)
{
    auto state = create_cache_index();

    auto request_headers = HTTP::HeaderList::create();
    auto response_headers = HTTP::HeaderList::create({
        { "Cache-Control"sv, "max-age=60"sv },
        { "Vary"sv, "*"sv },
    });
    auto now = UnixDateTime::now();

    TRY_OR_FAIL(state.index.create_entry(1, 0, "https://example.com"_string, request_headers, response_headers, 10, now, now));

    auto reloaded_index = MUST(HTTP::CacheIndex::create(*state.database, cache_directory()));
    EXPECT(!reloaded_index.find_entry(1, *request_headers).has_value());
}
