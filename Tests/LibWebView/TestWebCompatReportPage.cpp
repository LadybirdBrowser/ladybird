/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebCompatReport.h>

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

// A web compatibility report describes the page as the view shows it, and dumps how the page was laid out and painted.
// Pages that are not websites cannot be reported.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestWebCompatReportPage-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

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
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    view->load_html("<!DOCTYPE html><div style=\"width: 100px; height: 50px; background: red\">Broken</div>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    auto page = WebView::WebCompatReport::describe_page(*view);
    VERIFY(page.url == view->url());
    VERIFY(page.viewport_size == Gfx::IntSize(800, 600));
    VERIFY(page.device_pixel_ratio == 1.);
    VERIFY(page.zoom_level == 1.);
    VERIFY(!page.user_agent.is_empty());

    VERIFY(!view->can_report_website_issue());
    auto requests = 0;
    view->on_request_website_issue_report = [&] { ++requests; };
    view->report_website_issue();
    VERIFY(requests == 0);

    auto dumps = TRY(WebView::WebCompatReport::request_page_dumps(*view)->await());
    auto layout_tree = dumps.find_byte_offset("Viewport <#document>"sv);
    auto stacking_context_tree = dumps.find_byte_offset("\nSC for Viewport"sv);
    auto display_list = dumps.find_byte_offset("\nDisplayList:\n"sv);
    VERIFY(layout_tree == 0u);
    VERIFY(stacking_context_tree.has_value() && *stacking_context_tree > 0);
    VERIFY(display_list.has_value() && *display_list > *stacking_context_tree);
    VERIFY(dumps.bytes_as_string_view().substring_view(0, *stacking_context_tree).contains("Broken"sv));
    VERIFY(dumps.bytes_as_string_view().substring_view(*display_list).contains("FillRect"sv));

    outln("PASS: Web compatibility reports describe and dump the page the view shows");
    return 0;
}
