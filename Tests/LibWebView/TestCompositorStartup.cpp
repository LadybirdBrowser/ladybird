/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
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
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        // Outside test mode, the Compositor is launched from what the system probe reports.
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

}

TEST_CASE(a_tab_opened_before_the_compositor_launches_paints_once_it_does)
{
    Test::HeadlessTestEnvironment environment { "TestCompositorStartup"sv };
    // A temporary profile has no facts cached by an earlier probe, so the Compositor waits for this one.
    Array arguments { "--temporary-profile"sv };
    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>(arguments));
    EXPECT(!app->has_compositor_process());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    EXPECT(!app->has_compositor_process());

    bool painted = false;
    view->on_ready_to_paint = [&] { painted = true; };
    view->load(URL::Parser::basic_parse("data:text/html,<p>painted"sv).release_value());
    Core::EventLoop::current().spin_until([&] { return painted; });
    view->on_ready_to_paint = nullptr;

    EXPECT(app->has_compositor_process());
}

TEST_CASE(a_video_that_starts_before_the_compositor_launches_plays_once_it_does)
{
    Test::HeadlessTestEnvironment environment { "TestCompositorStartup"sv };
    Array arguments { "--temporary-profile"sv, "--enable-autoplay"sv };
    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>(arguments));

    // The tests run from this directory.
    auto video_path = TRY_OR_FAIL(FileSystem::real_path("../LibWeb/Assets/blue-with-second-in-gray-64x36.webm"sv));
    auto page_path = LexicalPath::join(environment.directory(), "video.html"sv).string();
    auto page = TRY_OR_FAIL(Core::File::open(page_path, Core::File::OpenMode::Write));
    TRY_OR_FAIL(page->write_formatted("<video autoplay muted src=\"{}\" oncanplay=\"document.title = 'canplay'\"></video>", URL::create_with_file_scheme(video_path)->serialize()));
    page->close();

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    bool can_play = false;
    view->on_title_change = [&](auto const& title) { can_play = title == "canplay"sv; };
    view->load(URL::create_with_file_scheme(page_path).release_value());
    EXPECT(!app->has_compositor_process());
    Core::EventLoop::current().spin_until([&] { return can_play; });
    view->on_title_change = nullptr;
}
