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

    auto suggestions = store->autocomplete_suggestions("alpha"sv, 8);

    VERIFY(suggestions.size() == 3);
    EXPECT_EQ(suggestions[0], "https://alpha.example.com/"_string);
    EXPECT_EQ(suggestions[1], "https://docs.example.com/"_string);
    EXPECT_EQ(suggestions[2], "https://beta.example.com/"_string);
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
    EXPECT(store->autocomplete_suggestions("example"sv, 8).is_empty());
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
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(database_directory, "HistoryStore"sv));
        auto store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));

        auto entry = store->entry_for_url(parse_url("https://persist.example.com/"sv));
        VERIFY(entry.has_value());

        EXPECT_EQ(entry->title, Optional<String> { "Persisted title"_string });
        EXPECT_EQ(entry->visit_count, 1u);
        EXPECT_EQ(entry->last_visited_time, UnixDateTime::from_seconds_since_epoch(77));
    }
}
