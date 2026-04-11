/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/HistoryStore.h>

static URL::URL parse_url(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url.release_value();
}

static void populate_history_for_url_autocomplete_tests(WebView::HistoryStore& store)
{
    store.record_visit(parse_url("https://www.google.com/"sv), {}, UnixDateTime::from_seconds_since_epoch(30));
    store.record_visit(parse_url("https://x.com/"sv), {}, UnixDateTime::from_seconds_since_epoch(20));
    store.record_visit(parse_url("https://github.com/LadybirdBrowser/ladybird"sv), {}, UnixDateTime::from_seconds_since_epoch(10));
}

static void expect_history_autocomplete_ignores_url_boilerplate(WebView::HistoryStore& store)
{
    populate_history_for_url_autocomplete_tests(store);

    EXPECT(store.autocomplete_entries("https://"sv, 8).is_empty());
    EXPECT(store.autocomplete_entries("https://www."sv, 8).is_empty());
    EXPECT(store.autocomplete_entries("www."sv, 8).is_empty());

    auto git_entries = store.autocomplete_entries("git"sv, 8);
    VERIFY(git_entries.size() == 1);
    EXPECT_EQ(git_entries[0].url, "https://github.com/LadybirdBrowser/ladybird"_string);

    auto https_goo_entries = store.autocomplete_entries("https://goo"sv, 8);
    VERIFY(https_goo_entries.size() == 1);
    EXPECT_EQ(https_goo_entries[0].url, "https://www.google.com/"_string);
}

static void expect_history_autocomplete_requires_three_characters_for_title_matches(WebView::HistoryStore& store)
{
    store.record_visit(parse_url("https://example.com/"sv), "Foo bar baz wip wap wop"_string, UnixDateTime::from_seconds_since_epoch(10));

    EXPECT(store.autocomplete_entries("w"sv, 8).is_empty());
    EXPECT(store.autocomplete_entries("wi"sv, 8).is_empty());

    auto entries = store.autocomplete_entries("wip"sv, 8);
    VERIFY(entries.size() == 1);
    EXPECT_EQ(entries[0].url, "https://example.com/"_string);
}

static void expect_history_autocomplete_requires_three_characters_for_non_prefix_url_matches(WebView::HistoryStore& store)
{
    store.record_visit(parse_url("https://example.com/wip-path"sv), "Example"_string, UnixDateTime::from_seconds_since_epoch(10));

    EXPECT(store.autocomplete_entries("w"sv, 8).is_empty());
    EXPECT(store.autocomplete_entries("wi"sv, 8).is_empty());

    auto entries = store.autocomplete_entries("wip"sv, 8);
    VERIFY(entries.size() == 1);
    EXPECT_EQ(entries[0].url, "https://example.com/wip-path"_string);
}

static void expect_history_autocomplete_entries_include_metadata(WebView::HistoryStore& store)
{
    auto google_url = parse_url("https://www.google.com/"sv);
    auto github_url = parse_url("https://github.com/LadybirdBrowser/ladybird"sv);

    store.record_visit(google_url, "Google"_string, UnixDateTime::from_seconds_since_epoch(20));
    store.update_favicon(google_url, "Zm9v"_string);
    store.record_visit(github_url, "Ladybird repository"_string, UnixDateTime::from_seconds_since_epoch(10));

    auto entries = store.autocomplete_entries("goo"sv, 8);
    VERIFY(entries.size() == 1);
    EXPECT_EQ(entries[0].url, "https://www.google.com/"_string);
    EXPECT_EQ(entries[0].title, Optional<String> { "Google"_string });
    EXPECT_EQ(entries[0].favicon_base64_png, Optional<String> { "Zm9v"_string });
    EXPECT_EQ(entries[0].visit_count, 1u);
    EXPECT_EQ(entries[0].last_visited_time, UnixDateTime::from_seconds_since_epoch(20));
}

TEST_CASE(record_and_lookup_history_entries)
{
    auto store = WebView::HistoryStore::create();

    auto visited_at = UnixDateTime::from_seconds_since_epoch(1234);
    store->record_visit(parse_url("https://example.com/path#fragment"sv), "Example page"_string, visited_at);
    store->record_visit(parse_url("https://example.com/path"sv), {}, visited_at);

    auto entry = store->entry_for_url(parse_url("https://example.com/path"sv));
    VERIFY(entry.has_value());

    EXPECT_EQ(entry->url, "https://example.com/path"_string);
    EXPECT_EQ(entry->title, Optional<String> { "Example page"_string });
    EXPECT_EQ(entry->visit_count, 2u);
    EXPECT_EQ(entry->last_visited_time, visited_at);
}

TEST_CASE(history_autocomplete_prefers_url_prefix_then_recency)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("https://beta.example.com/"sv), "Alpha reference"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->record_visit(parse_url("https://alpha.example.com/"sv), "Something else"_string, UnixDateTime::from_seconds_since_epoch(20));
    store->record_visit(parse_url("https://docs.example.com/"sv), "Alpha docs"_string, UnixDateTime::from_seconds_since_epoch(30));

    auto entries = store->autocomplete_entries("alpha"sv, 8);

    VERIFY(entries.size() == 3);
    EXPECT_EQ(entries[0].url, "https://alpha.example.com/"_string);
    EXPECT_EQ(entries[1].url, "https://docs.example.com/"_string);
    EXPECT_EQ(entries[2].url, "https://beta.example.com/"_string);
}

TEST_CASE(history_autocomplete_trims_whitespace)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("https://ladybird.dev/"sv), "Ladybird"_string, UnixDateTime::from_seconds_since_epoch(10));

    auto entries = store->autocomplete_entries("  ladybird  "sv, 8);

    VERIFY(entries.size() == 1);
    EXPECT_EQ(entries[0].url, "https://ladybird.dev/"_string);
}

TEST_CASE(history_autocomplete_ignores_www_prefix_for_host_matches)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("https://www.google.com/"sv), "Google"_string, UnixDateTime::from_seconds_since_epoch(20));
    store->record_visit(parse_url("https://www.goodreads.com/"sv), "Goodreads"_string, UnixDateTime::from_seconds_since_epoch(10));

    auto entries = store->autocomplete_entries("goo"sv, 8);

    VERIFY(entries.size() == 2);
    EXPECT_EQ(entries[0].url, "https://www.google.com/"_string);
    EXPECT_EQ(entries[1].url, "https://www.goodreads.com/"_string);
}

TEST_CASE(history_autocomplete_ignores_scheme_and_www_boilerplate_prefixes)
{
    auto store = WebView::HistoryStore::create();
    expect_history_autocomplete_ignores_url_boilerplate(*store);
}

TEST_CASE(history_autocomplete_requires_three_characters_for_title_matches)
{
    auto store = WebView::HistoryStore::create();
    expect_history_autocomplete_requires_three_characters_for_title_matches(*store);
}

TEST_CASE(history_autocomplete_requires_three_characters_for_non_prefix_url_matches)
{
    auto store = WebView::HistoryStore::create();
    expect_history_autocomplete_requires_three_characters_for_non_prefix_url_matches(*store);
}

TEST_CASE(history_favicon_updates_entry)
{
    auto store = WebView::HistoryStore::create();
    auto url = parse_url("https://ladybird.dev/"sv);

    store->record_visit(url, "Ladybird"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->update_favicon(url, "Zm9v"_string);

    auto entry = store->entry_for_url(url);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->favicon_base64_png, Optional<String> { "Zm9v"_string });
}

TEST_CASE(history_autocomplete_entries_include_metadata)
{
    auto store = WebView::HistoryStore::create();
    expect_history_autocomplete_entries_include_metadata(*store);
}

TEST_CASE(non_browsable_urls_are_not_recorded)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("about:blank"sv));
    store->record_visit(parse_url("data:text/plain,hello"sv));

    EXPECT(!store->entry_for_url(parse_url("about:blank"sv)).has_value());
    EXPECT(!store->entry_for_url(parse_url("data:text/plain,hello"sv)).has_value());
}

TEST_CASE(disabled_history_store_ignores_updates)
{
    auto store = WebView::HistoryStore::create_disabled();
    auto url = parse_url("https://example.com/"sv);

    store->record_visit(url, "Example"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->update_title(url, "Example title"_string);
    store->remove_entries_accessed_since(UnixDateTime::from_seconds_since_epoch(0));
    store->clear();

    EXPECT(!store->entry_for_url(url).has_value());
    EXPECT(store->autocomplete_entries("example"sv, 8).is_empty());
}

TEST_CASE(history_entries_accessed_since_can_be_removed)
{
    auto store = WebView::HistoryStore::create();

    auto older_url = parse_url("https://older.example.com/"sv);
    auto newer_url = parse_url("https://newer.example.com/"sv);

    store->record_visit(older_url, "Older"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->record_visit(newer_url, "Newer"_string, UnixDateTime::from_seconds_since_epoch(20));

    store->remove_entries_accessed_since(UnixDateTime::from_seconds_since_epoch(15));

    EXPECT(store->entry_for_url(older_url).has_value());
    EXPECT(!store->entry_for_url(newer_url).has_value());
}

TEST_CASE(persisted_history_survives_reopen)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        store->record_visit(parse_url("https://persist.example.com/"sv), "Persisted title"_string, UnixDateTime::from_seconds_since_epoch(77));
        store->update_favicon(parse_url("https://persist.example.com/"sv), "Zm9v"_string);
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

        auto entry = store->entry_for_url(parse_url("https://persist.example.com/"sv));
        VERIFY(entry.has_value());

        EXPECT_EQ(entry->title, Optional<String> { "Persisted title"_string });
        EXPECT_EQ(entry->visit_count, 1u);
        EXPECT_EQ(entry->last_visited_time, UnixDateTime::from_seconds_since_epoch(77));
        EXPECT_EQ(entry->favicon_base64_png, Optional<String> { "Zm9v"_string });
    }
}

TEST_CASE(persisted_history_entries_accessed_since_can_be_removed)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-remove-since-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    auto older_url = parse_url("https://older.example.com/"sv);
    auto newer_url = parse_url("https://newer.example.com/"sv);

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        store->record_visit(older_url, "Older"_string, UnixDateTime::from_seconds_since_epoch(10));
        store->record_visit(newer_url, "Newer"_string, UnixDateTime::from_seconds_since_epoch(20));
        store->remove_entries_accessed_since(UnixDateTime::from_seconds_since_epoch(15));
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

        EXPECT(store->entry_for_url(older_url).has_value());
        EXPECT(!store->entry_for_url(newer_url).has_value());
    }
}

TEST_CASE(persisted_history_autocomplete_ignores_scheme_and_www_boilerplate_prefixes)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-autocomplete-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
    auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

    expect_history_autocomplete_ignores_url_boilerplate(*store);
}

TEST_CASE(persisted_history_autocomplete_requires_three_characters_for_title_matches)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-title-autocomplete-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
    auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

    expect_history_autocomplete_requires_three_characters_for_title_matches(*store);
}

TEST_CASE(persisted_history_autocomplete_requires_three_characters_for_non_prefix_url_matches)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-url-autocomplete-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
    auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

    expect_history_autocomplete_requires_three_characters_for_non_prefix_url_matches(*store);
}

TEST_CASE(persisted_history_autocomplete_entries_include_metadata)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-entry-autocomplete-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
    auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

    expect_history_autocomplete_entries_include_metadata(*store);
}
