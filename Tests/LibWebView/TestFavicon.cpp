/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

TEST_CASE(favicon)
{
    Test::HeadlessTestEnvironment environment { "TestFavicon"sv, 120 };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    {
        bool favicon_changed = false;

        view->on_favicon_change = [&](Optional<Gfx::Bitmap const&> bitmap) {
            if (bitmap.has_value()) {
                VERIFY(bitmap->width() == 32);
                VERIFY(bitmap->height() == 32);
                VERIFY(bitmap->get_pixel(0, 0) == Gfx::Color::from_named_css_color_string("lime"sv));
                favicon_changed = true;
            }
        };

        view->load_html(R"(<!doctype html>
            <html>
                <link
                    rel="icon"
                    href='data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" version="1.0"><rect x="0" y="0" width="32" height="32" fill="lime"/></svg>'
                />
            </html>
        )"sv);

        Core::EventLoop::current().spin_until([&]() { return favicon_changed; });
    }

    {
        bool favicon_changed = false;
        Gfx::Color expected_color = Gfx::Color::Blue;

        view->on_favicon_change = [&](Optional<Gfx::Bitmap const&> bitmap) {
            if (bitmap.has_value()) {
                VERIFY(bitmap->width() == 32);
                VERIFY(bitmap->height() == 32);
                VERIFY(bitmap->get_pixel(0, 0) == expected_color);
                favicon_changed = true;
            }
        };

        view->load_html(R"(<!doctype html>
            <link rel="icon" href='data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><rect width="32" height="32" fill="blue"/></svg>'>
            <link id="preferred" rel="icon">
        )"sv);

        Core::EventLoop::current().spin_until([&]() { return favicon_changed; });

        for (auto invalid_icon : { "data:image/png,invalid"sv, "data:image/svg+xml,invalid"sv }) {
            expected_color = Gfx::Color::from_named_css_color_string("lime"sv).value();
            favicon_changed = false;
            view->run_javascript(R"(
                document.getElementById("preferred").href = 'data:image/svg+xml,<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><rect width="32" height="32" fill="lime"/></svg>';
            )"_string);
            Core::EventLoop::current().spin_until([&]() { return favicon_changed; });

            // A failed replacement must discard this icon's cached bitmap and if it was the preferred icon, select the next most appropriate one.
            expected_color = Gfx::Color::Blue;
            favicon_changed = false;
            view->run_javascript(MUST(String::formatted("document.getElementById('preferred').href = '{}';", invalid_icon)));
            Core::EventLoop::current().spin_until([&]() { return favicon_changed; });
        }
    }

    {
        bool favicon_changed = false;

        view->on_favicon_change = [&](Optional<Gfx::Bitmap const&> bitmap) {
            if (bitmap.has_value()) {
                VERIFY(bitmap->width() == 32);
                VERIFY(bitmap->height() == 32);
                VERIFY(bitmap->get_pixel(0, 0) == Gfx::Color::DarkGreen);
                favicon_changed = true;
            }
        };

        // Favicon with a red background and a dark green PNG. We should wait until the PNG is loaded before rasterizing the favicon.
        view->load_html(R"(<!doctype html>
            <html>
                <link
                    rel="icon"
                    href='data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" version="1.0" style="background-color: red"><image width="32" height="32" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAAD0lEQVR4AQEEAPv/AACAAAEEAIEu/TP9AAAAAElFTkSuQmCC"/></svg>'
                />
            </html>
        )"sv);

        Core::EventLoop::current().spin_until([&]() { return favicon_changed; });
    }
}
