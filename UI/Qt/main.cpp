/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Settings.h>

#include <QCoreApplication>
#include <QStyleHints>
#include <QtGlobal>

#if defined(AK_OS_MACOS)
#    include <UI/AppKit/Utilities/ApplicationIcon.h>
#    include <UI/Qt/MacWindow.h>
#endif

namespace Ladybird {

// FIXME: Find a place to put this declaration (and other helper functions).
bool is_using_dark_system_theme(QWidget&);
bool is_using_dark_system_theme(QWidget& widget)
{
    // Use the explicitly set or system default color scheme whenever available
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme != Qt::ColorScheme::Unknown)
        return color_scheme == Qt::ColorScheme::Dark;

    // Calculate luma based on Rec. 709 coefficients
    // https://en.wikipedia.org/wiki/Rec._709#Luma_coefficients
    auto color = widget.palette().color(widget.backgroundRole());
    auto luma = 0.2126f * color.redF() + 0.7152f * color.greenF() + 0.0722f * color.blueF();
    return luma <= 0.5f;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

#ifdef AK_OS_MACOS
    // The web content view is a native QRhiWidget child. Keep it from forcing
    // every sibling in the tab UI to become native as well.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#endif

    auto app = TRY(Ladybird::Application::create(arguments));
    if (app->should_exit_after_profile_coordination()) {
        outln("Opening in existing process");
        return 0;
    }

    app->initialize_macos_application_menu();
    auto& browser_process = app->browser_process();

    if (auto const& browser_options = Ladybird::Application::browser_options(); !browser_options.headless_mode.has_value()) {
#if defined(AK_OS_MACOS)
        Ladybird::set_profile_application_icon(Ladybird::Application::profile());
#endif

        app->on_open_file = [&](auto const& file_url) {
            if (auto* window = app->non_private_window_if_any()) {
                auto& tab = window->new_tab_from_url(file_url, Web::HTML::ActivateTab::Yes, Ladybird::BrowserWindow::TabLocation::end());
                tab.set_url_is_hidden(false);
                auto& view = tab.view();
#if defined(AK_OS_MACOS)
                Ladybird::make_appkit_window_first_responder(view);
#endif
                view.setFocus();
                window->show();
                window->activateWindow();
                window->raise();
                return;
            }
            app->new_window({ file_url });
        };

        browser_process.on_new_tab = [&](auto const& urls) {
            if (!app->active_window_if_any()) {
                app->new_window(urls);
                return;
            }

            auto& window = *app->active_window_if_any();
            for (size_t i = 0; i < urls.size(); ++i) {
                window.new_tab_from_url(urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, Ladybird::BrowserWindow::TabLocation::end());
            }
            window.show();
            window.activateWindow();
            window.raise();
        };

        browser_process.on_new_window = [&](auto const& urls) {
            app->new_window(urls, app->configuration_for_new_window());
        };

        auto last_size = Ladybird::Settings::the()->last_size();
        Ladybird::WindowConfiguration configuration {
            .width = last_size.width(),
            .height = last_size.height(),
            .maximized = Ladybird::Settings::the()->is_maximized(),
        };
        if (auto last_position = Ladybird::Settings::the()->last_position(); last_position.has_value()) {
            configuration.x = last_position->x();
            configuration.y = last_position->y();
        }
        auto& window = app->new_window(browser_options.urls, configuration);
        window.setWindowTitle("Ladybird");
    }

    return app->execute();
}
