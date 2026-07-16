/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Profile.h>
#include <LibWebView/SessionStore.h>

static ByteString test_root()
{
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-session-store-{}", Core::System::getpid())).string();
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

TEST_CASE(missing_session_file_loads_as_empty)
{
    remove_test_root();
    auto profile = MUST(WebView::Profile::create({ .name = "default" }, roots()));

    auto session = TRY_OR_FAIL(WebView::load_session(profile));
    EXPECT(!session.has_value());

    remove_test_root();
}

TEST_CASE(save_and_load_round_trips_profile_session)
{
    remove_test_root();
    auto profile = MUST(WebView::Profile::create({ .name = "default" }, roots()));

    WebView::Session session;
    session.active_window_index = 1;
    session.windows.append({
        .tabs = { { parse_url("https://example.com/"sv) }, { parse_url("about:settings"sv) } },
        .active_tab_index = 1,
        .x = 10,
        .y = 20,
        .width = 1024,
        .height = 768,
        .maximized = true,
    });
    session.windows.append({
        .tabs = { { parse_url("https://ladybird.org/"sv) } },
        .active_tab_index = 0,
        .x = 30,
        .y = 40,
        .width = 800,
        .height = 600,
    });

    TRY_OR_FAIL(WebView::save_session(profile, session));

    auto loaded = TRY_OR_FAIL(WebView::load_session(profile));
    VERIFY(loaded.has_value());
    EXPECT_EQ(loaded->active_window_index, 1u);
    EXPECT_EQ(loaded->windows.size(), 2u);
    EXPECT_EQ(loaded->windows[0].active_tab_index, 1u);
    EXPECT_EQ(loaded->windows[0].x, 10);
    EXPECT_EQ(loaded->windows[0].y, 20);
    EXPECT_EQ(loaded->windows[0].width, 1024);
    EXPECT_EQ(loaded->windows[0].height, 768);
    EXPECT(loaded->windows[0].maximized);
    EXPECT_EQ(loaded->windows[0].tabs.size(), 2u);
    EXPECT_EQ(loaded->windows[0].tabs[0].url.serialize(), "https://example.com/"sv);
    EXPECT_EQ(loaded->windows[0].tabs[1].url.serialize(), "about:settings"sv);
    EXPECT_EQ(loaded->windows[1].x, 30);
    EXPECT_EQ(loaded->windows[1].y, 40);
    EXPECT_EQ(loaded->windows[1].width, 800);
    EXPECT_EQ(loaded->windows[1].height, 600);
    EXPECT(!loaded->windows[1].maximized);
    EXPECT_EQ(loaded->windows[1].tabs[0].url.serialize(), "https://ladybird.org/"sv);

    remove_test_root();
}

TEST_CASE(negative_active_indices_default_to_zero)
{
    remove_test_root();
    auto profile = MUST(WebView::Profile::create({ .name = "default" }, roots()));

    MUST(Core::Directory::create(profile.paths().data, Core::Directory::CreateDirectories::Yes, 0700));
    auto file = MUST(Core::File::open(WebView::session_store_path(profile), Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(R"json({
        "version": 2,
        "activeWindowIndex": -1,
        "windows": [{
            "activeTabIndex": -1,
            "tabs": [{ "url": "https://example.com/" }]
        }]
    })json"sv));

    auto session = TRY_OR_FAIL(WebView::load_session(profile));
    VERIFY(session.has_value());
    EXPECT_EQ(session->active_window_index, 0u);
    EXPECT_EQ(session->windows[0].active_tab_index, 0u);

    remove_test_root();
}

TEST_CASE(corrupt_session_file_loads_as_empty)
{
    remove_test_root();
    auto profile = MUST(WebView::Profile::create({ .name = "default" }, roots()));

    MUST(Core::Directory::create(profile.paths().data, Core::Directory::CreateDirectories::Yes, 0700));
    auto file = MUST(Core::File::open(WebView::session_store_path(profile), Core::File::OpenMode::Write));
    MUST(file->write_until_depleted("not json"sv.bytes()));

    auto session = TRY_OR_FAIL(WebView::load_session(profile));
    EXPECT(!session.has_value());

    remove_test_root();
}

TEST_CASE(empty_session_is_cleared)
{
    remove_test_root();
    auto profile = MUST(WebView::Profile::create({ .name = "default" }, roots()));

    WebView::Session session;
    session.windows.append({ .tabs = { { parse_url("https://example.com/"sv) } } });
    TRY_OR_FAIL(WebView::save_session(profile, session));
    EXPECT(FileSystem::exists(WebView::session_store_path(profile)));

    TRY_OR_FAIL(WebView::save_session(profile, {}));
    EXPECT(!FileSystem::exists(WebView::session_store_path(profile)));

    remove_test_root();
}

TEST_CASE(session_files_are_profile_specific)
{
    remove_test_root();
    auto first_profile = MUST(WebView::Profile::create({ .name = "first" }, roots()));
    auto second_profile = MUST(WebView::Profile::create({ .name = "second" }, roots()));

    WebView::Session session;
    session.windows.append({ .tabs = { { parse_url("https://first.example/"sv) } } });
    TRY_OR_FAIL(WebView::save_session(first_profile, session));

    auto first_session = TRY_OR_FAIL(WebView::load_session(first_profile));
    auto second_session = TRY_OR_FAIL(WebView::load_session(second_profile));
    EXPECT(first_session.has_value());
    EXPECT(!second_session.has_value());
    EXPECT_NE(WebView::session_store_path(first_profile), WebView::session_store_path(second_profile));

    remove_test_root();
}
