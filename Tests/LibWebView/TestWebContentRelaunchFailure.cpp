/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

// When a tab's WebContent process dies and no new one can be launched, only that tab goes: It's closed, and the browser
// keeps running.

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
        // NB: Without a headless mode, a crashed tab is recovered as a browser window's tab is, rather than restored.
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }
};

}

TEST_CASE(a_tab_whose_process_cannot_be_replaced_is_closed)
{
    Test::HeadlessTestEnvironment environment { "TestWebContentRelaunchFailure"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 1; });
    bool closed = false;
    view->on_close = [&] { closed = true; };

    // With no helper process to find, the replacement process can't be launched, as when the system is out of memory.
    WebView::set_helper_process_directory_for_testing("/nonexistent"sv);
    view->debug_request("crash-current-page"sv);
    Core::EventLoop::current().spin_until([&] { return closed; });
    WebView::set_helper_process_directory_for_testing({});

    outln("PASS: a tab whose WebContent process cannot be replaced is closed, and the browser keeps running");
}
