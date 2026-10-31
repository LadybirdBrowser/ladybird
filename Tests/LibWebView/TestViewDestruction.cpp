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
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>

// A frontend destroys its widgets before the view they hold: the view's teardown, which closes its page, must not call
// back into the frontend.
TEST_CASE(a_view_being_destroyed_does_not_report_its_audibility)
{
    Test::HeadlessTestEnvironment environment { "TestViewDestruction"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    OwnPtr<WebView::HeadlessWebView> view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished == 1; });

    Optional<Audio::Audibility> reported_audibility;
    view->on_audibility_changed = [&](auto audibility) { reported_audibility = audibility; };
    view->traversable().for_each_hosting_page([](WebView::WebContentPage& page) {
        page.set_audibility(Audio::Audibility::Audible);
    });
    EXPECT(reported_audibility == Audio::Audibility::Audible);

    reported_audibility.clear();
    view = nullptr;
    EXPECT(!reported_audibility.has_value());
}
