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
#include <LibURL/Parser.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/Utilities.h>

// A frontend can show a context menu in a nested event loop, and destroy the view from inside it, as closing the tab
// while the menu is open does.
TEST_CASE(a_view_destroyed_while_its_context_menu_is_open)
{
    Test::HeadlessTestEnvironment environment { "TestContextMenuLifetime"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    OwnPtr<WebView::HeadlessWebView> view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished == 1; });

    auto url = URL::Parser::basic_parse("data:text/html,<body style='height:100vh'>page"sv).release_value();
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return loads_finished == 2; });

    bool menu_was_shown = false;
    view->page_context_menu().on_activation = [&](Gfx::IntPoint) {
        menu_was_shown = true;
        view = nullptr;
    };

    for (auto type : { Web::MouseEvent::Type::MouseDown, Web::MouseEvent::Type::MouseUp }) {
        view->enqueue_input_event(Web::MouseEvent {
            .type = type,
            .position = { 10, 10 },
            .screen_position = { 10, 10 },
            .button = Web::UIEvents::MouseButton::Secondary,
            .buttons = type == Web::MouseEvent::Type::MouseDown ? Web::UIEvents::MouseButton::Secondary : Web::UIEvents::MouseButton::None,
            .click_count = 1,
            .browser_data = nullptr,
        });
    }
    Core::EventLoop::current().spin_until([&] { return menu_was_shown; });

    EXPECT(!view);
}
