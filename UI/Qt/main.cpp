/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Settings.h>

#include <QCoreApplication>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#    include <QStyleHints>
#endif

namespace Ladybird {

// FIXME: Find a place to put this declaration (and other helper functions).
bool is_using_dark_system_theme(QWidget&);
bool is_using_dark_system_theme(QWidget& widget)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Use the explicitly set or system default color scheme whenever available
    auto color_scheme = QGuiApplication::styleHints()->colorScheme();
    if (color_scheme != Qt::ColorScheme::Unknown)
        return color_scheme == Qt::ColorScheme::Dark;
#endif

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
    WebView::BrowserProcess browser_process;

    WebView::copy_default_config_files(Ladybird::Settings::the()->directory());

    if (auto const& browser_options = Ladybird::Application::browser_options(); !browser_options.headless_mode.has_value()) {
        if (browser_options.force_new_process == WebView::ForceNewProcess::No) {
            auto disposition = TRY(browser_process.connect(browser_options.raw_urls, browser_options.new_window));

            if (disposition == WebView::BrowserProcess::ProcessDisposition::ExitProcess) {
                outln("Opening in existing process");
                return 0;
            }
        }

        app->on_open_file = [&](auto const& file_url) {
            auto& window = app->active_window();
            window.view().load(file_url);
        };

        browser_process.on_new_tab = [&](auto const& urls) {
            auto& window = app->active_window();
            for (size_t i = 0; i < urls.size(); ++i) {
                window.new_tab_from_url(urls[i], (i == 0) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No);
            }
            window.show();
            window.activateWindow();
            window.raise();
        };

        browser_process.on_new_window = [&](auto const& urls) {
            auto const& previous_active_window = app->active_window();
            Ladybird::WindowConfiguration configuration {
                .width = previous_active_window.width(),
                .height = previous_active_window.height(),
                .maximized = previous_active_window.isMaximized(),
            };
            app->new_window(urls, configuration);
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

        Vector<WebView::SessionWindow> session_windows;
        auto store = WebView::Application::session_store();
        if (store.has_value())
            session_windows = store->load_session();

        if (session_windows.is_empty()) {
            auto& window = app->new_window(browser_options.urls, configuration);
            window.setWindowTitle("Ladybird");
        } else {
            bool any_window_created = false;
            for (auto& session_window : session_windows) {
                Vector<URL::URL> urls;
                for (auto& tab : session_window.tabs) {
                    if (auto url = URL::Parser::basic_parse(tab.url); url.has_value())
                        urls.append(url.release_value());
                }
                if (urls.is_empty())
                    continue;

                Ladybird::WindowConfiguration window_config {
                    .x = session_window.x.has_value() ? Optional<Web::DevicePixels>(*session_window.x) : Optional<Web::DevicePixels>(),
                    .y = session_window.y.has_value() ? Optional<Web::DevicePixels>(*session_window.y) : Optional<Web::DevicePixels>(),
                    .width = session_window.width.has_value() ? Optional<Web::DevicePixels>(*session_window.width) : configuration.width,
                    .height = session_window.height.has_value() ? Optional<Web::DevicePixels>(*session_window.height) : configuration.height,
                    .maximized = session_window.maximized,
                    .session_window_id = session_window.id,
                };
                auto& window = app->new_window(urls, window_config);
                window.activate_tab(static_cast<int>(session_window.active_tab_index));
                window.setWindowTitle("Ladybird");
                any_window_created = true;
            }

            if (!any_window_created) {
                auto& window = app->new_window(browser_options.urls, configuration);
                window.setWindowTitle("Ladybird");
            }
        }
    }

    return app->execute();
}
