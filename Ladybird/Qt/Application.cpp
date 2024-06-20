/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Application.h"
#include "StringUtils.h"
#include "TaskManagerWindow.h"
#include <LibWebView/URL.h>
#include <QFileOpenEvent>

namespace Ladybird {

Application::Application(int& argc, char** argv)
    : QApplication(argc, argv)
{
}

Application::~Application()
{
    close_task_manager_window();
}

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

void Application::show_task_manager_window()
{
    if (!m_task_manager_window) {
        m_task_manager_window = new TaskManagerWindow(nullptr);
    }
    m_task_manager_window->show();
    m_task_manager_window->activateWindow();
    m_task_manager_window->raise();
}

void Application::close_task_manager_window()
{
    if (m_task_manager_window) {
        m_task_manager_window->close();
        delete m_task_manager_window;
        m_task_manager_window = nullptr;
    }
}

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls, WebView::CookieJar& cookie_jar, WebContentOptions const& web_content_options, StringView webdriver_content_ipc_path, bool allow_popups, Tab* parent_tab, Optional<u64> page_index)
{
    auto* window = new BrowserWindow(initial_urls, cookie_jar, web_content_options, webdriver_content_ipc_path, allow_popups, parent_tab, move(page_index));
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

}
