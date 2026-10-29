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
#include <LibWebView/Utilities.h>
#include <stdlib.h>

namespace {

void click(WebView::ViewImplementation& view)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = { 10, 10 },
        .screen_position = { 10, 10 },
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = { 10, 10 },
        .screen_position = { 10, 10 },
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::None,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

}

TEST_CASE(before_unload)
{
    Test::HeadlessTestEnvironment environment { "TestBeforeUnload"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished == 1; });

    auto source_url = URL::Parser::basic_parse("data:text/html,<title>source</title><button style='width:100px;height:100px' onclick=\"document.title='activated'\">Activate</button><script>onbeforeunload=e=>e.preventDefault()</script>"sv).release_value();
    auto destination_url = URL::Parser::basic_parse("data:text/html,<title>destination</title>Destination"sv).release_value();

    auto view_is_at = [&](URL::URL const& url) { return view->url().serialize() == url.serialize(); };
    auto load_and_wait = [&](URL::URL const& url) {
        auto expected_loads_finished = loads_finished + 1;
        view->load(url);
        Core::EventLoop::current().spin_until([&] { return loads_finished >= expected_loads_finished && view_is_at(url); });
    };
    auto activate_page = [&] {
        bool activated = false;
        view->on_title_change = [&](auto const& title) { activated = title == "activated"sv; };
        click(*view);
        Core::EventLoop::current().spin_until([&] { return activated; });
        view->on_title_change = {};
    };

    // Frontends without a beforeunload handler must act as if the user chose Leave.
    load_and_wait(source_url);
    activate_page();
    load_and_wait(destination_url);

    size_t prompt_count = 0;
    bool accept_prompt = false;
    bool adopt_navigation_prompt_for_close = false;
    Optional<bool> adopted_close_preflight_result;
    Optional<String> prompt_source;
    view->on_request_before_unload = [&](auto const& source, auto on_complete) {
        ++prompt_count;
        prompt_source = source;
        if (adopt_navigation_prompt_for_close) {
            view->request_close_preflight([&](bool approved) {
                adopted_close_preflight_result = approved;
            });
        }
        on_complete(accept_prompt);
    };

    // A cancellation request without sticky activation must not show a prompt.
    load_and_wait(source_url);
    load_and_wait(destination_url);
    VERIFY(prompt_count == 0);

    // Stay cancels the navigation. The opaque data: origin uses the scheme fallback.
    load_and_wait(source_url);
    activate_page();
    auto loads_finished_before_cancellation = loads_finished;
    view->load(destination_url);
    Core::EventLoop::current().spin_until([&] { return loads_finished > loads_finished_before_cancellation; });
    VERIFY(prompt_count == 1);
    VERIFY(prompt_source == "data://"sv);
    VERIFY(view_is_at(source_url));

    // A subsequent navigation may ask once again, and Leave allows it to proceed.
    accept_prompt = true;
    load_and_wait(destination_url);
    VERIFY(prompt_count == 2);

    auto request_close_preflight = [&] {
        Optional<bool> result;
        view->request_close_preflight([&](bool approved) {
            result = approved;
        });
        Core::EventLoop::current().spin_until([&] { return result.has_value(); });
        return *result;
    };

    // A close preflight asks without closing the page.
    load_and_wait(source_url);
    activate_page();
    accept_prompt = false;
    VERIFY(!request_close_preflight());
    VERIFY(view_is_at(source_url));
    VERIFY(prompt_count == 3);

    accept_prompt = true;
    VERIFY(request_close_preflight());
    VERIFY(view_is_at(source_url));
    VERIFY(prompt_count == 4);

    // A close preflight adopts an already-open navigation prompt instead of showing another one.
    adopt_navigation_prompt_for_close = true;
    accept_prompt = false;
    auto prompt_count_before_adoption = prompt_count;
    view->load(destination_url);
    Core::EventLoop::current().spin_until([&] { return adopted_close_preflight_result.has_value(); });
    VERIFY(!*adopted_close_preflight_result);
    VERIFY(view_is_at(source_url));
    VERIFY(prompt_count == prompt_count_before_adoption + 1);

    adopted_close_preflight_result.clear();
    accept_prompt = true;
    prompt_count_before_adoption = prompt_count;
    view->load(destination_url);
    Core::EventLoop::current().spin_until([&] { return adopted_close_preflight_result.has_value(); });
    VERIFY(*adopted_close_preflight_result);
    VERIFY(view_is_at(source_url));
    VERIFY(prompt_count == prompt_count_before_adoption + 1);

    bool closed = false;
    view->on_close = [&] { closed = true; };
    auto close_without_prompting = view->prepare_for_close_without_prompting();
    close_without_prompting();
    Core::EventLoop::current().spin_until([&] { return closed; });

    outln("PASS: beforeunload prompt handling");
}
