/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

// Browser-UI traversals resolve their target at their queue position, so a second Forward requested while one is
// pending must select the entry after the first Forward's target instead of resolving both requests against the
// same starting position.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestBrowserHistoryTraversal-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    VERIFY(setenv("XDG_CONFIG_HOME", test_config_directory.characters(), 1) == 0);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    size_t expected_loads = 1;
    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= expected_loads; });

    auto url_a = URL::Parser::basic_parse("data:text/html,<title>A</title>first"sv).release_value();
    auto url_b = URL::Parser::basic_parse("data:text/html,<title>B</title>second"sv).release_value();
    auto url_c = URL::Parser::basic_parse("data:text/html,<title>C</title>third"sv).release_value();
    auto url_d = URL::Parser::basic_parse("data:text/html,<title>D</title>fourth"sv).release_value();

    auto view_is_at = [&](URL::URL const& url) { return view->url().serialize() == url.serialize(); };
    // Waiting for the load count as well as the URL keeps the next step from racing the destination document.
    auto wait_until_at = [&](URL::URL const& url, size_t new_loads = 1) {
        expected_loads += new_loads;
        Core::EventLoop::current().spin_until([&]() { return loads_finished >= expected_loads && view_is_at(url); });
    };

    view->load(url_a);
    wait_until_at(url_a);
    view->load(url_b);
    wait_until_at(url_b);
    view->load(url_c);
    wait_until_at(url_c);
    view->load(url_d);
    wait_until_at(url_d);

    view->traverse_the_history_by_delta(-1);
    wait_until_at(url_c);
    view->traverse_the_history_by_delta(-1);
    wait_until_at(url_b);
    view->traverse_the_history_by_delta(-1);
    wait_until_at(url_a);

    // The Back at the start of history has no entry to select. The two Forwards queued behind it must still
    // compose, the second selecting the entry after the first Forward's target.
    view->traverse_the_history_by_delta(-1);
    view->traverse_the_history_by_delta(1);
    view->traverse_the_history_by_delta(1);
    wait_until_at(url_c, 2);

    view->traverse_the_history_by_delta(1);
    wait_until_at(url_d);

    outln("PASS: browser history traversal");
    return 0;
}
