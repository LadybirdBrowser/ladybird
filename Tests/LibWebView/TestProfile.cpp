/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/Profile.h>
#include <LibWebView/Settings.h>
#include <LibWebView/StorageJar.h>

static ByteString test_root()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-profile-{}", Core::System::getpid())).string();
}

static WebView::ProfileRoots roots()
{
    auto root = test_root();
    return {
        .config = LexicalPath::join(root, "config-root"sv).string(),
        .data = LexicalPath::join(root, "data-root"sv).string(),
        .cache = LexicalPath::join(root, "cache-root"sv).string(),
        .runtime = LexicalPath::join(root, "runtime-root"sv).string(),
        .temporary = LexicalPath::join(root, "temporary-root"sv).string(),
    };
}

static void remove_test_root()
{
    if (FileSystem::exists(test_root()))
        MUST(FileSystem::remove(test_root(), FileSystem::RecursionMode::Allowed));
}

static URL::URL parse_url(StringView url)
{
    return URL::Parser::basic_parse(url).release_value();
}

TEST_CASE(profile_names)
{
    EXPECT(WebView::Profile::is_valid_name("default"sv));
    EXPECT(WebView::Profile::is_valid_name("release-2_0"sv));
    EXPECT(WebView::Profile::is_valid_name("legacy"sv));
    EXPECT(!WebView::Profile::is_valid_name({}));
    EXPECT(!WebView::Profile::is_valid_name("Default"sv));
    EXPECT(!WebView::Profile::is_valid_name("has.dot"sv));
    EXPECT(!WebView::Profile::is_valid_name("has/slash"sv));
}

TEST_CASE(routing_identity)
{
    EXPECT_EQ(WebView::Profile::routing_identifier("/profiles/default"sv), WebView::Profile::routing_identifier("/profiles/default"sv));
    EXPECT_NE(WebView::Profile::routing_identifier("/profiles/default"sv), WebView::Profile::routing_identifier("/profiles/work"sv));
    EXPECT_EQ(WebView::Profile::routing_identifier("/profiles/default"sv).length(), 32u);
}

TEST_CASE(named_and_default_layouts)
{
    remove_test_root();
    auto profile_roots = roots();

    auto named = MUST(WebView::Profile::create({ .name = "work" }, profile_roots));
    EXPECT_EQ(named.identifier(), "work"sv);
    EXPECT_EQ(named.paths().config, LexicalPath::join(profile_roots.config, "Ladybird/Profiles/work"sv).string());
    EXPECT_EQ(named.paths().data, LexicalPath::join(profile_roots.data, "Ladybird/Profiles/work"sv).string());
    EXPECT_EQ(named.paths().cache, LexicalPath::join(profile_roots.cache, "Ladybird/Profiles/work"sv).string());
    EXPECT_EQ(named.paths().runtime, LexicalPath::join(profile_roots.runtime, "Ladybird/Profiles/work"sv).string());

    auto default_profile = MUST(WebView::Profile::create({}, profile_roots));
    EXPECT_EQ(default_profile.identifier(), "default"sv);
    EXPECT_EQ(default_profile.paths().config, LexicalPath::join(profile_roots.config, "Ladybird/Profiles/default"sv).string());
    EXPECT_EQ(default_profile.paths().data, LexicalPath::join(profile_roots.data, "Ladybird/Profiles/default"sv).string());
    EXPECT_EQ(default_profile.paths().cache, LexicalPath::join(profile_roots.cache, "Ladybird/Profiles/default"sv).string());
    EXPECT_EQ(default_profile.paths().runtime, LexicalPath::join(profile_roots.runtime, "Ladybird/Profiles/default"sv).string());

    auto explicit_default_profile = MUST(WebView::Profile::create({ .name = "default" }, profile_roots));
    EXPECT_EQ(explicit_default_profile.paths().identity, default_profile.paths().identity);
    remove_test_root();
}

TEST_CASE(custom_layout)
{
    remove_test_root();
    auto root = LexicalPath::join(test_root(), "custom"sv).string();
    auto profile = MUST(WebView::Profile::create({ .path = root }, roots()));
    auto canonical_root = MUST(FileSystem::real_path(root));
    EXPECT_EQ(profile.paths().config, LexicalPath::join(canonical_root, "config"sv).string());
    EXPECT_EQ(profile.paths().data, LexicalPath::join(canonical_root, "data"sv).string());
    EXPECT_EQ(profile.paths().cache, LexicalPath::join(canonical_root, "cache"sv).string());
    EXPECT_EQ(profile.paths().runtime, LexicalPath::join(canonical_root, "runtime"sv).string());
    EXPECT_EQ(profile.paths().identity, canonical_root);
    remove_test_root();
}

TEST_CASE(legacy_layout)
{
    remove_test_root();
    auto profile_roots = roots();
    auto profile = MUST(WebView::Profile::create_legacy(profile_roots));
    EXPECT_EQ(profile.identifier(), "legacy"sv);
    EXPECT_EQ(profile.paths().config, LexicalPath::join(profile_roots.config, "Ladybird"sv).string());
    EXPECT_EQ(profile.paths().data, LexicalPath::join(profile_roots.data, "Ladybird"sv).string());
    EXPECT_EQ(profile.paths().cache, LexicalPath::join(profile_roots.cache, "Ladybird"sv).string());
    EXPECT_EQ(profile.paths().runtime, profile_roots.runtime);
    remove_test_root();
}

TEST_CASE(invalid_selection)
{
    remove_test_root();
    EXPECT(WebView::Profile::create({ .name = "UPPER" }, roots()).is_error());
    EXPECT(WebView::Profile::create({ .path = "relative/path" }, roots()).is_error());
    EXPECT(WebView::Profile::create({ .name = "one", .temporary = true }, roots()).is_error());
    EXPECT(WebView::Profile::create({ .name = "one", .path = "/absolute" }, roots()).is_error());
    remove_test_root();
}

TEST_CASE(temporary_profiles_are_unique_and_removed)
{
    remove_test_root();
    ByteString first_root;
    ByteString second_root;
    {
        auto first = MUST(WebView::Profile::create({ .temporary = true }, roots()));
        auto second = MUST(WebView::Profile::create({ .temporary = true }, roots()));
        first_root = first.paths().identity;
        second_root = second.paths().identity;
        EXPECT_NE(first_root, second_root);
        EXPECT(FileSystem::is_directory(first.paths().config));
        EXPECT(FileSystem::is_directory(second.paths().data));
    }
    EXPECT(!FileSystem::exists(first_root));
    EXPECT(!FileSystem::exists(second_root));
    remove_test_root();
}

TEST_CASE(profile_settings_and_bookmarks_are_isolated)
{
    remove_test_root();
    auto first_profile = MUST(WebView::Profile::create({ .name = "first" }, roots()));
    auto second_profile = MUST(WebView::Profile::create({ .name = "second" }, roots()));

    auto first_settings_path = LexicalPath::join(first_profile.paths().config, "Settings.json"sv).string();
    auto second_settings_path = LexicalPath::join(second_profile.paths().config, "Settings.json"sv).string();
    auto first_settings = WebView::Settings::create(first_settings_path);
    first_settings.set_show_menu_bar(true);
    auto second_settings = WebView::Settings::create(second_settings_path);
    EXPECT(!second_settings.show_menu_bar());
    EXPECT(WebView::Settings::create(first_settings_path).show_menu_bar());

    auto first_bookmarks_path = LexicalPath::join(first_profile.paths().config, "Bookmarks.json"sv).string();
    auto second_bookmarks_path = LexicalPath::join(second_profile.paths().config, "Bookmarks.json"sv).string();
    auto url = URL::Parser::basic_parse("https://profile-isolation.example/"sv).release_value();
    auto first_bookmarks = WebView::BookmarkStore::create(first_bookmarks_path);
    first_bookmarks.add_bookmark(url, {}, {});
    auto second_bookmarks = WebView::BookmarkStore::create(second_bookmarks_path);
    EXPECT(!second_bookmarks.is_bookmarked(url));
    EXPECT(WebView::BookmarkStore::create(first_bookmarks_path).is_bookmarked(url));
    remove_test_root();
}

TEST_CASE(profile_databases_are_isolated)
{
    remove_test_root();
    auto first_profile = MUST(WebView::Profile::create({ .name = "first" }, roots()));
    auto second_profile = MUST(WebView::Profile::create({ .name = "second" }, roots()));
    auto url = parse_url("https://profile-isolation.example/"sv);

    {
        auto database = TRY_OR_FAIL(Database::Database::create(first_profile.paths().data, "Ladybird"sv));
        EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);
        EXPECT_EQ(TRY_OR_FAIL(WebView::HSTSStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
        EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

        auto cookie_jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));
        HTTP::Cookie::ParsedCookie cookie {
            .name = "profile"_string,
            .value = "first"_string,
            .expiry_time_from_expires_attribute = UnixDateTime::now() + AK::Duration::from_seconds(3600),
        };
        cookie_jar->set_cookie(url, cookie, HTTP::Cookie::Source::Http);

        auto hsts_store = TRY_OR_FAIL(WebView::HSTSStore::create(*database));
        hsts_store->store_policy("profile-isolation.example"_string, HTTP::HSTS::ParsedHSTSPolicy { AK::Duration::from_seconds(3600), false });

        auto storage_jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
        storage_jar->set_item(WebView::StorageEndpointType::LocalStorage, "https://profile-isolation.example"_string, "profile"_utf16, "first"_utf16);
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(second_profile.paths().data, "Ladybird"sv));
        EXPECT_EQ(TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*database)), Database::MigrationOutcome::Success);
        EXPECT_EQ(TRY_OR_FAIL(WebView::HSTSStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
        EXPECT_EQ(TRY_OR_FAIL(WebView::StorageJar::migrate_schema(*database)), Database::MigrationOutcome::Success);

        auto cookie_jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));
        EXPECT(cookie_jar->get_all_cookies().is_empty());
        auto hsts_store = TRY_OR_FAIL(WebView::HSTSStore::create(*database));
        EXPECT(!hsts_store->is_known_hsts_host("profile-isolation.example"sv));
        auto storage_jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
        EXPECT(!storage_jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://profile-isolation.example"_string, "profile"_utf16).has_value());
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(first_profile.paths().data, "Ladybird"sv));
        auto cookie_jar = TRY_OR_FAIL(WebView::CookieJar::create(*database));
        EXPECT_EQ(cookie_jar->get_all_cookies().size(), 1uz);
        auto hsts_store = TRY_OR_FAIL(WebView::HSTSStore::create(*database));
        EXPECT(hsts_store->is_known_hsts_host("profile-isolation.example"sv));
        auto storage_jar = TRY_OR_FAIL(WebView::StorageJar::create(*database));
        EXPECT_EQ(storage_jar->get_item(WebView::StorageEndpointType::LocalStorage, "https://profile-isolation.example"_string, "profile"_utf16), Optional<Utf16String> { "first"_utf16 });
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(first_profile.paths().data, "History"sv));
        EXPECT_EQ(TRY_OR_FAIL(WebView::HistoryStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
        auto history_store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        history_store->record_visit(url, "First profile"_string, UnixDateTime::from_seconds_since_epoch(1));
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(second_profile.paths().data, "History"sv));
        EXPECT_EQ(TRY_OR_FAIL(WebView::HistoryStore::migrate_schema(*database)), Database::MigrationOutcome::Success);
        auto history_store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        EXPECT(!history_store->entry_for_url(url).has_value());
    }

    {
        auto database = TRY_OR_FAIL(Database::Database::create(first_profile.paths().data, "History"sv));
        auto history_store = TRY_OR_FAIL(WebView::HistoryStore::create(*database));
        EXPECT(history_store->entry_for_url(url).has_value());
    }

    remove_test_root();
}

TEST_CASE(profile_caches_are_isolated)
{
    remove_test_root();
    auto first_profile = MUST(WebView::Profile::create({ .name = "first" }, roots()));
    auto second_profile = MUST(WebView::Profile::create({ .name = "second" }, roots()));
    auto url = parse_url("https://profile-isolation.example/script.js"sv);
    auto request_headers = HTTP::HeaderList::create({});
    auto bytecode = TRY_OR_FAIL(ByteBuffer::copy("first-profile-bytecode"sv.bytes()));

    {
        auto cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Normal, LexicalPath { first_profile.paths().cache })).release_value();
        EXPECT(TRY_OR_FAIL(cache.create_synthetic_entry(url, "GET"sv)));
        EXPECT(TRY_OR_FAIL(cache.store_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode, bytecode.bytes())));
    }

    {
        auto cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Normal, LexicalPath { second_profile.paths().cache })).release_value();
        EXPECT(!TRY_OR_FAIL(cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode)).has_value());
    }

    {
        auto cache = TRY_OR_FAIL(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Normal, LexicalPath { first_profile.paths().cache })).release_value();
        auto retrieved_bytecode = TRY_OR_FAIL(cache.retrieve_associated_data(url, "GET"sv, *request_headers, {}, HTTP::CacheEntryAssociatedData::JavaScriptBytecode));
        VERIFY(retrieved_bytecode.has_value());
        EXPECT_EQ(retrieved_bytecode->bytes(), bytecode.bytes());
    }

    remove_test_root();
}
