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

TEST_CASE(recently_closed_entries_are_reopened_in_lifo_order)
{
    auto store = WebView::HistoryStore::create();
    auto first_url = parse_url("https://first.example.com/"sv);
    auto second_url = parse_url("https://second.example.com/"sv);
    auto third_url = parse_url("https://third.example.com/"sv);

    EXPECT(!store->has_recently_closed_entries());

    store->record_closed_tab(first_url, UnixDateTime::from_seconds_since_epoch(10));
    store->record_closed_window({ second_url, third_url }, 1, UnixDateTime::from_seconds_since_epoch(20));

    EXPECT(store->has_recently_closed_entries());

    auto recently_closed_entry = store->pop_most_recently_closed_entry();
    VERIFY(recently_closed_entry.has_value());
    EXPECT(recently_closed_entry->was_window);
    EXPECT_EQ(recently_closed_entry->active_tab_index, 1u);
    EXPECT_EQ(recently_closed_entry->urls.size(), 2u);
    EXPECT_EQ(recently_closed_entry->urls[0], second_url);
    EXPECT_EQ(recently_closed_entry->urls[1], third_url);
    EXPECT(store->has_recently_closed_entries());

    recently_closed_entry = store->pop_most_recently_closed_entry();
    VERIFY(recently_closed_entry.has_value());
    EXPECT(!recently_closed_entry->was_window);
    EXPECT_EQ(recently_closed_entry->active_tab_index, 0u);
    EXPECT_EQ(recently_closed_entry->urls.size(), 1u);
    EXPECT_EQ(recently_closed_entry->urls[0], first_url);
    EXPECT(!store->has_recently_closed_entries());

    EXPECT(!store->pop_most_recently_closed_entry().has_value());
}

TEST_CASE(recently_closed_entries_accessed_since_can_be_removed)
{
    auto store = WebView::HistoryStore::create();
    auto older_url = parse_url("https://older.example.com/"sv);
    auto newer_url = parse_url("https://newer.example.com/"sv);
    auto newest_url = parse_url("https://newest.example.com/"sv);

    store->record_closed_tab(older_url, UnixDateTime::from_seconds_since_epoch(10));
    store->record_closed_window({ newer_url, newest_url }, 0, UnixDateTime::from_seconds_since_epoch(20));

    store->remove_entries_accessed_since(UnixDateTime::from_seconds_since_epoch(15));

    EXPECT(store->has_recently_closed_entries());

    auto recently_closed_entry = store->pop_most_recently_closed_entry();
    VERIFY(recently_closed_entry.has_value());
    EXPECT_EQ(recently_closed_entry->urls.size(), 1u);
    EXPECT_EQ(recently_closed_entry->urls[0], older_url);
    EXPECT(!store->has_recently_closed_entries());
}

TEST_CASE(recently_closed_entries_can_be_peeked_without_popping)
{
    auto store = WebView::HistoryStore::create();
    auto first_url = parse_url("https://peek.example.com/"sv);
    auto second_url = parse_url("https://peek-two.example.com/"sv);

    store->record_closed_window({ first_url, second_url }, 0, UnixDateTime::from_seconds_since_epoch(10));

    auto recently_closed_entry = store->most_recently_closed_entry();
    VERIFY(recently_closed_entry.has_value());
    EXPECT(recently_closed_entry->was_window);
    EXPECT_EQ(recently_closed_entry->urls.size(), 2u);
    EXPECT_EQ(recently_closed_entry->urls[0], first_url);
    EXPECT_EQ(recently_closed_entry->urls[1], second_url);
    EXPECT(store->has_recently_closed_entries());
}

TEST_CASE(recently_closed_entries_are_cleared_with_history)
{
    auto store = WebView::HistoryStore::create();
    auto first_url = parse_url("https://clear.example.com/"sv);
    auto second_url = parse_url("https://clear-two.example.com/"sv);

    store->record_closed_tab(first_url, UnixDateTime::from_seconds_since_epoch(10));
    store->record_closed_window({ second_url }, 0, UnixDateTime::from_seconds_since_epoch(20));

    store->clear();

    EXPECT(!store->has_recently_closed_entries());
    EXPECT(!store->most_recently_closed_entry().has_value());
    EXPECT(!store->pop_most_recently_closed_entry().has_value());
}

TEST_CASE(history_autocomplete_entries_include_metadata)
{
    auto store = WebView::HistoryStore::create();
    expect_history_autocomplete_entries_include_metadata(*store);
}

TEST_CASE(remove_entry_by_url)
{
    auto store = WebView::HistoryStore::create();
    auto keep_url = parse_url("https://keep.example.com/"sv);
    auto remove_url = parse_url("https://remove.example.com/"sv);

    store->record_visit(keep_url, "Keep"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->record_visit(remove_url, "Remove"_string, UnixDateTime::from_seconds_since_epoch(20));

    store->remove_entry_by_url("https://remove.example.com/"_string);

    EXPECT(store->entry_for_url(keep_url).has_value());
    EXPECT(!store->entry_for_url(remove_url).has_value());
}

TEST_CASE(list_all_entries)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("https://alpha.example.com/"sv), "Alpha"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->record_visit(parse_url("https://beta.example.com/"sv), "Beta"_string, UnixDateTime::from_seconds_since_epoch(20));
    store->record_visit(parse_url("https://gamma.example.com/"sv), "Gamma"_string, UnixDateTime::from_seconds_since_epoch(30));

    auto entries = store->list_all_entries();
    EXPECT_EQ(entries.size(), 3u);

    bool found_alpha = false;
    bool found_beta = false;
    bool found_gamma = false;
    for (auto const& entry : entries) {
        if (entry.url == "https://alpha.example.com/"_string)
            found_alpha = true;
        else if (entry.url == "https://beta.example.com/"_string)
            found_beta = true;
        else if (entry.url == "https://gamma.example.com/"_string)
            found_gamma = true;
    }
    EXPECT(found_alpha);
    EXPECT(found_beta);
    EXPECT(found_gamma);
}

TEST_CASE(update_title_updates_existing_entry)
{
    auto store = WebView::HistoryStore::create();
    auto url = parse_url("https://example.com/"sv);

    store->record_visit(url, "Original"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->update_title(url, "Updated"_string);

    auto entry = store->entry_for_url(url);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->title, Optional<String> { "Updated"_string });
}

TEST_CASE(update_title_ignores_empty_string)
{
    auto store = WebView::HistoryStore::create();
    auto url = parse_url("https://example.com/"sv);

    store->record_visit(url, "Original"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->update_title(url, String {});

    auto entry = store->entry_for_url(url);
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->title, Optional<String> { "Original"_string });
}

TEST_CASE(update_title_ignores_nonexistent_entry)
{
    auto store = WebView::HistoryStore::create();
    auto url = parse_url("https://nonexistent.example.com/"sv);

    store->update_title(url, "Title"_string);

    EXPECT(!store->entry_for_url(url).has_value());
}

TEST_CASE(update_favicon_ignores_nonexistent_entry)
{
    auto store = WebView::HistoryStore::create();
    auto url = parse_url("https://nonexistent.example.com/"sv);

    store->update_favicon(url, "Zm9v"_string);

    EXPECT(!store->entry_for_url(url).has_value());
}

TEST_CASE(record_closed_window_ignores_empty_urls)
{
    auto store = WebView::HistoryStore::create();

    store->record_closed_window({}, 0, UnixDateTime::from_seconds_since_epoch(10));

    EXPECT(!store->has_recently_closed_entries());
}

TEST_CASE(record_closed_window_clamps_active_tab_index)
{
    auto store = WebView::HistoryStore::create();
    auto first_url = parse_url("https://first.example.com/"sv);
    auto second_url = parse_url("https://second.example.com/"sv);

    store->record_closed_window({ first_url, second_url }, 99, UnixDateTime::from_seconds_since_epoch(10));

    auto entry = store->pop_most_recently_closed_entry();
    VERIFY(entry.has_value());
    EXPECT_EQ(entry->active_tab_index, 1u);
}

TEST_CASE(autocomplete_returns_empty_for_blank_query)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("https://example.com/"sv), "Example"_string, UnixDateTime::from_seconds_since_epoch(10));

    EXPECT(store->autocomplete_entries(""sv, 8).is_empty());
    EXPECT(store->autocomplete_entries("   "sv, 8).is_empty());
}

TEST_CASE(autocomplete_respects_limit)
{
    auto store = WebView::HistoryStore::create();

    store->record_visit(parse_url("https://test1.example.com/"sv), "Test 1"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->record_visit(parse_url("https://test2.example.com/"sv), "Test 2"_string, UnixDateTime::from_seconds_since_epoch(20));
    store->record_visit(parse_url("https://test3.example.com/"sv), "Test 3"_string, UnixDateTime::from_seconds_since_epoch(30));

    auto entries = store->autocomplete_entries("test"sv, 2);
    EXPECT_EQ(entries.size(), 2u);
}

TEST_CASE(disabled_store_ignores_list_remove_and_favicon)
{
    auto store = WebView::HistoryStore::create_disabled();
    auto url = parse_url("https://example.com/"sv);

    store->record_visit(url, "Example"_string, UnixDateTime::from_seconds_since_epoch(10));
    store->update_favicon(url, "Zm9v"_string);

    EXPECT(store->list_all_entries().is_empty());

    store->remove_entry_by_url("https://example.com/"_string);
    EXPECT(store->list_all_entries().is_empty());
}

TEST_CASE(persisted_remove_entry_by_url)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-remove-by-url-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    auto keep_url = parse_url("https://keep.example.com/"sv);
    auto remove_url = parse_url("https://remove.example.com/"sv);

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        store->record_visit(keep_url, "Keep"_string, UnixDateTime::from_seconds_since_epoch(10));
        store->record_visit(remove_url, "Remove"_string, UnixDateTime::from_seconds_since_epoch(20));
        store->remove_entry_by_url("https://remove.example.com/"_string);
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

        EXPECT(store->entry_for_url(keep_url).has_value());
        EXPECT(!store->entry_for_url(remove_url).has_value());
    }
}

TEST_CASE(persisted_list_all_entries)
{
    auto database_directory = ByteString::formatted(
        "{}/ladybird-history-store-list-all-test-{}",
        Core::StandardPaths::tempfile_directory(),
        generate_random_uuid());
    TRY_OR_FAIL(Core::Directory::create(database_directory, Core::Directory::CreateDirectories::Yes));

    auto cleanup = ScopeGuard([&] {
        MUST(FileSystem::remove(database_directory, FileSystem::RecursionMode::Allowed));
    });

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        store->record_visit(parse_url("https://alpha.example.com/"sv), "Alpha"_string, UnixDateTime::from_seconds_since_epoch(10));
        store->record_visit(parse_url("https://beta.example.com/"sv), "Beta"_string, UnixDateTime::from_seconds_since_epoch(20));
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

        auto entries = store->list_all_entries();
        EXPECT_EQ(entries.size(), 2u);

        // Persisted list_all_entries orders by last_visited_time DESC.
        EXPECT_EQ(entries[0].url, "https://beta.example.com/"_string);
        EXPECT_EQ(entries[1].url, "https://alpha.example.com/"_string);
    }
}

TEST_CASE(disabled_history_store_ignores_updates)
{
    auto store = WebView::HistoryStore::create_disabled();
    auto url = parse_url("https://example.com/"sv);

    auto check_is_empty = [&] {
        EXPECT(!store->entry_for_url(url).has_value());
        EXPECT(store->autocomplete_entries("example"sv, 8).is_empty());
    };

    store->record_visit(url, "Example"_string, UnixDateTime::from_seconds_since_epoch(10));
    check_is_empty();

    store->update_title(url, "Example title"_string);
    check_is_empty();

    store->remove_entries_accessed_since(UnixDateTime::from_seconds_since_epoch(0));
    check_is_empty();

    store->clear();
    check_is_empty();
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
