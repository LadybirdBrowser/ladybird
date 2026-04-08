/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <UI/Gtk/Application.h>
#include <UI/Gtk/BrowserWindow.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto app = TRY(Ladybird::Application::create(arguments));

    if (auto const& browser_options = Ladybird::Application::browser_options(); !browser_options.headless_mode.has_value()) {
        // Single-instance is handled via D-Bus in Application::create_platform_event_loop().
        // If this is a remote instance, it already forwarded URLs and exited.
        auto& window = app->new_window(browser_options.urls);
        (void)window;
    }

    return app->execute();
}
