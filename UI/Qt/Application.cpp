/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QFileDialog>
#include <QFileOpenEvent>

namespace Ladybird {

Application::Application(Badge<WebView::Application>, Main::Arguments& arguments)
    : QApplication(arguments.argc, arguments.argv)
{
}

void Application::create_platform_options(WebView::BrowserOptions&, WebView::WebContentOptions& web_content_options)
{
    web_content_options.config_path = Settings::the()->directory();
}

Application::~Application() = default;

bool Application::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::FileOpen: {
        if (!on_open_file)
            break;

        auto const& open_event = *static_cast<QFileOpenEvent const*>(event);
        auto file = ak_string_from_qstring(open_event.file());

        if (auto file_url = WebView::sanitize_url(file); file_url.has_value())
            on_open_file(file_url.release_value());
        break;
    }
    default:
        break;
    }

    return QApplication::event(event);
}

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls, BrowserWindow::IsPopupWindow is_popup_window, Tab* parent_tab, Optional<u64> page_index)
{
    auto* window = new BrowserWindow(initial_urls, is_popup_window, parent_tab, move(page_index));
    set_active_window(*window);
    window->show();
    if (initial_urls.is_empty()) {
        auto* tab = window->current_tab();
        if (tab) {
            tab->set_url_is_hidden(true);
            tab->focus_location_editor();
        }
    }
    window->activateWindow();
    window->raise();
    return *window;
}

Optional<ByteString> Application::ask_user_for_download_folder() const
{
    auto path = QFileDialog::getExistingDirectory(nullptr, "Select download directory", QDir::homePath());
    if (path.isNull())
        return {};

    return ak_byte_string_from_qstring(path);
}

}
