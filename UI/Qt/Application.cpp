/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibWebView/EventLoop/EventLoopImplementationQt.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QFileDialog>
#include <QFileOpenEvent>

namespace Ladybird {

class LadybirdQApplication : public QApplication {
public:
    explicit LadybirdQApplication(Main::Arguments& arguments)
        : QApplication(arguments.argc, arguments.argv)
    {
    }

    virtual bool event(QEvent* event) override
    {
        auto& application = static_cast<Application&>(WebView::Application::the());

        switch (event->type()) {
        case QEvent::FileOpen: {
            if (!application.on_open_file)
                break;

            auto const& open_event = *static_cast<QFileOpenEvent const*>(event);
            auto file = ak_string_from_qstring(open_event.file());

            if (auto file_url = WebView::sanitize_url(file); file_url.has_value())
                application.on_open_file(file_url.release_value());
            break;
        }

        default:
            break;
        }

        return QApplication::event(event);
    }
};

Application::Application() = default;
Application::~Application() = default;

void Application::create_platform_options(WebView::BrowserOptions&, WebView::WebContentOptions& web_content_options)
{
    web_content_options.config_path = Settings::the()->directory();
}

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        m_application = make<LadybirdQApplication>(arguments());
        Core::EventLoopManager::install(*new WebView::EventLoopManagerQt);
    }

    auto event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<WebView::EventLoopImplementationQt&>(event_loop->impl()).set_main_loop();

    return event_loop;
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
