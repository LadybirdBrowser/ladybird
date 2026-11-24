/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }
};

}

// A file page navigates to a file in another folder than the one the user opened.
TEST_CASE(local_file_navigation)
{
    Test::HeadlessTestEnvironment environment { "TestLocalFileNavigation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto directory = TRY_OR_FAIL(FileSystem::real_path(environment.directory()));
    auto write_page = [&](ByteString const& folder, StringView title) {
        MUST(Core::Directory::create(folder, Core::Directory::CreateDirectories::Yes));
        auto path = LexicalPath::join(folder, ByteString::formatted("{}.html", title)).string();
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(ByteString::formatted("<title>{}</title>", title)));
        return URL::create_with_file_scheme(path).release_value();
    };
    auto start_page_url = write_page(LexicalPath::join(directory, "inside"sv).string(), "start"sv);
    auto outside_page_url = write_page(LexicalPath::join(directory, "outside"sv).string(), "elsewhere"sv);

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Utf16String title;
    view->on_title_change = [&](auto const& new_title) { title = new_title; };
    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    auto view_is_at = [&](URL::URL const& url) { return view->url().serialize() == url.serialize(); };
    auto wait_for_load_of = [&](URL::URL const& url) {
        auto expected_loads = loads_finished + 1;
        Core::EventLoop::current().spin_until([&]() { return loads_finished >= expected_loads && view_is_at(url); });
    };

    view->load_from_browser_ui(start_page_url);
    wait_for_load_of(start_page_url);
    VERIFY(title == "start"sv);

    view->run_javascript(MUST(String::formatted("location.href = '{}';", outside_page_url.serialize())));
    wait_for_load_of(outside_page_url);
    VERIFY(title == "elsewhere"sv);
}
