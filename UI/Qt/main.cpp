/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/BrowserVersion.h>
#include <LibCore/GeolocationProvider.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/SystemAppearance.h>
#if defined(LADYBIRD_QT_HAVE_POSITIONING)
#    include <UI/Qt/GeolocationProviderQt.h>
#endif
#include <UI/Qt/WebContentView.h>

#include <QCoreApplication>
#include <QStyleHints>
#include <QtGlobal>

#if defined(AK_OS_MACOS)
#    include <QColorSpace>
#    include <QSurfaceFormat>
#    include <UI/Qt/MacWindow.h>
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
    // Headless mode has no use for a UI, so the Hub takes over this process.
    if (any_of(arguments.strings, [](auto argument) { return argument.starts_with("--headless"sv); })) {
        auto hub_path = TRY(Ladybird::Application::hub_executable_path());
        execv(hub_path.characters(), arguments.argv);
        return Error::from_syscall("execv"sv, errno);
    }
#endif

    // The web content view is presented in a native child window. Without this attribute, a widget
    // gaining a native window would force every other widget in the browser window to become native
    // as well.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

#ifdef AK_OS_MACOS
    auto surface_format = QSurfaceFormat::defaultFormat();
    surface_format.setColorSpace(QColorSpace::SRgb);
    QSurfaceFormat::setDefaultFormat(surface_format);
#endif

#if defined(LADYBIRD_QT_HAVE_POSITIONING)
    // Only fall back to Qt on platforms that have no geolocation provider of their own.
    if (!Core::GeolocationProvider::is_available())
        Ladybird::install_qt_geolocation_provider();
#endif

    auto app = TRY(Ladybird::Application::create(arguments));
    if (auto exit_status = app->exit_status_if_done(); exit_status.has_value())
        return *exit_status;

    Ladybird::ChromeStyle::install_application_style();

    if (auto const& browser_options = Ladybird::Application::browser_options(); !browser_options.headless_mode.has_value()) {
        Ladybird::SystemAppearance::the().apply_interface_theme(Ladybird::Application::settings().appearance().interface_theme);
        Ladybird::SystemAppearance::the().apply_color_theme(Ladybird::Application::settings().appearance().color_theme);

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

        // NB: URLs from outside the browser, such as a link clicked in another app, never open in a private window.
        app->on_new_tab = [&](auto const& urls) {
            auto* non_private_window = app->non_private_window_if_any();
            if (!non_private_window) {
                app->new_window(urls);
                return;
            }

            auto& window = *non_private_window;
            for (size_t i = 0; i < urls.size(); ++i) {
                window.new_tab_from_url(urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, Ladybird::BrowserWindow::TabLocation::end());
            }
            window.show();
            window.activateWindow();
            window.raise();
        };

        app->on_new_window = [&](auto const& urls) {
            app->new_window(urls, app->configuration_for_new_window());
        };

        auto configuration = app->configuration_for_new_window();
        if (auto* restored_window = app->restore_previous_session(configuration)) {
            // A whole restored session replaces the usual startup tab, so only the URLs the user passed open with it.
            // With only pinned tabs restored, the usual startup tabs open after them.
            auto restored_everything = app->launch_restore_policy() == Ladybird::SessionLaunchPolicy::Everything;
            if (!restored_everything || !browser_options.raw_urls.is_empty())
                app->open_startup_tabs(*restored_window, browser_options.urls);
            app->offer_to_restore_unexpectedly_closed_tabs(*restored_window);
        } else {
            auto& window = app->new_window(browser_options.urls, configuration);
            window.setWindowTitle(BROWSER_PRODUCT_NAME);
            app->offer_to_restore_unexpectedly_closed_tabs(window);
        }
    }

    // NB: Messages to the Hub arrive in order, so this comes after whatever these windows have asked to show.
    if (auto* hub = Ladybird::HubConnection::the_if_connected())
        hub->async_ui_did_start();

    return app->execute();
}
