/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebCompatReport.h>

// A web compatibility report describes the page as the view shows it, and dumps how the page was laid out and painted.
// Pages that are not websites cannot be reported.

TEST_CASE(web_compat_report_page)
{
    Test::HeadlessTestEnvironment environment { "TestWebCompatReportPage"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

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

    auto dumps = TRY_OR_FAIL(WebView::WebCompatReport::request_page_dumps(*view)->await());
    auto layout_tree = dumps.find_byte_offset("Viewport <#document>"sv);
    auto stacking_context_tree = dumps.find_byte_offset("\nSC for Viewport"sv);
    auto display_list = dumps.find_byte_offset("\nDisplayList:\n"sv);
    VERIFY(layout_tree == 0u);
    VERIFY(stacking_context_tree.has_value() && *stacking_context_tree > 0);
    VERIFY(display_list.has_value() && *display_list > *stacking_context_tree);
    VERIFY(dumps.bytes_as_string_view().substring_view(0, *stacking_context_tree).contains("Broken"sv));
    VERIFY(dumps.bytes_as_string_view().substring_view(*display_list).contains("FillRect"sv));

    outln("PASS: Web compatibility reports describe and dump the page the view shows");
}
