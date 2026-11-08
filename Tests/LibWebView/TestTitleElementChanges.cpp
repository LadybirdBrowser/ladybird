/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

// The view shows the document's title after a title element is replaced or removed, not the title from before.
TEST_CASE(title_element_replacement_and_removal)
{
    Test::HeadlessTestEnvironment environment { "TestTitleElementChanges"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    Utf16String title;
    view->on_title_change = [&](Utf16String const& new_title) { title = new_title; };

    view->load_html(R"html(<!DOCTYPE html><title>TITLE_A</title>)html"sv);
    Core::EventLoop::current().spin_until([&]() { return title == "TITLE_A"sv; });

    view->run_javascript(R"js(
        const replacement = document.createElement("title");
        replacement.textContent = "TITLE_B";
        document.querySelector("title").replaceWith(replacement);
    )js"_string);
    Core::EventLoop::current().spin_until([&]() { return title == "TITLE_B"sv; });

    // A document without a title element has an empty title, which the view shows as the page's URL.
    view->run_javascript(R"js(document.querySelector("title").remove();)js"_string);
    auto url = Utf16String::from_utf8(view->url().serialize());
    Core::EventLoop::current().spin_until([&]() { return title == url; });

    view->run_javascript(R"js(
        const title = document.createElement("title");
        title.textContent = "TITLE_C";
        document.head.append(title);
    )js"_string);
    Core::EventLoop::current().spin_until([&]() { return title == "TITLE_C"sv; });
}
