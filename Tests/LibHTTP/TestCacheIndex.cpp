/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
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
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers_v1);
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
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers);
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
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers);
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
    auto vary_key = HTTP::create_vary_key(*request_headers, *response_headers);
    auto now = UnixDateTime::now();

    state.index.set_maximum_disk_cache_size(80);

    for (u64 cache_key = 1; cache_key <= 5; ++cache_key)
        TRY_OR_FAIL(state.index.create_entry(cache_key, vary_key, "https://example.com/script.js"_string, request_headers, response_headers, 10, now, now));
    state.index.update_associated_data_size(1, vary_key, 50);

    EXPECT_EQ(state.index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total, 100u);

    Vector<u64> removed_entries;
    state.index.remove_entries_exceeding_cache_limit([&](auto removed_cache_key, auto) {
        removed_entries.append(removed_cache_key);
    });

    EXPECT(removed_entries.size() > 0);
    EXPECT(state.index.estimate_cache_size_accessed_since(UnixDateTime::earliest()).total <= 80u);
}
